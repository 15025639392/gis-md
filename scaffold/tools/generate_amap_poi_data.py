#!/usr/bin/env python3
"""Generate POI label, icon, background and collision contracts from AMap PBF."""

import argparse
import hashlib
import math
import re
from pathlib import Path

from extract_amap_style import extract


def point_style_classes(styles):
    return sorted(int(class_code) for class_code in styles
                  if 10000 <= int(class_code) < 20000 or
                  int(class_code) == 40001)


def identity(class_code, subkey):
    return class_code * 10000 + subkey


def argb(value):
    if value is None:
        return "0x00000000u"
    if not value.startswith("#") or len(value) != 9:
        raise RuntimeError(f"invalid official ARGB color: {value}")
    return f"0x{value[1:].upper()}u"


def atlas_number(name):
    return int(name.removeprefix("icons_"))


def q8t_provisional_size(record):
    """Consume the official R9t q8t provisional-dimension contract."""
    scale = record.get("alternateScale")
    cell_w = record.get("alternateIconCellWidth")
    cell_h = record.get("alternateIconCellHeight")
    if (not isinstance(cell_w, (int, float)) or isinstance(cell_w, bool) or
            not math.isfinite(cell_w) or cell_w <= 0 or
            not isinstance(cell_h, (int, float)) or isinstance(cell_h, bool) or
            not math.isfinite(cell_h) or cell_h <= 0):
        raise RuntimeError(
            "official q8t alternate cell dimensions must be positive finite "
            f"numbers: {cell_w!r}x{cell_h!r}")
    # Exact R9t branch: absent w7t preserves the atlas cell dimensions;
    # observed positive w7t scales both axes with Math.floor. Explicit
    # non-positive/non-finite values are not observed official contracts and
    # fail closed instead of silently becoming the missing-field branch.
    if scale is None:
        return cell_w, cell_h
    if (not isinstance(scale, (int, float)) or isinstance(scale, bool) or
            not math.isfinite(scale) or scale <= 0):
        raise RuntimeError(
            "official q8t alternateScale must be a positive finite number: "
            f"{scale!r}")
    return math.floor(cell_w / scale), math.floor(cell_h / scale)


def emit_label_records(styles):
    grouped = {}
    for class_code in point_style_classes(styles):
        if class_code == 40001:
            continue
        for record in styles[str(class_code)]:
            size = record.get("labelSize")
            if size is None:
                continue
            minimum = record["minZoom"]
            maximum = record["maxZoom"]
            if minimum > maximum:
                continue
            visual = (size, argb(record["poiLabelColor"]),
                      argb(record["poiLabelCasingColor"]))
            grouped.setdefault((class_code, record["subKey"]), []).append(
                (minimum, maximum, visual))
    normalized = []
    for (class_code, sub_key), records in sorted(grouped.items()):
        merged = []
        for minimum, maximum, visual in sorted(records):
            if merged and minimum <= merged[-1][1]:
                if visual != merged[-1][2]:
                    raise RuntimeError(
                        "overlapping official POI label contracts disagree: "
                        f"{class_code}:{sub_key} {merged[-1]} vs "
                        f"{(minimum, maximum, visual)}")
                merged[-1] = (merged[-1][0],
                              max(merged[-1][1], maximum), visual)
            else:
                merged.append((minimum, maximum, visual))
        normalized.extend((class_code, sub_key, minimum, maximum, visual)
                          for minimum, maximum, visual in merged)
    rows = [
        f"    {{{identity(class_code, sub_key)}, {minimum}, {maximum}, "
        f"{size}.0f, {text}, {halo}}},"
        for class_code, sub_key, minimum, maximum, (size, text, halo)
        in normalized]
    return "\n".join(["static constexpr PoiLabelRecord kPoiLabelRecords[] = {",
                       *rows, "};"])


def emit_guide_labels(styles):
    rows = []
    for record in styles.get("40001", []):
        if (record.get("guideFontSize") is None or
                record["minZoom"] > record["maxZoom"]):
            continue
        rows.append(
            f"    {{{record['subKey']}, {record['minZoom']}, "
            f"{record['maxZoom']}, {record['guideFontSize']}.0f, "
            f"{argb(record['guideTextColor'])}}},")
    return "\n".join([
        "static constexpr GuideLabelRecord kGuideLabelRecords[] = {",
        *rows, "};"])


