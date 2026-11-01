# HTTP 接口

默认端口 80。除预览帧与片段数据外均为 JSON。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 单文件页面（内嵌，约 19 KB） |
| GET | `/api/status` | 设备状态总览 |
| GET | `/api/config` | 当前配置（页面据此动态渲染） |
| GET | `/api/frame` | 预览帧；预览关闭时返回 `204` |
| POST | `/api/record` | 触发录制 |
| POST | `/api/preview` | 开关预览 |
| GET | `/api/clip/last` | 回放最近一段（回放格式） |
| GET | `/api/groups` | 分组列表（分页/筛选） |
| GET | `/api/group` | 单组数据（回放格式 或 `raw=1` 原始文件） |
| POST | `/api/group/delete` | 批量删组（异步） |
| POST | `/api/group/rescan` | 手动对账 |
| GET | `/api/stats` | 每类组数与字节小计 |

---

## GET `/api/status`

```json
{
  "wifi":   { "connected": true, "ip": "172.19.217.8", "rssi": -27 },
  "cam":    { "w": 96, "h": 96 },
  "sd":     { "mounted": true, "free_kb": 30947328 },
  "rec":    { "state": "idle", "got": 0, "total": 0,
              "last": "data0010", "last_frames": 12, "last_bytes": 110646 },
  "preview":{ "on": true, "fps": 12, "sent": 341 },
  "del":    { "busy": false, "done": 0, "total": 0 },
  "cfg_crc": "23491944",
  "groups": 9,
  "next_id": 11,
  "heap_internal": 154811,
  "uptime_ms": 51823
}
```

`rec.state` 取值：`idle` / `recording` / `writing` / `error`

---

## GET `/api/config`

```json
{
  "cfg_crc": "23491944",
  "types": ["thumbs_up", "fist", "..."],
  "variables": [
    { "name": "light", "max": 3,
      "labels": [ {"code":1,"name":"warm"}, {"code":2,"name":"cool"} ] }
  ]
}
```

> `labels` 在响应里是**数组**而非对象，方便前端遍历（JSON 对象键顺序不可靠）。

---

## GET `/api/frame`

响应 `200`：`8 字节头 + 9216 字节灰度`

```
[0..3] seq (u32 LE)  [4..5] fps (u16)  [6..7] 保留
```

预览关闭时返回 `204 No Content`。

> 页面用 **2 个并发连接**轮询。因为 httpd 的响应分两次 `write()`，受 Nagle + 延迟 ACK 影响，单请求约 120 ms，单通道只有 8 fps。

---

## POST `/api/record`

```json
{ "label": "fist", "frames": 12, "vars": [2, 1, 3, 5] }
```

| 情况 | 返回 |
|---|---|
| 正常 | `202 {"ok":true}` |
| 正在录制 / 正在删除 | `409` |
| 类别不存在 | `400 {"err":"unknown label"}` |
| `vars[i] > max` | `400 {"err":"vars out of range"}` |
| `frames` 非法（0 或 >160） | `400` |

> 录制期间**自动关闭预览**，结束后自动恢复。`preview.sent` 在录制期间不再增长，可据此验证。

---

## GET `/api/groups?label=&offset=&limit=`

```json
{
  "total": 9, "offset": 0, "limit": 50,
  "groups": [
    { "name": "data0001", "label": "fist", "vars": [3,2,3,5],
      "frames": 12, "bytes": 110646, "mtime": 1, "flags": 0 }
  ]
}
```

- `limit` 上限 200
- 数据全来自 RAM 索引，**不碰 SD**

---

## GET `/api/group?name=data0007[&raw=1]`

| 模式 | 返回 |
|---|---|
| 默认 | 回放格式：`8 字节头 + N×9216`（浏览器用，**不含 delta_ms**） |
| `raw=1` | **原始 `.gseq` 文件**（PC 工具用，含完整头与 delta_ms） |

> ⚠️ 两个用途不同：回放只要像素，训练要完整文件。`tools/fetch_dataset.py` 用的是 `raw=1`。

非法组名（不是 `data<数字>`）返回 `400`，不存在返回 `404`。

---

## POST `/api/group/delete`

```json
{ "names": ["data0009", "data0010"] }
```

- 立即返回 `202`，后台逐组 `unlink` + `rmdir`，完成后重写索引
- 进度看 `/api/status` 的 `del.{busy,done,total}`
- 录制中或已有删除任务 → `409`
- 单次最多 32 组

---

## POST `/api/group/rescan`

重新对账（`readdir` + 索引），返回 `{"ok":true,"groups":N}`。

用于：卡在电脑上被改过、或想强制刷新时。

---

## GET `/api/stats`

```json
{ "groups": 9,
  "stats": [ {"label":"fist","count":2,"bytes":221292}, {"label":"circle","count":3,"bytes":331944} ] }
```

按 `config.json` 的 `types` 顺序输出全部类别（含 0 组的）。
