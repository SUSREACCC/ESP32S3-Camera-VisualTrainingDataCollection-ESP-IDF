# ESP-CAM 数据采集终端

> 本模板源自 `D:\WorkSpace\ESP-IDF\ESP32S3camAI`（手势识别项目）的采集子系统，
> 抽出后作为**可复用的通用数据采集工具**。两边共享的 7 个组件与 `main.c` 保持逐字节一致，
> 只有 `sdkconfig`（模板用 `sdkconfig.defaults`）与文档不同。
> 修改公共代码后请同步到两个项目。

> 给 ESP32-S3 + OV2640 用的**网页控制数据采集工具**：手机连上热点打开网页，实时看画面、点按钮录片段、现场浏览/回放/删除，数据落到 SD 卡。
> 为手势识别一类的时序任务准备，也适用于任何需要"短片段 + 时序一致性"的视觉数据集。

---

## 它解决什么问题

传统做法是"写固件定时抓图 → 拔卡 → 拷到 PC → 整理"。这套工具把流程压成：

```
上电 → 手机打开网页 → 摆好动作 → 点录制 → 当场回放检查 → 不合格就删
                                                        ↓
                                            卡上留下结构化的 .gseq 片段
                                                        ↓
                                        tools/fetch_dataset.py 一键拉到 PC
```

**核心设计取舍**（都是踩过坑之后定下来的，详见 `docs/04-踩坑与设计决策.md`）：

| 决定 | 原因 |
|---|---|
| 录**原始连续帧**，不在设备端融合窗口 | 窗口长度/步长/负样本要能在训练侧自由调整 |
| 相机固定 **96×96 灰度 / 25 fps**，运行期绝不重配 | 出流后再写 SCCB 寄存器会被 NACK（详见 docs/04） |
| 每帧记 **delta_ms** | 唯一能发现"帧间隔不是 40 ms"的手段 |
| 录制时**关闭实时图传** | 降功耗峰值、避免取帧与推流抢资源 |
| 卡上 **`directory.txt` 单索引** + 一组一目录 | 列表零 SD I/O；删除只需 unlink+rmdir |
| 类别/变量由 **`config.json`** 驱动 | 换数据集不用重新烧录固件 |

---

## 硬件要求

| 项 | 要求 |
|---|---|
| MCU | ESP32-S3，**必须带 PSRAM**（相机帧缓冲 + 录制缓冲都放这里） |
| 模组 | 推荐 N16R8（16 MB Flash + 8 MB Octal PSRAM） |
| 相机 | OV2640（DVP 并口 + SCCB）；其他传感器需改 `camera_driver` 的引脚与格式 |
| 存储 | microSD，FAT32 |
| 供电 | USB 5 V；注意板载 PTC 保险丝额定（本项目 0.5 A），WiFi 峰值 + 相机 + SD 写可能逼近上限 |

引脚与接线见 `docs/05-移植指南.md`。

---

## 快速开始

### 1. 准备热点

手机开个人热点，**必须是 2.4 GHz**：
- iPhone：打开「最大兼容性」
- Android：热点设置里选「2.4 GHz 频段」

> ESP32-S3 不支持 5 GHz。iPhone 热点长时间无连接会自动关闭，测试时把设置页留在前台。

### 2. 填凭据并编译

```bash
cp main/wifi_credentials.h.example main/wifi_credentials.h
# 编辑 wifi_credentials.h，填入 SSID / 密码

idf.py build        # target 已在 sdkconfig.defaults 里指定为 esp32s3
idf.py -p COM20 flash
```

> 首次构建会从 `sdkconfig.defaults` 自动生成 `sdkconfig`（已实测：清空 build/ 与 sdkconfig 后能一次编译通过）。
> 依赖（esp32-camera / esp_jpeg）已 vendor 到 `components/`，**无需联网**。

### 3. 准备 SD 卡

卡根目录放：

```
project/
├─ config.json      # 类别与变量定义（见 config/config.example.json）
└─ directory.txt    # 可以不存在，固件会自动创建
```

`config.json` 示例见 `config/config.example.json`。**没有 config.json 也能用**，会回落到内置默认（10 类手势 + 4 变量）。

### 4. 打开网页

串口会打印：

```
I (xxxx) APP: >>> 手机浏览器打开: http://172.19.x.x/
```

手机浏览器访问该地址即可。页面功能：

| 区域 | 用途 |
|---|---|
| 预览 | 96×96 灰度实时画面 + 十字/安全框，用于调整机位 |
| 类别 / 变量 | 由 `config.json` 动态生成，显示 `2 · cool` 这样的编码+名称 |
| 每段帧数 | 会话级选项：12 / 24 / 45 / 75 帧，实时换算秒数与文件大小 |
| 录制 | 一键录制，录制期间自动关图传，完成后自动回放 |
| 数据管理 | 分组列表、播放、整组删除、批量操作 |

