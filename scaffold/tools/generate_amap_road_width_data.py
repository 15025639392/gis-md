#!/usr/bin/env python3
"""Generate ordinary-road stroke tables from an official AMap style PBF."""

import argparse
import hashlib
from pathlib import Path

from extract_amap_style import extract


def style_identity(class_code, subkey):
    """Collision-free encoding of the official (class, subKey) tuple."""
    return class_code * 1000 + subkey


def center_drawable(record):
    return (record.get("lineWidth") or 0) > 0 and record.get("roadColor") is not None


def casing_drawable(record):
    # G9t forwards signed strokeWidth as borderWidth. The renderer's border
    # pass uses roadWidth + borderWidth, so a negative value can still be a
    # visible, narrower secondary stroke (for example 20024:50 and 20025).
    return ((record.get("lineWidth") or 0) +
            (record.get("casingWidth") or 0)) > 0 and record.get("casing") is not None


def record_drawable(record):
    return center_drawable(record) or casing_drawable(record)


def identities(styles):
    # Admit only identities with an official drawable pass. Width-only and
    # color-only records are not geometry contracts.
    return {
        int(class_code): tuple(sorted({
            record["subKey"] for record in records
            if record_drawable(record)
        }))
        for class_code, records in styles.items()
        if 20000 <= int(class_code) < 30000 and
           any(record_drawable(record) for record in records)
    }


def label_identities(styles):
    fields = ("labelSize", "roadLabelColor", "roadLabelCasingColor")
    return {
        int(class_code): tuple(sorted({
            record["subKey"] for record in records
            if any(record.get(field) is not None for field in fields)
        }))
        for class_code, records in styles.items()
        if 20000 <= int(class_code) < 30000 and
           any(any(record.get(field) is not None for field in fields)
               for record in styles[class_code])
    }


def casing_identities(styles, identity_map):
    return {
        class_code: tuple(subkey for subkey in subkeys if any(
            record["subKey"] == subkey and
            casing_drawable(record)
            for record in styles[str(class_code)]))
        for class_code, subkeys in identity_map.items()
        if any(any(record["subKey"] == subkey and
                   casing_drawable(record)
                   for record in styles[str(class_code)])
               for subkey in subkeys)
    }


def visibility_windows(styles, identity_map, predicate):
    result = []
    for class_code, subkeys in identity_map.items():
        records = styles[str(class_code)]
        for subkey in subkeys:
            matching = [r for r in records if r["subKey"] == subkey and
                        predicate(r)]
            result.append((style_identity(class_code, subkey),
                           min(r["minZoom"] for r in matching),
                           max(r["maxZoom"] for r in matching)))
    return result


def curves(styles, identity_map, field, maximum_zoom, reset_value,
           reset_before_active=False, transform=lambda value: value):
    result = []
    for class_code, subkeys in identity_map.items():
        records = styles[str(class_code)]
        for subkey in subkeys:
            matching = [r for r in records if r["subKey"] == subkey and
                        r["minZoom"] <= maximum_zoom]
            raw_stops = []
            active = False
            for record in matching:
                value = record[field]
                if value is not None:
                    active = True
                    raw_stops.append((record["minZoom"], transform(value)))
                elif record["fresh"] and (active or reset_before_active):
                    # A fresh record with an omitted field resets inherited
                    # state. Encode the reset explicitly instead of allowing
                    # the previous numeric step to leak into later zooms.
                    raw_stops.append((record["minZoom"], reset_value))
            stops = []
            for stop in raw_stops:
                if not stops or stops[-1][1] != stop[1]:
                    stops.append(stop)
            if not stops:
                # Some official identities are casing-only (or begin their
                # center stroke beyond the generated zoom ceiling). Preserve
                # the identity with the field's neutral value rather than
                # dropping it back to a class-level approximation.
                first_zoom = min(record["minZoom"] for record in matching)
                stops.append((first_zoom, reset_value))
            result.append((style_identity(class_code, subkey), stops))
    return result


def label_curves(styles, identity_map, field, maximum_zoom):
    """Return only identities that carry an official road-label field."""
    result = []
    for class_code, subkeys in identity_map.items():
        records = styles[str(class_code)]
        for subkey in subkeys:
            matching = [r for r in records if r["subKey"] == subkey and
                        r["minZoom"] <= maximum_zoom]
            raw_stops = [(record["minZoom"], record.get(field))
                         for record in matching
                         if record.get(field) is not None]
            stops = []
            for stop in raw_stops:
                if not stops or stops[-1][1] != stop[1]:
                    stops.append(stop)
            if stops:
                result.append((style_identity(class_code, subkey), stops))
    return result


def emit_table(name, values, stop_type, curve_type, format_value):
    stops = []
    rows = []
    canonical = {}
    for style_group, curve in values:
        signature = tuple(curve)
        if signature not in canonical:
            canonical[signature] = (len(stops), len(curve))
            stops.extend(curve)
        offset, count = canonical[signature]
        rows.append((style_group, offset, count))
    lines = [f"static constexpr {stop_type} k{name}Stops[] = {{"]
    lines.extend(f"    {{{zoom}, {format_value(value)}}}," for zoom, value in stops)
    lines.append("};")
    lines.append(f"static constexpr {curve_type} k{name}Curves[] = {{")
    lines.extend(f"    {{{group}, {offset}, {count}}},"
                 for group, offset, count in rows)
    lines.append("};")
    return "\n".join(lines)


