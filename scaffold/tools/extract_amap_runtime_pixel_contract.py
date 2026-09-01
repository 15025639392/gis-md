#!/usr/bin/env python3
"""Extract AMap's complete road-width pixel contract without executing JS."""

import argparse
import hashlib
import json
from pathlib import Path

from extract_amap_style import extract as extract_style


MAIN_REQUIRED = {
    "roadStyle": ("roadWidth:t.lineWidth||0", "borderWidth:i||0"),
    "retinaCapability": ("scale:retina?2:1",),
    "poiTextLayout": (
        "t.prototype.AO=function",
        "b=s[0],w=s[1],T=s[2],S=s[3]",
        "strokeWidth:2,padding:[0,1,0,1],fold:!i&&5<=V",
        "strokeWidth:0,padding:[0,1,0,1]}}",
    ),
}

POI_FLAG_REQUIRED = {
    "decode": (
        "K9t=function(t){return{canCovered:0!=(1&t),Y8t:0!=(4&t),X8t:0!=(8&t)}",
    ),
    "providerToTextStyle": (
        "Y8t:q,Z8t:Z,X8t:K,strokeWidth:2",
    ),
    "styleMerge": (
        "r.style.Y8t=t.Y8t||undefined",
    ),
}

WEBGL_REQUIRED = {
    "propertyConsumer": (
        "e.prototype.An=function",
        "var LineProperty=function",
        'roadWidth:new LineProperty("roadWidth","linear")',
    ),
    "casingConsumer": (
        "function getLineBorderUniformValues",
        "var v=linePropertys.roadWidth.An(i,o,f,n,s)+linePropertys.borderWidth.An(i,o,f,n,s)",
        "return{u_skyHeight:a,u_matrix:e.viewState.mvpMatrix,u_meter_per_pixel:e.viewState.resolution,u_width:v,u_border:1",
    ),
    "faceConsumer": (
        "function getLineFaceUniformValues",
        "var c=linePropertys.roadWidth.An(i,o,f,n,s)",
        "return{u_matrix:e.viewState.mvpMatrix,u_meter_per_pixel:e.viewState.resolution,u_width:c,u_color:l",
    ),
    "vertexExpansion": (
        'var lineVertextString="precision highp float;',
        r"eltaCenter.y;\\n    gl_Position = u_matrix * vec4(pos + a_normal * width * u_meter_per_pixel * 0.5,",
    ),
    "backingStore": (
        "var scale$4=Support$7.scale",
        "e.prototype.renderFrame=function(e){if(e.size[0]*scale$4!==this.io.width)",
        'if(e.size[0]*scale$4!==this.io.width){this.io.width=e.size[0]*scale$4;this.io.style.width=e.size[0]+"px"}',
        'if(e.size[1]*scale$4!==this.io.height){this.io.height=e.size[1]*scale$4;this.io.style.height=e.size[1]+"px"}',
        "this.context.be.set([0,0,this.io.width,this.io.height])",
        "this.gl.viewport(e[0],e[1],e[2],e[3])",
    ),
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def unique_offsets(source: str, required: dict[str, tuple[str, ...]]) -> dict:
    offsets = {}
    for section, needles in required.items():
        offsets[section] = {}
        for needle in needles:
            count = source.count(needle)
            if count != 1:
                raise ValueError(
                    f"official runtime contract must match once, got {count}: {needle}"
                )
            offsets[section][needle] = source.find(needle)
    return offsets


def official_poi_flag_contract(source: str) -> dict:
    offsets = unique_offsets(source, POI_FLAG_REQUIRED)
    # The fixed 2.3.5.6 main bundle contains the complete LabelPlacement,
    # LabelBucket, LabelTextStyle and LabelWorker implementations. Y8t occurs
    # only in decode/forward/merge positions; a new occurrence is therefore a
    # behavior-contract change that must be audited rather than guessed.
    occurrences = source.count("Y8t")
    if occurrences != 7:
        raise ValueError(
            "official Y8t contract changed: expected exactly 7 bundle "
            f"occurrences, got {occurrences}")
    consumers = ("LabelPlacement=function", "LabelBucket=", "LabelTextStyle=",
                 "LabelWorker=function")
    for consumer in consumers:
        if source.count(consumer) != 1:
            raise ValueError(f"official label consumer missing or duplicated: {consumer}")
    return {
        "bitMasks": {"canCovered": 1, "Y8t": 4, "X8t": 8},
        "Y8tOccurrenceCount": occurrences,
        "observableConsumer": None,
        "semantics": "decoded-forwarded-no-observable-consumer",
        "completeConsumersPresent": list(consumers),
        "offsets": offsets,
    }


def official_width_signature(style_pbf: Path, class_code: int, sub_key: int,
                             display_zoom: float, css_width: int,
                             css_height: int) -> dict:
    if not 0.0 <= display_zoom <= 29.0:
        raise ValueError("display zoom must map to an official provider zoom")
    provider_zoom = int(display_zoom) + 1
    style_data = style_pbf.read_bytes()
    records = extract_style(style_data)["styles"].get(str(class_code), ())
    matching = [record for record in records
                if record["subKey"] == sub_key and
                record["minZoom"] <= provider_zoom <= record["maxZoom"]]
    if len(matching) != 1:
        raise ValueError(
            f"official width record must match once, got {len(matching)}: "
            f"{class_code}:{sub_key} provider zoom {provider_zoom}")
    record = matching[0]
    road_width = record.get("lineWidth")
    border_width = record.get("casingWidth")
    if road_width is None or border_width is None:
        raise ValueError("official width record lacks center or signed border width")
    center_css = float(road_width)
    secondary_css = center_css + float(border_width)
    if center_css < 0 or secondary_css < 0:
        raise ValueError("official width record produces a negative visible width")
    scales = {}
    for scale in (1, 2):
        scales[f"{scale}x"] = {
            "physicalViewport": [css_width * scale, css_height * scale],
            "centerPhysicalPx": center_css * scale,
            "secondaryPhysicalPx": secondary_css * scale,
        }
    return {
        "stylePbf": {"path": str(style_pbf), "sha256": sha256(style_data)},
        "identity": {"class": class_code, "subKey": sub_key},
        "displayZoom": display_zoom,
        "providerZoom": provider_zoom,
        "roadWidthCssPx": center_css,
        "signedBorderWidthCssPx": float(border_width),
        "centerCssPx": center_css,
        "secondaryCssPx": secondary_css,
        "cssViewport": [css_width, css_height],
        "officialRetinaBranches": scales,
    }


def extract(runtime_js: Path, webgl_render: Path, style_pbf: Path,
            *, class_code: int = 20001, sub_key: int = 1,
            display_zoom: float = 13.0, css_width: int = 1280,
            css_height: int = 720) -> dict:
    runtime_data = runtime_js.read_bytes()
    webgl_data = webgl_render.read_bytes()
    main_offsets = unique_offsets(
        runtime_data.decode("utf-8", errors="ignore"), MAIN_REQUIRED)
    runtime_source = runtime_data.decode("utf-8", errors="ignore")
    webgl_offsets = unique_offsets(
        webgl_data.decode("utf-8", errors="ignore"), WEBGL_REQUIRED)
    return {
        "artifacts": {
            "runtime": {"path": str(runtime_js), "sha256": sha256(runtime_data)},
            "webglRender": {"path": str(webgl_render), "sha256": sha256(webgl_data)},
        },
        "styleWidthUnit": "css-px",
        "poiTextLayoutContract": {
            "paddingCssPx": [0, 1, 0, 1],
            "formatterBranches": ["Poi", "Guide"],
            "consumer": "NebulaLabelFormat.AO",
        },
        "poiFlagContract": official_poi_flag_contract(runtime_source),
        "consumerChain": [
            "PBF Road.lineWidth / signed Road.borderWidth",
            "G9t roadWidth / borderWidth",
            "LineProperty.An(identity, zoom)",
            "face u_width = roadWidth",
            "casing u_width = roadWidth + signed borderWidth",
            "u_meter_per_pixel = viewState.resolution",
            "vertex offset = normal * width * resolution * 0.5",
            "canvas backing = CSS size * Support.scale",
            "gl.viewport(canvas backing size)",
        ],
        "numericSignature": official_width_signature(
            style_pbf, class_code, sub_key, display_zoom,
            css_width, css_height),
        "offsets": {"runtime": main_offsets, "webglRender": webgl_offsets},
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runtime_js", type=Path)
    parser.add_argument("--webgl-render", required=True, type=Path)
    parser.add_argument("--style-pbf", required=True, type=Path)
    parser.add_argument("--class-code", type=int, default=20001)
    parser.add_argument("--sub-key", type=int, default=1)
    parser.add_argument("--display-zoom", type=float, default=13.0)
    parser.add_argument("--css-width", type=int, default=1280)
    parser.add_argument("--css-height", type=int, default=720)
    args = parser.parse_args()
    try:
        result = extract(
            args.runtime_js, args.webgl_render, args.style_pbf,
            class_code=args.class_code, sub_key=args.sub_key,
            display_zoom=args.display_zoom, css_width=args.css_width,
            css_height=args.css_height)
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
