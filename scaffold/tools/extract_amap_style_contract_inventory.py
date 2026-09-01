#!/usr/bin/env python3
"""Inventory every observed official AMap classic-normal style field.

This is a fail-loud schema/consumer gate, not a second style decoder.  The
fixed official PBF owns the observed field set; each field must have an
explicit classification and a traced extractor -> generated/runtime -> C++
consumer chain.  A newly observed field is therefore a test failure instead
of silently becoming an ignored protobuf value.
"""

import argparse
import json
from pathlib import Path

from extract_amap_style import fields, nested_map


SCOPES = {
    "style": set(range(1, 12)),
    "label": set(range(1, 18)) | {20, 21},
    "road": set(range(1, 10)),
    "region": {1},
    "guide": set(range(1, 9)),
    "building": {1, 2},
}

NESTED_SCOPE = {5: "label", 6: "road", 7: "region", 8: "guide", 9: "building"}

STYLE_NAMES = {
    1: "class", 2: "subKey", 3: "minZoom", 4: "maxZoom",
    5: "label", 6: "road", 7: "region", 8: "guide", 9: "building",
    10: "fresh", 11: "continuation",
}
LABEL_NAMES = {
    1: "fontSize", 2: "textColor", 3: "haloColor", 4: "atlas",
    5: "iconIndex", 6: "cellWidth", 7: "cellHeight", 8: "atlasWidth",
    9: "displayHeight", 10: "displayWidth", 11: "alternateAtlas",
    12: "alternateIndex", 13: "alternateCellWidth",
    14: "alternateCellHeight", 15: "alternateAtlasWidth",
    16: "alternateScale", 17: "flags", 20: "atlasHeight",
    21: "alternateAtlasHeight",
}
ROAD_NAMES = {
    1: "lineType", 2: "casingLineType", 3: "color", 4: "casingWidth",
    5: "lineWidth", 6: "casingColor", 7: "labelSize",
    8: "labelColor", 9: "labelHaloColor",
}
GUIDE_NAMES = {
    1: "textColor", 2: "fontSize", 3: "atlas", 4: "iconIndex",
    5: "cellWidth", 6: "cellHeight", 7: "atlasWidth", 8: "atlasHeight",
}
NAMES = {
    "style": STYLE_NAMES, "label": LABEL_NAMES, "road": ROAD_NAMES,
    "region": {1: "fillColor"}, "guide": GUIDE_NAMES,
    "building": {1: "roofColor", 2: "wallColor"},
}

EXTRACTOR = "scaffold/tools/extract_amap_style.py"
POI_GEN = "scaffold/tools/generate_amap_poi_data.py"
ROAD_GEN = "scaffold/tools/generate_amap_road_width_data.py"
SURFACE_GEN = "scaffold/tools/generate_amap_surface_data.py"
LABEL_CPP = "scaffold/src/earth_engine/style/AmapClassicLabelStyle.cpp"
ROAD_CPP = "scaffold/src/earth_engine/style/AmapClassicRoadStyle.cpp"
SURFACE_CPP = "scaffold/src/earth_engine/style/AmapClassicSurfaceStyle.cpp"
LAYER_CPP = "scaffold/src/earth_engine/layers/FeatureRenderLayer.cpp"
ASSETS_CPP = "scaffold/src/earth_engine/style/AmapClassicAssets.cpp"
LAYER_TEST_CPP = "scaffold/tests/unit/layers/test_feature_render_layer.cpp"
RUNTIME_JS = ".codex/artifacts/amap-webapi-maps-2.3.5.6.js"
WEBGL_JS = ".codex/artifacts/amap-webgl-render-20260830.js"


