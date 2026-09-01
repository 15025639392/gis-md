#!/usr/bin/env python3
"""Extract the road color/zoom records from AMap classic-normal PBF.

The endpoint is a protobuf envelope whose field #1 is a repeated style record.
This intentionally uses a tiny wire reader instead of a generated schema: the
schema is private and the extractor should remain usable when AMap adds fields.
It emits only observed values and never invents a fallback style.
"""
import argparse
import json
import struct
from pathlib import Path

def varint(buf, pos):
    value = 0
    shift = 0
    while pos < len(buf):
        b = buf[pos]
        pos += 1
        value |= (b & 0x7f) << shift
        if not b & 0x80:
            return value, pos
        shift += 7
        if shift > 70:
            raise ValueError("varint too long")
    raise ValueError("truncated varint")


def fields(buf):
    pos = 0
    while pos < len(buf):
        tag, pos = varint(buf, pos)
        number, wire = tag >> 3, tag & 7
        if wire == 0:
            value, pos = varint(buf, pos)
        elif wire == 1:
            value, pos = buf[pos:pos + 8], pos + 8
        elif wire == 2:
            size, pos = varint(buf, pos)
            value, pos = buf[pos:pos + size], pos + size
        elif wire == 5:
            value, pos = buf[pos:pos + 4], pos + 4
        else:
            raise ValueError(f"unsupported wire type {wire}")
        yield number, wire, value


def signed32(value):
    value &= 0xffffffff
    return value - 0x100000000 if value & 0x80000000 else value


def nested_map(buf):
    result = {}
    for number, wire, value in fields(buf):
        if wire == 0:
            result[number] = signed32(value)
        elif wire == 2:
            result[number] = value
    return result


def decode_style_records(data):
    records = []
    for number, wire, value in fields(data):
        if number != 1 or wire != 2:
            continue
        record = nested_map(value)
        records.append(record)

    decoded = []
    previous = None
    for index, raw in enumerate(records):
        current = dict(raw)
        current_road = nested_map(raw.get(6, b""))
        current_label = nested_map(raw.get(5, b""))
        current_surface = nested_map(raw.get(7, b""))
        current_guide = nested_map(raw.get(8, b""))
        current_building = nested_map(raw.get(9, b""))
        fresh = bool(raw.get(10, 0))
        continuation = bool(raw.get(11, 0))
        if previous is not None:
            if continuation:
                current[1] = previous[1]
                current[2] = previous[2]
                current[3] = previous[4] + 1
                current[4] = previous[4] + 1
            else:
                current.setdefault(1, previous[1])
                if raw.get(2, 0) == 0:
                    current[2] = previous[2] + 1
                if raw.get(3, 0) == 0:
                    current[3] = previous[3]
                if raw.get(4, 0) == 0:
                    current[4] = previous[4]
            if not fresh:
                inherited = dict(previous.get(6, {}))
                for field, value in current_road.items():
                    if value not in (0, ""):
                        inherited[field] = value
                current_road = inherited
                inherited_label = dict(previous.get(5, {}))
                for field, value in current_label.items():
                    if value not in (0, ""):
                        inherited_label[field] = value
                current_label = inherited_label
                inherited_surface = dict(previous.get(7, {}))
                for field, value in current_surface.items():
                    if value not in (0, ""):
                        inherited_surface[field] = value
                current_surface = inherited_surface
                inherited_guide = dict(previous.get(8, {}))
                for field, value in current_guide.items():
                    if value not in (0, ""):
                        inherited_guide[field] = value
                current_guide = inherited_guide
                inherited_building = dict(previous.get(9, {}))
                for field, value in current_building.items():
                    if value not in (0, ""):
                        inherited_building[field] = value
                current_building = inherited_building
        current[6] = current_road
        current[5] = current_label
        current[7] = current_surface
        current[8] = current_guide
        current[9] = current_building
        current[10] = fresh
        current[11] = continuation
        current["recordIndex"] = index
        decoded.append(current)
        previous = current
    return decoded


def argb(value):
    return f"#{value & 0xffffffff:08x}" if value is not None else None


