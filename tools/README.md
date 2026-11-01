# 工具脚本

| 脚本 | 用途 |
|---|---|
| `gseq_dump.py` | 解析 `.gseq` v3：打印头信息、**帧间隔统计**、导出 PGM 帧 |
| `fetch_dataset.py` | 通过 HTTP 把设备上的片段批量拉到 PC，生成 `index.csv` + `legend.json` |
| `serial_capture.py` | 复位设备并抓串口日志（需 pyserial） |

## 示例

```bash
# 看一段的帧间隔是否均匀
python gseq_dump.py data0007.gseq

# 导出前 10 帧为 PGM 用图片查看器看
python gseq_dump.py data0007.gseq --pgm out/

# 批量拉取整个数据集
python fetch_dataset.py --host 172.19.217.8 --out dataset/

# 抓串口（复位后抓 60 秒）
python serial_capture.py COM20 115200 60 boot.log
```

## 依赖

`gseq_dump.py` / `fetch_dataset.py` 只用 Python 3 标准库。
`serial_capture.py` 需要 `pip install pyserial`。
