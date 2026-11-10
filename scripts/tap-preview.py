#!/usr/bin/env python3
"""生成自包含的 TAP 源码预览页，供 WorkBuddy 结果区「网页」栏实时预览。

用法:
    python scripts/tap-preview.py examples/hello.tp
    python scripts/tap-preview.py examples/hello.tp -o build/hello.tap.html
    python scripts/tap-preview.py std/*.tp            # 每个文件各生成一页

产出的 HTML 把源码以 base64 内嵌，不依赖 fetch / 本地文件读取，
双击或在 WorkBuddy 里打开即可看到带行号与语法高亮的源码。
也可以直接打开 scripts/tap-view.html，用选择器或拖入查看任意 .tp / .tap。
"""
from __future__ import annotations

import argparse
import base64
import json
from pathlib import Path

TEMPLATE = Path(__file__).with_name("tap-view.html")
MARKER = "<!--TAP_EMBED-->"


def render(source: Path, template: str) -> str:
    if MARKER not in template:
        raise SystemExit(f"error: {TEMPLATE} 缺少 {MARKER} 占位符")
    text = source.read_bytes().decode("utf-8", errors="replace")
    # CRLF 在 <pre> 里会多出一个换行，显示前统一成 LF（只影响展示，不改源文件）。
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    payload = {"name": source.name, "text": text}
    encoded = base64.b64encode(
        json.dumps(payload, ensure_ascii=False).encode("utf-8")
    ).decode("ascii")
    embed = (
        '<script id="tap-embed" type="application/octet-stream">'
        + encoded
        + "</script>"
    )
    return template.replace(MARKER, embed)


def main() -> int:
    ap = argparse.ArgumentParser(description="生成 TAP 源码高亮预览页")
    ap.add_argument("sources", nargs="+", type=Path, help=".tp / .tap 源文件")
    ap.add_argument("-o", "--output", type=Path, help="输出 HTML（仅单个源文件时可用）")
    ap.add_argument("--dir", type=Path, default=Path("build/tap-preview"),
                    help="输出目录（默认 build/tap-preview）")
    args = ap.parse_args()

    if args.output and len(args.sources) != 1:
        raise SystemExit("error: -o 只能与单个源文件同用")

    template = TEMPLATE.read_text(encoding="utf-8")
    for src in args.sources:
        if not src.is_file():
            raise SystemExit(f"error: 找不到文件 {src}")
        dst = args.output if args.output else args.dir / (src.name + ".html")
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(render(src, template), encoding="utf-8")
        print(dst.as_posix())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