def contract(scope, number):
    """Return a field-specific classification and evidence chain."""
    if scope == "style":
        if number == 1:
            return "consumed-identity-class", [
                (EXTRACTOR, "code = record.get(1)"),
                (EXTRACTOR, "output.setdefault(str(code)"),
                (POI_GEN, "for class_code in point_style_classes(styles)"),
                (ROAD_GEN, "for class_code, records in styles.items()"),
                (SURFACE_GEN, "for class_code in sorted(map(int, styles))"),
                (LABEL_CPP, "amapClassicLabelIdentity(classCode, subKey)"),
                (ROAD_CPP, "officialStyleIdentity(it->first, subKey)"),
                (SURFACE_CPP, "std::pair{classCode, subKey}"),
            ]
        if number == 2:
            return "consumed-identity-subkey", [
                (EXTRACTOR, '"subKey": record.get(2, 0)'),
                (POI_GEN, 'grouped.setdefault((class_code, record["subKey"])'),
                (ROAD_GEN, 'record["subKey"] for record in records'),
                (SURFACE_GEN, 'grouped.setdefault((class_code, record["subKey"])'),
                (LABEL_CPP, "it->cls == classCode && it->sub == subKey"),
                (ROAD_CPP, "it->subKey == subKey"),
                (SURFACE_CPP, "it->classCode == classCode && it->subKey == subKey"),
            ]
        if number == 3:
            return "consumed-window-minimum", [
                (EXTRACTOR, '"minZoom": record.get(3)'),
                (POI_GEN, 'minimum = record["minZoom"]'),
                (ROAD_GEN, 'min(r["minZoom"] for r in matching)'),
                (SURFACE_GEN, '(record["minZoom"], record["maxZoom"]'),
                (LABEL_CPP, "displayZoom >= it->minZoom"),
                (ROAD_CPP, "visibility.minZoom - 1.0"),
                (SURFACE_CPP, "record.minZoom <= providerZoom"),
            ]
        if number == 4:
            return "consumed-window-maximum", [
                (EXTRACTOR, '"maxZoom": record.get(4)'),
                (POI_GEN, 'maximum = record["maxZoom"]'),
                (ROAD_GEN, 'max(r["maxZoom"] for r in matching)'),
                (SURFACE_GEN, '(record["minZoom"], record["maxZoom"]'),
                (LABEL_CPP, "displayZoom < it->maxZoom"),
                (ROAD_CPP, "maxIt->second != visibility.maxZoom"),
                (SURFACE_CPP, "providerZoom <= record.maxZoom"),
            ]
        if number in NESTED_SCOPE:
            routes = {
                5: ("consumed-label-container-route", [
                    (EXTRACTOR, "current_label = nested_map(raw.get(5, b\"\"))"),
                    (POI_GEN, "kPoiLabelRecords"),
                    (LABEL_CPP,
                     "style.labelSizeExprByStyleGroup[styleGroup] = makeSteps(sizes)"),
                    (LAYER_CPP,
                     "style.labelSizeExprByStyleGroup.find(styleGroup)")]),
                6: ("consumed-road-container-route", [
                    (EXTRACTOR, "current_road = nested_map(raw.get(6, b\"\"))"),
                    (ROAD_GEN, "OrdinaryRoadCenterWidth"),
                    (ROAD_CPP, "style.lineWidthExprByStyleGroup"),
                    (LAYER_CPP,
                     "auto rangeLineWidthCssPxValue = evalStrokeNumber("),
                    (LAYER_CPP,
                     "style_.lineWidthExprByStyleGroup);")]),
                7: ("consumed-region-container-route", [
                    (EXTRACTOR, "current_surface = nested_map(raw.get(7, b\"\"))"),
                    (SURFACE_GEN, "kOfficialSurfaceRecords"),
                    (SURFACE_CPP,
                     "style.fillColorExprByStyleGroup[styleGroup] = StyleExpression::step"),
                    (LAYER_CPP, "cmd.kind = RenderCommandKind::VectorFill")]),
                8: ("consumed-guide-container-route", [
                    (EXTRACTOR, "current_guide = nested_map(raw.get(8, b\"\"))"),
                    (POI_GEN, "kGuideLabelRecords"),
                    (LABEL_CPP, "for (const auto& record : kGuideLabelRecords)"),
                    (LABEL_CPP, "out.officialIconAtlas = icon.atlas"),
                    (LAYER_CPP,
                     "officialIconAtlasDemand_(resolved.officialIconAtlas)")]),
                9: ("consumed-building-container-route", [
                    (EXTRACTOR, "current_building = nested_map(raw.get(9, b\"\"))"),
                    (SURFACE_GEN, "kBuilding55001"),
                    (SURFACE_CPP,
                     "style.extrusionRoofColorByStyleGroup[styleGroup] = color(record.roofArgb)"),
                    (LAYER_CPP,
                     "cmd.kind = RenderCommandKind::VectorExtrusion")]),
            }
            return routes[number]
        if number == 10:
            return "consumed-road-field-reset-control", [
                (EXTRACTOR, "fresh = bool(raw.get(10, 0))"),
                (EXTRACTOR, "if not fresh:"),
                (EXTRACTOR, "inherited = dict(previous.get(6, {}))"),
                (EXTRACTOR, '"fresh": record.get(10, False)'),
                (ROAD_GEN,
                 'elif record["fresh"] and (active or reset_before_active):'),
                (ROAD_GEN, 'raw_stops.append((record["minZoom"], reset_value))'),
                (LAYER_TEST_CPP,
                 "Keeping the old red casing here would violate field 10."),
            ]
        return "consumed-record-continuation-control", [
            (EXTRACTOR, "continuation = bool(raw.get(11, 0))"),
            (EXTRACTOR, "if continuation:"),
            (EXTRACTOR, "current[1] = previous[1]"),
            (EXTRACTOR, "current[2] = previous[2]"),
            (EXTRACTOR, "current[3] = previous[4] + 1"),
            (EXTRACTOR, "current[4] = previous[4] + 1"),
            (EXTRACTOR, '"continuation": record.get(11, False)'),
            (ROAD_GEN, 'r["minZoom"] <= maximum_zoom]'),
            (ROAD_GEN, 'raw_stops.append((record["minZoom"], transform(value)))'),
            (LAYER_TEST_CPP,
             "Provider zoom 10 is a continuation record that explicitly restores"),
        ]

    if scope == "label":
        extracted = {
            1: "labelSize", 2: "poiLabelColor", 3: "poiLabelCasingColor",
            4: "labelType", 5: "iconIndex", 6: "iconCellWidth",
            7: "iconCellHeight", 8: "iconAtlasWidth",
            9: "iconDisplayHeightRetina", 10: "iconDisplayWidthRetina",
            11: "alternateIconAtlas", 12: "alternateIconIndex",
            13: "alternateIconCellWidth", 14: "alternateIconCellHeight",
            15: "alternateIconAtlasWidth", 16: "alternateScale",
            17: "iconFlags",
            20: "iconAtlasHeight", 21: "alternateIconAtlasHeight",
        }
        if number == 16:
            return "consumed-transient-final-overwritten", [
                (EXTRACTOR, '"alternateScale"'),
                (RUNTIME_JS, "t.poi.w7t"),
                (POI_GEN, "q8t_provisional_size"),
                (POI_GEN, "math.floor(cell_w / scale)"),
                (LAYER_CPP,
                 "measuredLayout->iconWidthPx = totalAdvance + dynamicInsetPx"),
                (LAYER_CPP, "measuredLayout->iconHeightPx ="),
            ]
        key = extracted[number]
        if number <= 3:
            generated = {1: "size, argb(record[\"poiLabelColor\"])",
                         2: 'argb(record["poiLabelColor"])',
                         3: 'argb(record["poiLabelCasingColor"])'}[number]
            final = {1: "style.labelSizeExprByStyleGroup[styleGroup] = makeSteps(sizes)",
                     2: "style.labelColorExprByStyleGroup[styleGroup] = makeSteps(colors)",
                     3: "style.labelHaloColorExprByStyleGroup[styleGroup] = makeSteps(halos)"}[number]
            classification = {1: "consumed-label-size",
                              2: "consumed-label-color",
                              3: "consumed-label-halo-color"}[number]
            chain = [(EXTRACTOR, f'"{key}"'),
                     (POI_GEN, generated), (LABEL_CPP, final)]
            final_oracle = {
                1: "Provider zoom 5 changes only field 1",
                2: "Official label field 2 final text-color consumer.",
                3: "Official label field 3 final halo-color consumer.",
            }[number]
            chain.append((LAYER_TEST_CPP, final_oracle))
            return classification, chain
        if 4 <= number <= 10 or number == 20:
            generated = {
                4: 'atlas_number(atlas_name)',
                5: 'index = record.get(prefix + "Index")',
                6: 'cell_w, cell_h, atlas_w, atlas_h',
                7: 'cell_w, cell_h, atlas_w, atlas_h',
                8: 'cell_w, cell_h, atlas_w, atlas_h',
                9: 'display_h = record["iconDisplayHeightRetina"] / 2.0',
                10: 'display_w = record["iconDisplayWidthRetina"] / 2.0',
                20: 'cell_w, cell_h, atlas_w, atlas_h',
            }[number]
            final = {
                4: (LABEL_CPP, "out.officialIconAtlas = icon.atlas"),
                5: (ASSETS_CPP, "frameContract.iconIndex - 1"),
                6: (ASSETS_CPP, "frameContract.cellWidth"),
                7: (ASSETS_CPP, "frameContract.cellHeight"),
                8: (ASSETS_CPP, "image->width != frameContract.atlasWidth"),
                9: (LABEL_CPP, "out.labelLayout->iconHeightPx = icon.displayHeightPx"),
                10: (LABEL_CPP, "out.labelLayout->iconWidthPx = icon.displayWidthPx"),
                20: (ASSETS_CPP, "image->height != frameContract.atlasHeight"),
            }[number]
            chain = [
                (EXTRACTOR, f'"{key}"'), (POI_GEN, generated),
                (LABEL_CPP, "return {true, row.atlas, row.index, row.cellW, row.cellH"),
                final]
            if number <= 10:
                chain.append((LAYER_TEST_CPP, {
                    4: "Official label field 4 final atlas consumer.",
                    5: "Official label field 5 final one-based icon-index consumer.",
                    6: "Official label field 6 final cell-width consumer.",
                    7: "Official label field 7 final cell-height consumer.",
                    8: "Official label field 8 final source-atlas-width consumer.",
                    9: "Official label field 9 final display-height consumer.",
                    10: "Official label field 10 final display-width consumer.",
                }[number]))
            return f"consumed-icon-{NAMES['label'][number]}", chain
        if number == 17:
            return "consumed-visible-bit1+decoded-nonrendering-bit4", [
                (EXTRACTOR, '"iconFlags"'), (POI_GEN, "kPoiFlagRecords"),
                (LABEL_CPP, "amapClassicPoiFlags"),
                (LABEL_CPP, "out.officialCanCovered = (flags & 1) != 0"),
                (LAYER_CPP,
                 "officialCanCovered = resolved.officialCanCovered"),
                (LABEL_CPP, "const int flags = amapClassicPoiFlags("),
                ("scaffold/tools/extract_amap_runtime_pixel_contract.py",
                 '"observableConsumer": None')]
        dynamic_final = {
            11: (LABEL_CPP, "out.officialDynamicBackgroundAtlas = dynamic.atlas"),
            12: (ASSETS_CPP, "frameContract.iconIndex - 1"),
            13: (ASSETS_CPP, "frameContract.cellWidth"),
            14: (ASSETS_CPP, "frameContract.cellHeight"),
            15: (ASSETS_CPP, "image->width != frameContract.atlasWidth"),
            21: (ASSETS_CPP, "image->height != frameContract.atlasHeight"),
        }[number]
        chain = [
            (EXTRACTOR, f'"{key}"'),
            (POI_GEN, 'prefix = "alternateIcon" if alternate else "icon"'),
            (POI_GEN, "kPoiDynamicTextBackgroundRecords"),
            (LABEL_CPP, "resolveAmapClassicPoiDynamicBackgroundStyle"),
            (LAYER_CPP, "providerLayout->dynamicBackgroundImage"),
            dynamic_final]
        if 11 <= number <= 15:
            chain.append((LAYER_TEST_CPP, {
                11: "EXPECT_EQ(4, dynamic.atlas)",
                12: "EXPECT_EQ(73, dynamic.iconIndex)",
                13: "EXPECT_EQ(64, dynamic.cellWidth)",
                14: "EXPECT_EQ(64, dynamic.cellHeight)",
                15: "EXPECT_EQ(512, dynamic.atlasWidth)",
            }[number]))
        return f"consumed-dynamic-{NAMES['label'][number]}", chain

    if scope == "road":
        key = {1: "lineType", 2: "casingLineType", 3: "roadColor",
               4: "casingWidth", 5: "lineWidth", 6: '"casing"',
               7: "labelSize", 8: "roadLabelColor",
               9: "roadLabelCasingColor"}[number]
        final = {
            1: "style.lineTypeExprByStyleGroup",
            2: "style.lineCasingTypeExprByStyleGroup",
            3: "style.lineColorExprByStyleGroup",
            4: "style.lineCasingWidthExprByStyleGroup",
            5: "style.lineWidthExprByStyleGroup",
            6: "style.lineCasingColorExprByStyleGroup",
            7: "style.labelSizeExprByStyleGroup",
            8: "style.labelColorExprByStyleGroup",
            9: "style.labelHaloColorExprByStyleGroup",
        }[number]
        classification = {
            1: "consumed-road-line-type",
            2: "consumed-road-casing-line-type",
            3: "consumed-road-color",
            4: "consumed-road-casing-width",
            5: "consumed-road-width",
            6: "consumed-road-casing-color",
            7: "consumed-road-label-size",
            8: "consumed-road-label-color",
            9: "consumed-road-label-halo-color",
        }[number]
        chain = [(EXTRACTOR, key), (ROAD_GEN, key),
                 (ROAD_CPP, final), (LAYER_CPP, "lineStyleGroup")]
        if number == 1:
            chain.append((
                LAYER_TEST_CPP,
                "AmapOfficialDashUsesBinaryRetinaScaleExactlyOnce"))
        elif number == 2:
            chain.append((
                LAYER_TEST_CPP,
                "OfficialCenterAndCasingLineTypesReachIndependentFinalCommands"))
        elif number in (3, 4, 5, 6):
            final_oracle = {
                3: "Official road field 3 final center-color consumer.",
                4: "Official road field 4 final signed casing-width consumer.",
                5: "Official road field 5 final center-width consumer.",
                6: "Official road field 6 final casing-color consumer.",
            }[number]
            chain.append((LAYER_TEST_CPP, final_oracle))
        elif number in (7, 8, 9):
            final_oracle = {
                7: "Official road label field 7 final size consumer:",
                8: "Official road label field 8 final text-color consumer.",
                9: "Official road label field 9 final halo-color consumer.",
            }[number]
            chain.append((LAYER_TEST_CPP, final_oracle))
        return classification, chain

    if scope == "region":
        return "consumed-region-fill-color", [
            (EXTRACTOR, '"surfaceFillColor": argb(surface.get(1))'),
            (SURFACE_GEN, 'color = record.get("surfaceFillColor")'),
            (SURFACE_GEN, "(record[\"minZoom\"], record[\"maxZoom\"], argb(color))"),
            (SURFACE_CPP,
             "style.fillColorExprByStyleGroup[styleGroup] = StyleExpression::step"),
            (LAYER_CPP,
             "style_.fillColorExprByStyleGroup.find(styleGroup)"),
            (LAYER_CPP, "const auto value = colorIt->second->evaluate(nullptr, zoomLevel)"),
            (LAYER_CPP, "cmd.vectorUniforms.color = value->color()")]

    if scope == "guide":
        key = {1: "guideTextColor", 2: "guideFontSize", 3: "guideIconAtlas",
               4: "guideIconIndex", 5: "guideIconCellWidth",
               6: "guideIconCellHeight", 7: "guideIconAtlasWidth",
               8: "guideIconAtlasHeight"}[number]
        if number <= 2:
            final = ("style.labelColorExprByStyleGroup" if number == 1
                     else "style.labelSizeExprByStyleGroup")
            runtime_final = ("fillColor:F&&Color$1.normalize(F.rgba)"
                             if number == 1 else "style:{fontSize:$")
            cpp_final = ("cmd.vectorUniforms.color =\n"
                         "                resolvedLabelColor(style_, styleGroup, zoomLevel)"
                         if number == 1 else
                         "const float labelSizeCssPx = resolvedLabelSizePx(")
            return f"consumed-guide-{NAMES['guide'][number]}", [
                (EXTRACTOR, key), (POI_GEN, "kGuideLabelRecords"),
                (WEBGL_JS, runtime_final),
                (LABEL_CPP, final), (LAYER_CPP, cpp_final)]
        final = {
            3: (LABEL_CPP, "out.officialIconAtlas = icon.atlas"),
            4: (ASSETS_CPP, "frameContract.iconIndex - 1"),
            5: (ASSETS_CPP, "frameContract.cellWidth"),
            6: (ASSETS_CPP, "frameContract.cellHeight"),
            7: (ASSETS_CPP, "image->width != frameContract.atlasWidth"),
            8: (ASSETS_CPP, "image->height != frameContract.atlasHeight"),
        }[number]
        chain = [
            (EXTRACTOR, key), (POI_GEN, key),
            (POI_GEN, "kPoiIconRecords"),
            (LABEL_CPP, "classCode == 40001 && icon.enabled"), final]
        chain.append((LAYER_TEST_CPP, {
            3: "EXPECT_EQ(1, guideIcon.atlas)",
            4: "EXPECT_EQ(114, guideIcon.iconIndex)",
            5: "EXPECT_EQ(64, guideIcon.cellWidth)",
            6: "EXPECT_EQ(64, guideIcon.cellHeight)",
            7: "EXPECT_EQ(512, guideIcon.atlasWidth)",
            8: "EXPECT_EQ(1024, guideIcon.atlasHeight)",
        }[number]))
        return f"consumed-guide-{NAMES['guide'][number]}", chain

    key = "roofColor" if number == 1 else "wallColor"
    final = ("style.extrusionRoofColorByStyleGroup[styleGroup] = color(record.roofArgb)"
             if number == 1 else
             "style.extrusionWallColorByStyleGroup[styleGroup] = color(record.wallArgb)")
    return f"consumed-building-{key}", [
        (EXTRACTOR, key), (SURFACE_GEN, key),
        (SURFACE_CPP, final),
        (LAYER_CPP, "style_.extrusionRoofColorByStyleGroup.find(styleGroup)" if number == 1
         else "style_.extrusionWallColorByStyleGroup.find(styleGroup)")]


