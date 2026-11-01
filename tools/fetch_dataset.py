#!/usr/bin/env python3
"""从采集终端批量拉取数据集到 PC。

用法：
    python fetch_dataset.py --host 172.19.217.8 --out dataset/
    python fetch_dataset.py --host 172.19.217.8 --out dataset/ --label fist
    python fetch_dataset.py --host 172.19.217.8 --out dataset/ --skip-existing

产出结构：
    dataset/
    ├─ config.json        # 采集时生效的配置副本（还原变量名用）
    ├─ index.csv          # 组名,类别,var0..3,帧数,字节,config_crc
    ├─ <label>/
    │   └─ data0007.gseq
    └─ legend.json        # 若配置里有 labels，导出成"数字→名称"映射

设计说明：
    - 列表走 /api/groups（设备端纯内存，零 SD I/O），不会拖慢录制
    - 每个片段单独 GET /api/group，失败会重试
    - config_crc 写进 index.csv，混了配置能一眼看出来
"""

import argparse
import csv
import json
import os
import sys
import time
import urllib.request
import urllib.error

GROUP_CHUNK = 100


def http_get(host, path, timeout=20):
    url = f"http://{host}{path}"
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise
            if attempt == 2:
                raise
        except Exception:
            if attempt == 2:
                raise
        time.sleep(0.5 * (attempt + 1))


def fetch_config(host):
    return json.loads(http_get(host, "/api/config"))


def fetch_groups(host, label=None):
    out, offset = [], 0
    while True:
        q = f"/api/groups?offset={offset}&limit={GROUP_CHUNK}"
        if label:
            q += f"&label={label}"
        page = json.loads(http_get(host, q))
        out.extend(page["groups"])
        offset += GROUP_CHUNK
        if offset >= page["total"]:
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True, help="设备 IP，如 172.19.217.8")
    ap.add_argument("--out", required=True, help="本地输出目录")
    ap.add_argument("--label", help="只拉某个类别")
    ap.add_argument("--skip-existing", action="store_true")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)

    # 1) 配置副本
    cfg = fetch_config(a.host)
    with open(os.path.join(a.out, "config.json"), "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    print(f"config: crc={cfg.get('cfg_crc')} types={len(cfg.get('types', []))} "
          f"vars={len(cfg.get('variables', []))}")

    # 2) 数字 → 名称映射（便于后续处理）
    legend = {}
    for v in cfg.get("variables", []):
        m = {str(l["code"]): l["name"] for l in v.get("labels", [])}
        if m:
            legend[v["name"]] = m
    if legend:
        with open(os.path.join(a.out, "legend.json"), "w", encoding="utf-8") as f:
            json.dump(legend, f, ensure_ascii=False, indent=2)
        print(f"legend: {list(legend.keys())}")

    # 3) 组列表
    groups = fetch_groups(a.host, a.label)
    print(f"共 {len(groups)} 组待拉取")
    if not groups:
        return

    rows, ok, fail = [], 0, 0
    for i, g in enumerate(groups, 1):
        label = g["label"]
        d = os.path.join(a.out, label)
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, g["name"] + ".gseq")

        if a.skip_existing and os.path.exists(path) and os.path.getsize(path) >= 8:
            rows.append([g["name"], label, *g["vars"], g["frames"], g["bytes"],
                         cfg.get("cfg_crc", "")])
            ok += 1
            continue

        try:
            blob = http_get(a.host, f"/api/group?name={g['name']}&raw=1")
            with open(path, "wb") as f:
                f.write(blob)
            rows.append([g["name"], label, *g["vars"], g["frames"], g["bytes"],
                         cfg.get("cfg_crc", "")])
            ok += 1
            print(f"  [{i}/{len(groups)}] {label}/{g['name']} {len(blob)} B")
        except Exception as e:
            fail += 1
            print(f"  [{i}/{len(groups)}] {g['name']} 失败: {e}")

    # 4) 索引
    with open(os.path.join(a.out, "index.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["name", "label", "v0", "v1", "v2", "v3",
                    "frames", "bytes", "config_crc"])
        w.writerows(rows)

    print(f"\n完成：成功 {ok}，失败 {fail} -> {a.out}")


if __name__ == "__main__":
    main()
