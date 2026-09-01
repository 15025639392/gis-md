#!/usr/bin/env python3
"""Extract and validate the current AMap dynamic SDF glyph contract.

The classic-normal renderer does not select a downloadable font file.  Its
SDFManagerWorker asks the official service for a PNG plus one metric tuple per
codepoint, then measures text as::

    sum((horiAdvance + 1) * fontSize / 24)

This tool deliberately records provider values only.  It never substitutes a
local font metric when a glyph or a field is absent.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import pathlib
import re
import urllib.parse
import urllib.request


CANONICAL_SIZE = 24
METRIC_NAMES = (
    "fontWidth",
    "fontHeight",
    "horiBearingX",
    "horiBearingY",
    "horiAdvance",
    "posX",
    "posY",
)


def require_runtime_contract(runtime: str) -> None:
    needles = (
        'pc:["://sdf.amap.com","://sdf01.amap.com"',
        'Mj:128',
        'ic:1',
        'e._size=24',
        'n+=(h+r)*a',
        '"/getsdfdata?chars="',
        'u=(n=n||12)<10?.78125:205/256',
        'f=u*(1-(10<o*e?10:o)/10.1)',
        'o=1.4142*(r<n||1<e?1.7:1.5)/n',
        'u+1.5/256*(e-1)',
    )
    for needle in needles:
        count = runtime.count(needle)
        if count != 1:
            raise ValueError(
                f"runtime contract needle must occur exactly once: "
                f"{needle!r}, got {count}"
            )


def decode_response(payload: bytes) -> dict:
    outer = json.loads(payload)
    if outer.get("code") != 1:
        raise ValueError("official SDF response did not succeed")
    url = outer.get("url", "")
    prefix = "data:image/png;base64,"
    if not url.startswith(prefix):
        raise ValueError("official SDF response has no PNG data URL")
    png = base64.b64decode(url[len(prefix) :], validate=True)
    if not png.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("official SDF payload is not PNG")
    raw_metrics = outer.get("info")
    if not isinstance(raw_metrics, dict) or not raw_metrics:
        raise ValueError("official SDF response has no glyph metrics")

    metrics = {}
    for key, values in sorted(raw_metrics.items(), key=lambda item: int(item[0])):
        codepoint = int(key)
        if codepoint <= 0 or codepoint > 0x10FFFF:
            raise ValueError(f"invalid codepoint {codepoint}")
        if not isinstance(values, list) or len(values) != len(METRIC_NAMES):
            raise ValueError(f"invalid metric tuple for U+{codepoint:04X}")
        if not all(isinstance(value, (int, float)) for value in values):
            raise ValueError(f"non-numeric metric for U+{codepoint:04X}")
        record = dict(zip(METRIC_NAMES, values))
        if record["fontWidth"] < 0 or record["fontHeight"] < 0 or record["horiAdvance"] < 0:
            raise ValueError(f"negative glyph extent for U+{codepoint:04X}")
        metrics[str(codepoint)] = record

    return {
        "canonicalSizePx": CANONICAL_SIZE,
        "letterSpacingPxAtCanonicalSize": 1,
        "measureFormula": "sum((horiAdvance + 1) * fontSize / 24)",
        "fragmentStyle": {
            "smallFontEdge": 0.78125,
            "normalFontEdge": 205 / 256,
            "gammaFormula": "1.4142 * (fontSize > 24 or DPR > 1 ? 1.7 : 1.5) / fontSize",
            "borderBufferFormula": "edge * (1 - min(10, strokeWidth * DPR) / 10.1)",
            "bufferFormula": "edge + 1.5 / 256 * (DPR - 1)",
        },
        "pngSha256": hashlib.sha256(png).hexdigest(),
        "pngBytes": len(png),
        "metrics": metrics,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=pathlib.Path, required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--response", type=pathlib.Path)
    source.add_argument("--chars")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    runtime = args.runtime.read_text(encoding="utf-8")
    require_runtime_contract(runtime)
    if args.response:
        payload = args.response.read_bytes()
    else:
        codepoints = [ord(char) for char in args.chars]
        if not codepoints:
            raise ValueError("--chars must contain at least one character")
        query = "%7C".join(str(value) for value in codepoints)
        url = "https://sdf.amap.com/getsdfdata?chars=" + query
        request = urllib.request.Request(
            url, headers={"Referer": "https://www.amap.com/"}
        )
        with urllib.request.urlopen(request, timeout=20) as response:
            payload = response.read()

    result = decode_response(payload)
    encoded = json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