def extract(data):
    output = {}
    for record in decode_style_records(data):
        code = record.get(1)
        point_style = code is not None and 10000 <= code < 20000 and bool(
            record.get(5))
        surface_style = code is not None and bool(record.get(7))
        guide_style = code is not None and bool(record.get(8))
        if (6 not in record and not point_style and
                not surface_style and not guide_style and code != 55001):
            continue
        road = record.get(6, {})
        label = record.get(5, {})
        surface = record.get(7, {})
        guide = record.get(8, {})
        building = record.get(9, {})
        if code == 55001:
            output.setdefault(str(code), []).append({
                "subKey": record.get(2, 0),
                "minZoom": record.get(3),
                "maxZoom": record.get(4),
                "roofColor": argb(building.get(1)),
                "wallColor": argb(building.get(2)),
                "fresh": record.get(10, False),
                "continuation": record.get(11, False),
                "recordIndex": record["recordIndex"],
            })
            continue
        if surface_style:
            output.setdefault(str(code), []).append({
                "subKey": record.get(2, 0),
                "minZoom": record.get(3),
                "maxZoom": record.get(4),
                "surfaceFillColor": argb(surface.get(1)),
                "fresh": record.get(10, False),
                "continuation": record.get(11, False),
                "recordIndex": record["recordIndex"],
            })
            continue
        if guide_style:
            output.setdefault(str(code), []).append({
                "subKey": record.get(2, 0),
                "minZoom": record.get(3),
                "maxZoom": record.get(4),
                "guideTextColor": argb(guide.get(1)),
                "guideFontSize": guide.get(2),
                "guideIconAtlas": (f"icons_{guide.get(3)}"
                                   if guide.get(3) and guide.get(4) else None),
                "guideIconIndex": guide.get(4),
                "guideIconCellWidth": guide.get(5),
                "guideIconCellHeight": guide.get(6),
                "guideIconAtlasWidth": guide.get(7),
                "guideIconAtlasHeight": guide.get(8),
                "fresh": record.get(10, False),
                "continuation": record.get(11, False),
                "recordIndex": record["recordIndex"],
            })
            continue
        if point_style:
            output.setdefault(str(code), []).append({
                "subKey": record.get(2, 0),
                "minZoom": record.get(3),
                "maxZoom": record.get(4),
                "labelSize": label.get(1),
                "poiLabelColor": argb(label.get(2)),
                "poiLabelCasingColor": argb(label.get(3)),
                "labelType": label.get(4),
                # Current JSAPI Poi protobuf schema (2.3.5.6):
                #   #4  W9t       -> atlas suffix (`icons_<value>`)
                #   #5  iconIndex -> one-based cell index
                #   #6/#7         -> cell width / height
                #   #8            -> atlas width
                #   #9/#10        -> retina style height / width
                #   #17 flags     -> collision/coverage flags, NOT atlas id
                #   #20 _7t       -> atlas height
                "iconAtlas": (f"icons_{label.get(4)}"
                              if label.get(4) and label.get(5) else None),
                "iconIndex": label.get(5),
                "iconCellWidth": label.get(6),
                "iconCellHeight": label.get(7),
                "iconAtlasWidth": label.get(8),
                "iconDisplayHeightRetina": label.get(9),
                "iconDisplayWidthRetina": label.get(10),
                "iconFlags": label.get(17),
                # Runtime NebulaLabelFormat passes style.nn/style.an to Igt;
                # Igt maps them through 10 - 20*offset/24. Missing protobuf
                # fields are explicitly defaulted with `|| 0` by JSAPI.
                "iconAnchorOffsetX": label.get(18),
                "iconAnchorOffsetY": label.get(19),
                "iconAtlasHeight": label.get(20),
                "alternateIconAtlas": (f"icons_{label.get(11)}"
                                       if label.get(11) and label.get(12)
                                       else None),
                "alternateIconIndex": label.get(12),
                "alternateIconCellWidth": label.get(13),
                "alternateIconCellHeight": label.get(14),
                "alternateIconAtlasWidth": label.get(15),
                # Official JSAPI Poi field #16 (`w7t`) scales the alternate
                # q8t cell into provisional layout dimensions.  Igt later
                # replaces those dimensions with measured text geometry, but
                # the transient formula remains part of the official input
                # contract and must be validated by the generator.
                "alternateScale": label.get(16),
                "alternateIconAtlasHeight": label.get(21),
                "fresh": record.get(10, False),
                "continuation": record.get(11, False),
                "recordIndex": record["recordIndex"],
            })
            continue
        if 6 not in record:
            continue
        road_color = argb(road.get(3))
        road_label_color = argb(road.get(8))
        road_label_casing_color = argb(road.get(9))
        output.setdefault(str(code), []).append({
            "subKey": record.get(2, 0),
            "minZoom": record.get(3),
            "maxZoom": record.get(4),
            "lineType": road.get(1),
            "casingLineType": road.get(2),
            # Semantic names follow the current JSAPI Road schema/G9t path.
            "roadColor": road_color,
            "casingWidth": road.get(4),
            "lineWidth": road.get(5),
            "casing": argb(road.get(6)),
            "labelSize": road.get(7),
            "roadLabelColor": road_label_color,
            "roadLabelCasingColor": road_label_casing_color,
            "fresh": record.get(10, False),
            "continuation": record.get(11, False),
            "recordIndex": record["recordIndex"],
        })
    return {"format": "amap-classic-normal-style-v3", "styles": output}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pbf", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(extract(args.pbf.read_bytes()), ensure_ascii=False,
                        indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(result)
    else:
        print(result, end="")


if __name__ == "__main__":
    main()
