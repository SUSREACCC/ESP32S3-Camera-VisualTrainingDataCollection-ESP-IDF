#!/usr/bin/env python3
"""解析 .gseq v3 片段：打印头信息、检查帧间隔、导出 PGM。

用法：
    python gseq_dump.py clip.gseq                  # 只看头与统计
    python gseq_dump.py clip.gseq --pgm out/       # 同时导出帧为 PGM
    python gseq_dump.py clip.gseq --json           # 机器可读输出

.gseq v3 布局（小端）：
    0   4  "GSEQ"
    4   1  version = 3
    5   1  label_len
    6   1  width    7 height   8 fps   9 reserved
    10  2  frame_count (u16)
    12  4  timestamp_ms (u32)
    16  4  config_crc32 (u32)
    20  1  var_count   21 flags
    22  4  var[0..3] (u8)
    26  L  label
    26+L 2N delta_ms[]（相对首帧的毫秒偏移）
    ...    N × width × height 字节灰度
"""

import argparse
import json
import os
import struct
import sys

MAGIC = b"GSEQ"
HDR = 26
FRAME_BYTES = 96 * 96


def parse(path):
    with open(path, "rb") as f:
        raw = f.read()

    if len(raw) < HDR or raw[:4] != MAGIC:
        raise ValueError("不是 .gseq 文件（magic 不匹配）")

    ver = raw[4]
    if ver != 3:
        raise ValueError(f"只支持 v3，当前 version={ver}")

    label_len = raw[5]
    width, height = raw[6], raw[7]
    fps = raw[8]
    n = struct.unpack_from("<H", raw, 10)[0]
    ts = struct.unpack_from("<I", raw, 12)[0]
    cfg_crc = struct.unpack_from("<I", raw, 16)[0]
    var_count = raw[20]
    flags = raw[21]
    vars_ = list(raw[22:26])
    label = raw[26:26 + label_len].decode("utf-8", "replace")

    delta_off = HDR + label_len
    deltas = [struct.unpack_from("<H", raw, delta_off + 2 * i)[0] for i in range(n)]

    data_off = delta_off + 2 * n
    fb = width * height
    frames = [raw[data_off + i * fb: data_off + (i + 1) * fb] for i in range(n)]

    if len(raw) != data_off + n * fb:
        print(f"[warn] 文件长度 {len(raw)} 与期望 {data_off + n * fb} 不符", file=sys.stderr)

    return dict(version=ver, label=label, width=width, height=height, fps=fps,
                frames=n, timestamp_ms=ts, config_crc32=cfg_crc,
                var_count=var_count, vars=vars_, flags=flags,
                deltas=deltas, data=frames)


def interval_stats(deltas):
    iv = [deltas[i] - deltas[i - 1] for i in range(1, len(deltas))]
    if not iv:
        return {}
    ok40 = sum(1 for x in iv if 36 <= x <= 44)
    return {
        "count": len(iv),
        "mean_ms": round(sum(iv) / len(iv), 1),
        "min_ms": min(iv), "max_ms": max(iv),
        "at_40ms": ok40,
        "clean": ok40 == len(iv),
    }


def write_pgm(path, frame, w, h):
    with open(path, "wb") as f:
        f.write(f"P5\n{w} {h}\n255\n".encode())
        f.write(frame)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--pgm", metavar="DIR", help="把每帧导出为 PGM")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    a = ap.parse_args()

    info = parse(a.file)
    st = interval_stats(info["deltas"])

    if a.json:
        out = {k: v for k, v in info.items() if k not in ("deltas", "data")}
        out["intervals"] = st
        print(json.dumps(out, ensure_ascii=False, indent=2))
    else:
        print(f"文件      : {a.file}  ({os.path.getsize(a.file)} B)")
        print(f"版本      : v{info['version']}")
        print(f"类别      : {info['label']}")
        print(f"尺寸      : {info['width']}x{info['height']} @ {info['fps']} fps")
        print(f"帧数      : {info['frames']}")
        print(f"首帧时间  : {info['timestamp_ms']} ms（开机计时）")
        print(f"config crc: {info['config_crc32']:08x}")
        print(f"变量      : {info['vars']}  (var_count={info['var_count']})")
        print(f"flags     : {info['flags']}")
        if st:
            flag = "OK" if st["clean"] else "含跳变"
            print(f"帧间隔    : 均值 {st['mean_ms']} ms  "
                  f"min={st['min_ms']} max={st['max_ms']}  "
                  f"40ms 占比 {st['at_40ms']}/{st['count']}  [{flag}]")

    if a.pgm:
        os.makedirs(a.pgm, exist_ok=True)
        for i, fr in enumerate(info["data"]):
            write_pgm(os.path.join(a.pgm, f"frame_{i:03d}.pgm"),
                      fr, info["width"], info["height"])
        print(f"已导出 {len(info['data'])} 帧到 {a.pgm}/")


if __name__ == "__main__":
    main()