def emit_identities(identity_map, name="kOrdinaryRoadIdentities"):
    rows = [f"    {{{class_code}, {subkey}}},"
            for class_code, subkeys in identity_map.items()
            for subkey in subkeys]
    return "\n".join([
        f"static constexpr RoadIdentity {name}[] = {{",
        *rows,
        "};",
    ])


def emit_visibility(values, name):
    return "\n".join([
        f"static constexpr RoadVisibility {name}[] = {{",
        *(f"    {{{group}, {min_zoom}, {max_zoom}}},"
          for group, min_zoom, max_zoom in values),
        "};",
    ])


def argb_literal(value):
    if value is None:
        return "0x00000000u"
    if not value.startswith("#") or len(value) != 9:
        raise RuntimeError(f"invalid #AARRGGBB color: {value}")
    return f"0x{value[1:].upper()}u"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pbf", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--maximum-zoom", type=int, default=30)
    args = parser.parse_args()
    data = args.pbf.read_bytes()
    styles = extract(data)["styles"]
    identity_map = identities(styles)
    casing_identity_map = casing_identities(styles, identity_map)
    line_label_identity_map = label_identities(styles)
    line_visibility = visibility_windows(styles, identity_map, record_drawable)
    label_visibility = visibility_windows(
        styles, line_label_identity_map,
        lambda record: any(record.get(field) is not None for field in
                           ("labelSize", "roadLabelColor",
                            "roadLabelCasingColor")))
    center = curves(styles, identity_map, "lineWidth", args.maximum_zoom, 0)
    # Preserve the official signed borderWidth. The renderer evaluates the
    # border pass as roadWidth + borderWidth and fail-closes non-positive
    # totals, matching G9t without inventing an abs()/clamp fallback.
    casing = curves(styles, identity_map, "casingWidth", args.maximum_zoom, 0)
    center_color = curves(styles, identity_map, "roadColor", args.maximum_zoom, None, True)
    casing_color = curves(styles, identity_map, "casing", args.maximum_zoom, None, True)
    center_type = curves(styles, identity_map, "lineType", args.maximum_zoom, 0)
    casing_type = curves(styles, identity_map, "casingLineType", args.maximum_zoom, 0)
    all_label_size = label_curves(styles, line_label_identity_map, "labelSize", args.maximum_zoom)
    all_label_color = label_curves(styles, line_label_identity_map, "roadLabelColor", args.maximum_zoom)
    all_label_casing = label_curves(styles, line_label_identity_map, "roadLabelCasingColor", args.maximum_zoom)
    digest = hashlib.sha256(data).hexdigest()
    output = (
        "// Generated by scaffold/tools/generate_amap_road_width_data.py.\n"
        f"// Source SHA256: {digest}; official AMap zoom <= {args.maximum_zoom}.\n"
        "// Do not edit manually.\n\n"
        + emit_identities(identity_map) + "\n\n"
        + emit_identities(casing_identity_map,
                          "kOfficialCasingIdentities") + "\n\n"
        + emit_identities(line_label_identity_map,
                          "kOfficialLineLabelIdentities") + "\n\n"
        + emit_visibility(line_visibility,
                          "kOfficialLineVisibility") + "\n\n"
        + emit_visibility(label_visibility,
                          "kOfficialLineLabelVisibility") + "\n\n"
        + emit_table("OrdinaryRoadCenterWidth", center, "RoadWidthStop",
                     "RoadWidthCurve", lambda v: f"{v}.0f") + "\n\n"
        + emit_table("OrdinaryRoadCasingWidth", casing, "RoadWidthStop",
                     "RoadWidthCurve", lambda v: f"{v}.0f") + "\n\n"
        + emit_table("OrdinaryRoadCenterColor", center_color, "RoadColorStop",
                     "RoadStrokeCurve", argb_literal) + "\n\n"
        + emit_table("OrdinaryRoadCasingColor", casing_color, "RoadColorStop",
                     "RoadStrokeCurve", argb_literal) + "\n\n"
        + emit_table("OrdinaryRoadCenterLineType", center_type,
                     "RoadLineTypeStop", "RoadStrokeCurve", str) + "\n\n"
        + emit_table("OrdinaryRoadCasingLineType", casing_type,
                     "RoadLineTypeStop", "RoadStrokeCurve", str) + "\n\n"
        + emit_table("OfficialLineLabelSize", all_label_size,
                     "RoadWidthStop", "RoadWidthCurve",
                     lambda v: f"{v}.0f") + "\n\n"
        + emit_table("OfficialLineLabelColor", all_label_color,
                     "RoadColorStop", "RoadStrokeCurve", argb_literal) + "\n\n"
        + emit_table("OfficialLineLabelCasingColor", all_label_casing,
                     "RoadColorStop", "RoadStrokeCurve", argb_literal) + "\n")
    args.output.write_text(output)


if __name__ == "__main__":
    main()
