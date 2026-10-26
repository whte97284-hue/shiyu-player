#!/usr/bin/env python3
"""对照 manifest.json 校验 tag_probe 的输出。用法: check_fixtures.py <tag_probe> <fixture_dir>"""
import json
import os
import subprocess
import sys

probe_bin, fx = sys.argv[1], sys.argv[2]
manifest = json.load(open(os.path.join(fx, "manifest.json"), encoding="utf-8"))
files = [os.path.join(fx, e["file"]) for e in manifest]
out = subprocess.run([probe_bin] + files, capture_output=True, text=True, check=True).stdout
results = {os.path.basename(json.loads(l)["file"]): json.loads(l) for l in out.splitlines() if l.strip()}

KEYMAP = {"bits": "bits", "lossless": "lossless", "cover_type": "cover_type", "cover_mime": "cover_mime",
          "cover_len": "cover_len", "cover_w": "cover_w", "cover_h": "cover_h"}
fails = 0
for e in manifest:
    name, exp = e["file"], e["expect"]
    got = results.get(name)
    errs = []
    if not got or not got["ok"]:
        errs.append("parse failed")
    else:
        for k, v in exp.items():
            if k == "duration_ms":
                if abs(got["duration_ms"] - v) > e["dur_tol"]:
                    errs.append(f"duration {got['duration_ms']} vs {v} (tol {e['dur_tol']})")
            elif k == "has_cover":
                if got["has_cover"] != v:
                    errs.append(f"has_cover {got['has_cover']} vs {v}")
            elif k == "bits":
                if v and got["bits"] != v:
                    errs.append(f"bits {got['bits']} vs {v}")
            elif k in got:
                if got[k] != v:
                    errs.append(f"{k}: {got[k]!r} vs {v!r}")
        # 惰性模式:封面不入内存,但必须能按偏移精确提取到与完整模式一致的字节
        if got["has_cover"]:
            if not got["lazy_has_cover"]:
                errs.append("lazy mode lost cover")
            if not got["extract_ok"] or got["extract_hash"] != got["cover_hash"] or got["extract_len"] != got["cover_len"]:
                errs.append(f"ExtractCover mismatch ({got['extract_len']} vs {got['cover_len']})")
        lim = e.get("lazy_max_io_bytes")
        if lim and got["lazy_io_bytes"] > lim:
            errs.append(f"lazy io {got['lazy_io_bytes']} > {lim}")
    status = "PASS" if not errs else "FAIL"
    if errs:
        fails += 1
    io = f"full io {got['io_calls']}x/{got['io_bytes']//1024}KB, lazy io {got['lazy_io_calls']}x/{got['lazy_io_bytes']//1024}KB" if got else ""
    print(f"[{status}] {name:42s} {io}")
    for x in errs:
        print("        -", x)
print(f"\n{len(manifest) - fails}/{len(manifest)} fixtures passed")
sys.exit(1 if fails else 0)