### 5. 把数据拉到 PC

```bash
python tools/fetch_dataset.py --host 172.19.217.8 --out dataset/
```

产出 `dataset/<label>/dataNNNN.gseq` + `index.csv` + `legend.json`（数字→名称映射）+ `config.json` 副本。

先用 `tools/gseq_dump.py` 检查帧间隔是否干净：

```bash
python tools/gseq_dump.py dataset/fist/data0001.gseq
```

---

## 目录结构

```
espcam-datacollect/
├─ CMakeLists.txt
├─ partitions.csv              app 4 MB
├─ sdkconfig.defaults          关键配置（PSRAM/Flash/相机/FATFS）
├─ main/
│  ├─ main.c                   启动流程 + 取帧泵
│  ├─ CMakeLists.txt
│  └─ wifi_credentials.h.example
├─ components/
│  ├─ camera_driver/           OV2640 封装，统一输出 96×96 灰度
│  ├─ sd_files_driver/         SD 挂载/重挂载/剩余空间
│  ├─ cfg_store/               config.json 解析 + 校验 + CRC32
│  ├─ sd_index/                directory.txt 唯一读写入口
│  ├─ capture_recorder/        环形缓冲 + 录制状态机 + .gseq 写入
│  ├─ web_ui/                  HTTP 服务 + 内嵌单文件页面
│  ├─ wifi_sta/                STA 连接 + 退避重连
│  ├─ esp32-camera/            vendored 依赖
│  └─ esp_jpeg/                vendored 依赖
├─ config/config.example.json
├─ tools/
│  ├─ gseq_dump.py             解析片段、检查帧间隔、导出 PGM
│  ├─ fetch_dataset.py         批量拉取数据集
│  └─ serial_capture.py        复位 + 抓串口日志
└─ docs/
   ├─ 01-系统架构.md
   ├─ 02-配置与数据格式.md
   ├─ 03-HTTP接口.md
   ├─ 04-踩坑与设计决策.md
   └─ 05-移植指南.md
```

---

## 实测指标（ESP32-S3 N16R8 + OV2640）

| 项 | 值 |
|---|---|
| 输出 | 96×96 GRAYSCALE，1 B/px = **9216 B/帧** |
| 帧率 | **25 fps**（明亮场景）；⚠️ 低照度下自动曝光会把它降到 ~18 fps，见 `docs/04` |
| 帧间隔 | 明亮时均值 **40.0 ms**，p95 = 40.01 ms |
| 单段大小 | 12 帧 ≈ 108 KB；75 帧 ≈ 675 KB |
| SD | SDMMC 1-bit @40 MHz，写 2.9 MB/s |
| 固件 | 约 **1.0 MB**（app 分区 4 MB，余量 76%） |
| 内存 | 内部 RAM 余量 ~150 KB；PSRAM 余量 ~6 MB |
| 网页预览 | 11~12 fps（HTTP 轮询双通道） |

---

## 已知限制

1. **片段帧率会随光照变化**——低照度下自动曝光拉长曝光时间，帧率掉到 ~18 fps，帧间隔变成 ~55 ms。此时**所有按 40±4 ms 校验的窗口都会被拒**。采集前请确认预览帧率接近 25。
2. 预览是 HTTP 轮询 + 双通道，多客户端同时打开会成倍占用带宽（只按单客户端优化）。
3. 无 HTTPS，仅适用于可信局域网。
4. `mtime` 字段是**开机计时**（秒），不是墙上时间；跨重启排序不可靠。
5. 一组固定只放一个片段 `clip.gseq`（这样删除无需枚举目录）。
6. **手机浏览器打不开、提示 "Header fields are too long"** → 请求头缓冲太小，见 `docs/04` 第 14 条（已在本模板的 `sdkconfig.defaults` 里修好）。

---

## 相关文档

- 系统怎么搭的、数据怎么流 → [`docs/01-系统架构.md`](docs/01-系统架构.md)
- `config.json` 怎么写、`.gseq` 字节布局 → [`docs/02-配置与数据格式.md`](docs/02-配置与数据格式.md)
- 接口速查 → [`docs/03-HTTP接口.md`](docs/03-HTTP接口.md)
- **踩过的坑与为什么这么设计** → [`docs/04-踩坑与设计决策.md`](docs/04-踩坑与设计决策.md)
- 换板子/换传感器怎么改 → [`docs/05-移植指南.md`](docs/05-移植指南.md)