def icon_row(class_code, record, alternate):
    if class_code == 40001:
        if alternate:
            return None
        atlas_name = record.get("guideIconAtlas")
        index = record.get("guideIconIndex")
        if not atlas_name or not index:
            return None
        return (f"    {{{class_code}, {record['subKey']}, "
                f"{record['minZoom'] - 1}, {record['maxZoom']}, "
                f"{atlas_number(atlas_name)}, {index}, "
                f"{record['guideIconCellWidth']}, "
                f"{record['guideIconCellHeight']}, "
                f"{record['guideIconAtlasWidth']}, "
                f"{record['guideIconAtlasHeight']}, 24.0f, 24.0f" + "},")
    prefix = "alternateIcon" if alternate else "icon"
    atlas_name = record.get(prefix + "Atlas")
    index = record.get(prefix + "Index")
    if not atlas_name or not index:
        return None
    cell_w = record[prefix + "CellWidth"]
    cell_h = record[prefix + "CellHeight"]
    atlas_w = record[prefix + "AtlasWidth"]
    atlas_h = record[prefix + "AtlasHeight"]
    # Some official point records target a separate non-bitmap renderer and
    # therefore omit the q8t atlas cell geometry. They remain valid text
    # contracts, but must not be converted into guessed bitmap frames.
    if not all((cell_w, cell_h, atlas_w, atlas_h)):
        return None
    # Alternate artwork is a q8t dynamic text background. R9t first consumes
    # either floor(cell / w7t) when w7t is positive or the original cell size
    # when w7t is absent; Igt then unconditionally replaces the provisional
    # dimensions with measured named-text geometry. Validate both official
    # transient branches while keeping only the final measured contract in
    # the production C++ table.
    if alternate:
        q8t_provisional_size(record)
        display_w = 0.0
        display_h = 0.0
    else:
        display_w = record["iconDisplayWidthRetina"] / 2.0
        display_h = record["iconDisplayHeightRetina"] / 2.0
    anchor_x = record.get("iconAnchorOffsetX") or 0
    anchor_y = record.get("iconAnchorOffsetY") or 0
    anchor_suffix = (f", {float(anchor_x):.1f}f, {float(anchor_y):.1f}f"
                     if anchor_x or anchor_y else "")
    return (f"    {{{class_code}, {record['subKey']}, {record['minZoom'] - 1}, "
            f"{record['maxZoom']}, {atlas_number(atlas_name)}, {index}, "
            f"{cell_w}, {cell_h}, {atlas_w}, {atlas_h}, "
            f"{display_w:.1f}f, {display_h:.1f}f{anchor_suffix}}},")


def emit_icons(styles, alternate, name):
    rows = []
    for class_code in point_style_classes(styles):
        for record in styles[str(class_code)]:
            row = icon_row(class_code, record, alternate)
            if row:
                rows.append(row)
    return "\n".join([f"static constexpr PoiIconRecord {name}[] = {{",
                       *rows, "};"])


def emit_flags(styles):
    grouped = {}
    for class_code in point_style_classes(styles):
        for record in styles[str(class_code)]:
            flags = record.get("iconFlags") or 0
            if flags:
                minimum = record["minZoom"] - 1
                maximum = record["maxZoom"]
                # Raw style data contains a handful of empty/reversed records.
                # They cannot be selected by the official half-open window and
                # must not become production contracts.
                if minimum >= maximum:
                    continue
                grouped.setdefault((class_code, record["subKey"]), []).append(
                    (minimum, maximum, flags))
    rows = []
    for (class_code, sub_key), records in sorted(grouped.items()):
        merged = []
        for minimum, maximum, flags in sorted(records):
            if merged and minimum < merged[-1][1] and flags != merged[-1][2]:
                raise RuntimeError(
                    "overlapping official POI flag contracts disagree: "
                    f"{class_code}:{sub_key} {merged[-1]} vs "
                    f"{(minimum, maximum, flags)}")
            if merged and minimum < merged[-1][1] and flags == merged[-1][2]:
                merged[-1] = (merged[-1][0],
                              max(merged[-1][1], maximum), flags)
            else:
                merged.append((minimum, maximum, flags))
        rows.extend((class_code, sub_key, minimum, maximum, flags)
                    for minimum, maximum, flags in merged)
    rows.sort()
    return "\n".join([
        "static constexpr PoiFlagRecord kPoiFlagRecords[] = {",
        *(f"    {{{cls}, {sub}, {minimum}, {maximum}, {flags}}},"
          for cls, sub, minimum, maximum, flags in rows), "};"])


def extract_runtime_layout(runtime):
    city_block = re.search(
        r'CITY_SPECIAL_CONFIG=\{(.*?)\},TEXT_DIRECTION_STRATEGIES=', runtime)
    direction_block = re.search(
        r'TEXT_DIRECTION_STRATEGIES=\[(.*?)\],CONSTANTS=', runtime)
    if not city_block or not direction_block:
        raise RuntimeError("official runtime layout contracts not found")
    cities = re.findall(
        r'"([^"]+)":\{zooms:\[([0-9.]+),([0-9.]+)\],ggt:![01]\}',
        city_block.group(1))
    directions = []
    for predicate, direction, x, y in re.findall(
            r'\{ygt:function\(t\)\{return(.*?)\},result:\{direction:"([^"]+)",'
            r'offset:\[(-?[0-9.]+),(-?[0-9.]+)\]\}\}',
            direction_block.group(1)):
        names = re.findall(r'"([^"]+)"===t', predicate)
        if not names:
            raise RuntimeError(f"unsupported official direction predicate: {predicate}")
        directions.extend((name, direction, x, y) for name in names)
    if not cities or not directions:
        raise RuntimeError("official runtime layout contracts are empty")
    return cities, directions