def observed_fields(data):
    observed = {scope: set() for scope in SCOPES}
    for number, wire, value in fields(data):
        if number != 1 or wire != 2:
            continue
        raw = nested_map(value)
        observed["style"].update(k for k in raw if isinstance(k, int))
        for field_number, scope in NESTED_SCOPE.items():
            nested = raw.get(field_number)
            if isinstance(nested, bytes):
                observed[scope].update(nested_map(nested))
    return observed


def inventory(data):
    observed = observed_fields(data)
    if observed != SCOPES:
        details = {
            scope: {"expected": sorted(SCOPES[scope]),
                    "observed": sorted(observed[scope])}
            for scope in SCOPES if observed[scope] != SCOPES[scope]
        }
        raise RuntimeError("official style field inventory changed: " +
                           json.dumps(details, sort_keys=True))
    rows = []
    for scope, numbers in SCOPES.items():
        for number in sorted(numbers):
            classification, chain = contract(scope, number)
            rows.append({
                "scope": scope,
                "field": number,
                "name": NAMES[scope][number],
                "classification": classification,
                "consumer_chain": [f"{path}::{needle}" for path, needle in chain],
            })
    return rows


def verify_consumer_chains(root, read_text=None):
    if read_text is None:
        read_text = lambda path: path.read_text(errors="replace")
    for scope, numbers in SCOPES.items():
        for number in numbers:
            _, chain = contract(scope, number)
            for relative, needle in chain:
                path = root / relative
                if not path.is_file():
                    raise RuntimeError(
                        f"{scope}#{number} consumer file missing: {relative}")
                if needle not in read_text(path):
                    raise RuntimeError(
                        f"{scope}#{number} consumer missing: {relative}::{needle}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pbf", type=Path)
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    verify_consumer_chains(args.root)
    print(json.dumps(inventory(args.pbf.read_bytes()), ensure_ascii=False,
                     indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