GUIDE_SHIELD_WIDTH_CONTRACT = (
    'x=b.shield,M=b.shieldType;(m=(b=labelsUtil.j8t(b,l)).name)',
    'T=1<x.length/4?x.length/4:1',
    'size:[24*(1<c?T:9*T/7),24],anchor:"center",angle:0,retina:!0',
)

POI_MULTILINE_SPLIT_CONTRACT = (
    'function getSpiltLineWithSpiltIndex(t,e){if(0===e.length)return t.trim();',
    'for(var r="",n=0,i=0;i<e.length;i++)',
    'r+=t.substring(n,e[i])+"\\n",n=e[i];',
    'return(r+=t.substring(n)).replace(/\\s+$/,"")}',
)


def validate_guide_shield_width_contract(runtime):
    offsets = []
    for needle in GUIDE_SHIELD_WIDTH_CONTRACT:
        count = runtime.count(needle)
        if count != 1:
            raise RuntimeError(
                "official guide shield width contract must match once, "
                f"got {count}: {needle}")
        offsets.append(runtime.find(needle))
    if not offsets[0] < offsets[1] < offsets[2] or offsets[2] - offsets[0] > 1800:
        raise RuntimeError(
            "official guide shield width contract moved outside its "
            "shieldType/oV consumer")


def validate_poi_multiline_split_contract(runtime):
    offsets = []
    for needle in POI_MULTILINE_SPLIT_CONTRACT:
        count = runtime.count(needle)
        if count != 1:
            raise RuntimeError(
                "official POI multiline split contract must match once, "
                f"got {count}: {needle}")
        offsets.append(runtime.find(needle))
    if offsets != sorted(offsets) or offsets[-1] - offsets[0] > 400:
        raise RuntimeError(
            "official POI multiline split contract moved outside its helper")


def emit_runtime_layout(runtime, access_oversea):
    cities, directions = extract_runtime_layout(runtime)
    if 'n?{direction:"bottom",offset:[0,0]}' not in runtime:
        raise RuntimeError("official kgt access-oversea branch not found")
    validate_guide_shield_width_contract(runtime)
    validate_poi_multiline_split_contract(runtime)
    city_rows = [f'    {{"{name}", {minimum}, {maximum}}},'
                 for name, minimum, maximum in cities]
    direction_value = {"center": 0, "right": 1, "left": 2,
                       "top": 3, "bottom": 4}
    direction_rows = []
    for name, direction, x, y in directions:
        if direction not in direction_value:
            raise RuntimeError(f"unknown official direction: {direction}")
        x_literal = f"{float(x):.1f}f"
        y_literal = f"{float(y):.1f}f"
        direction_rows.append(
            f'    {{"{name}", {direction_value[direction]}, '
            f'{x_literal}, {y_literal}}},')
    return "\n\n".join([
        "static constexpr bool kPoiAccessOversea = "
        + ("true;" if access_oversea else "false;"),
        "static constexpr PoiCityWindowRecord kPoiCityWindowRecords[] = {\n"
        + "\n".join(city_rows) + "\n};",
        "static constexpr PoiDirectionRecord kPoiDirectionRecords[] = {\n"
        + "\n".join(direction_rows) + "\n};"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pbf", type=Path)
    parser.add_argument("--runtime-js", type=Path, required=True)
    parser.add_argument("--access-oversea", choices=(0, 1), type=int,
                        required=True)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    data = args.pbf.read_bytes()
    runtime_data = args.runtime_js.read_bytes()
    runtime = runtime_data.decode("utf-8")
    styles = extract(data)["styles"]
    output = (
        "// Generated by scaffold/tools/generate_amap_poi_data.py.\n"
        f"// PBF SHA256: {hashlib.sha256(data).hexdigest()}.\n"
        f"// Runtime SHA256: {hashlib.sha256(runtime_data).hexdigest()}.\n"
        "// Do not edit manually.\n\n"
        + emit_label_records(styles) + "\n\n"
        + emit_guide_labels(styles) + "\n\n"
        + emit_icons(styles, False, "kPoiIconRecords") + "\n\n"
        + emit_icons(styles, True, "kPoiDynamicTextBackgroundRecords") + "\n\n"
        + emit_flags(styles) + "\n\n"
        + emit_runtime_layout(runtime, bool(args.access_oversea)) + "\n")
    args.output.write_text(output)


if __name__ == "__main__":
    main()
