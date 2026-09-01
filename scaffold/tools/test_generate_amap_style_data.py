#!/usr/bin/env python3

import subprocess
import sys
import tempfile
import unittest
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "scaffold/tools"
STYLE = ROOT / ".codex/artifacts/amap-label-probe-20260829/amap-style-normal-official.pbf"
RUNTIME = ROOT / ".codex/artifacts/amap-webapi-maps-2.3.5.6.js"
OUTPUTS = ROOT / "scaffold/src/earth_engine/style"
sys.path.insert(0, str(TOOLS))

from extract_amap_style import extract
from generate_amap_road_width_data import (
    casing_identities,
    curves as road_curves,
    identities as road_identities,
    label_identities,
    label_curves,
    record_drawable,
    visibility_windows,
)
from generate_amap_poi_data import (
    GUIDE_SHIELD_WIDTH_CONTRACT,
    POI_MULTILINE_SPLIT_CONTRACT,
    emit_label_records,
    extract_runtime_layout,
    icon_row,
    q8t_provisional_size,
    validate_guide_shield_width_contract,
    validate_poi_multiline_split_contract,
)
from generate_amap_surface_data import emit_buildings, emit_surfaces
from generate_amap_line_type_data import generate as generate_line_types


class AmapGeneratedStyleDataTest(unittest.TestCase):
    def run_generator(self, script, output, *arguments):
        subprocess.run(
            [sys.executable, str(TOOLS / script), *map(str, arguments),
             "-o", str(output)],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True)

    def test_fixed_official_artifacts_rebuild_every_committed_table(self):
        with tempfile.TemporaryDirectory() as directory:
            generated = Path(directory)
            cases = [
                ("generate_amap_surface_data.py", "surface.inc",
                 "AmapClassicSurfaceData.inc", (STYLE,)),
                ("generate_amap_road_width_data.py", "road.inc",
                 "AmapClassicRoadWidthData.inc", (STYLE,)),
                ("generate_amap_line_type_data.py", "line.inc",
                 "AmapClassicLineTypeData.inc", (RUNTIME,)),
                ("generate_amap_poi_data.py", "poi.inc",
                 "AmapClassicPoiData.inc",
                 (STYLE, "--runtime-js", RUNTIME,
                  "--access-oversea", "1")),
            ]
            for script, temporary_name, committed_name, arguments in cases:
                with self.subTest(table=committed_name):
                    temporary = generated / temporary_name
                    self.run_generator(script, temporary, *arguments)
                    self.assertEqual(
                        (OUTPUTS / committed_name).read_bytes(),
                        temporary.read_bytes(),
                        f"{committed_name} must be generated only from the "
                        "fixed official artifacts")

    def test_guide_shield_width_uses_javascript_utf16_length(self):
        runtime = RUNTIME.read_text()
        validate_guide_shield_width_contract(runtime)
        for needle in GUIDE_SHIELD_WIDTH_CONTRACT:
            self.assertEqual(1, runtime.count(needle))
        # Five non-BMP scalars occupy ten UTF-16 code units in JavaScript.
        text = "\U00020000\U00020001\U00020002\U00020003\U00020004"
        utf16_units = len(text.encode("utf-16-le")) // 2
        self.assertEqual(10, utf16_units)
        self.assertEqual(2.5, max(1.0, utf16_units / 4.0))

    def test_guide_shield_width_contract_fails_loudly_on_drift(self):
        runtime = RUNTIME.read_text()
        for needle in GUIDE_SHIELD_WIDTH_CONTRACT:
            with self.subTest(missing=needle):
                with self.assertRaisesRegex(RuntimeError, "must match once"):
                    validate_guide_shield_width_contract(
                        runtime.replace(needle, "", 1))
            with self.subTest(duplicate=needle):
                with self.assertRaisesRegex(RuntimeError, "must match once"):
                    validate_guide_shield_width_contract(runtime + needle)

        width = GUIDE_SHIELD_WIDTH_CONTRACT[1]
        relocated = runtime.replace(width, "", 1) + width
        with self.assertRaisesRegex(RuntimeError, "moved outside"):
            validate_guide_shield_width_contract(relocated)

        wrong_source = runtime.replace("x=b.shield,M=b.shieldType",
                                       "x=b.name,M=b.shieldType", 1)
        with self.assertRaisesRegex(RuntimeError, "must match once"):
            validate_guide_shield_width_contract(wrong_source)

    def test_poi_multiline_uses_open_utf16_split_indices(self):
        runtime = RUNTIME.read_text()
        validate_poi_multiline_split_contract(runtime)
        for needle in POI_MULTILINE_SPLIT_CONTRACT:
            self.assertEqual(1, runtime.count(needle))
        # The helper always appends substring(n) after the supplied indices;
        # therefore the final split is not required to equal string length.
        self.assertIn('return(r+=t.substring(n))',
                      POI_MULTILINE_SPLIT_CONTRACT[3])

    def test_poi_multiline_split_contract_fails_loudly_on_drift(self):
        runtime = RUNTIME.read_text()
        for needle in POI_MULTILINE_SPLIT_CONTRACT:
            with self.subTest(missing=needle):
                with self.assertRaisesRegex(RuntimeError, "must match once"):
                    validate_poi_multiline_split_contract(
                        runtime.replace(needle, "", 1))
            with self.subTest(duplicate=needle):
                with self.assertRaisesRegex(RuntimeError, "must match once"):
                    validate_poi_multiline_split_contract(runtime + needle)

        suffix = POI_MULTILINE_SPLIT_CONTRACT[3]
        relocated = runtime.replace(suffix, "", 1) + suffix
        with self.assertRaisesRegex(RuntimeError, "moved outside"):
            validate_poi_multiline_split_contract(relocated)

        loop = POI_MULTILINE_SPLIT_CONTRACT[1]
        with self.assertRaisesRegex(RuntimeError, "must match once"):
            validate_poi_multiline_split_contract(
                runtime.replace(loop, loop.replace("n=0", "n=1"), 1))
        with self.assertRaisesRegex(RuntimeError, "must match once"):
            validate_poi_multiline_split_contract(
                runtime.replace(loop,
                                loop.replace("i<e.length", "i<e.length-1"),
                                1))
        helper = POI_MULTILINE_SPLIT_CONTRACT[0]
        with self.assertRaisesRegex(RuntimeError, "must match once"):
            validate_poi_multiline_split_contract(
                runtime.replace(helper, helper.replace("t.trim()", "t"), 1))
        suffix = POI_MULTILINE_SPLIT_CONTRACT[3]
        with self.assertRaisesRegex(RuntimeError, "must match once"):
            validate_poi_multiline_split_contract(
                runtime.replace(suffix,
                                suffix.replace('replace(/\\s+$/,"")',
                                               'replace(/ +$/,"")'),
                                1))

    def test_line_type_rejects_the_wrong_official_bundle(self):
        webgl = ROOT / ".codex/artifacts/amap-webgl-render-20260830.js"
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable,
                 str(TOOLS / "generate_amap_line_type_data.py"),
                 str(webgl), "-o", str(Path(directory) / "line.inc")],
                cwd=ROOT,
                capture_output=True,
                text=True)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("must match once", result.stderr)

    def test_line_type_preserves_the_official_unknown_default(self):
        generated = generate_line_types(RUNTIME)
        self.assertIn(
            "static constexpr LineTypeRecord kOfficialUnknownLineType =",
            generated)
        self.assertIn(
            "{-1, {{0, 0, 0, 0}}, 0, LineCap::Butt};",
            generated)
        self.assertRegex(
            RUNTIME.read_text(errors="ignore"),
            r"function getLineTypeStyle\(t\)\{switch\(t\)\{.*?"
            r"default:return\"solid\";?\}\}")

    def test_surface_generator_omits_unreachable_official_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "surface.inc"
            self.run_generator("generate_amap_surface_data.py", output,
                               STYLE)
            generated = output.read_text()
            self.assertNotIn("{30001, 30, 14, 13,", generated)
            self.assertNotIn("{30001, 31, 14, 13,", generated)
            self.assertNotIn("{30001, 34, 13, 10,", generated)
            self.assertIn("{30001, 34, 14, 30, 0xFFE0F4B1u}", generated)

            # No future official artifact may reintroduce an executable
            # reversed zoom interval through this generator.
            records = re.findall(
                r"\{\d+, \d+, (\d+), (\d+), 0x[0-9A-F]+u\}",
                generated)
            self.assertTrue(records)
            self.assertTrue(all(int(lo) <= int(hi)
                                for lo, hi in records))

    def test_every_surface_and_building_contract_matches_official_at_every_zoom(self):
        styles = extract(STYLE.read_bytes())["styles"]
        surface = emit_surfaces(styles)
        rows = [
            (int(cls), int(sub), int(minimum), int(maximum), int(color, 16))
            for cls, sub, minimum, maximum, color in re.findall(
                r"\{(\d+), (\d+), (\d+), (\d+), 0x([0-9A-F]+)u\}",
                surface)]
        identities = {(int(class_code), record["subKey"])
                      for class_code, records in styles.items()
                      for record in records
                      if record.get("surfaceFillColor") is not None}
        for class_code, subkey in identities:
            raw = [record for record in styles[str(class_code)]
                   if record["subKey"] == subkey and
                   record.get("surfaceFillColor") is not None and
                   record["minZoom"] <= record["maxZoom"]]
            for display_zoom in range(30):
                provider_zoom = display_zoom + 1
                active = [record for record in raw
                          if record["minZoom"] <= provider_zoom <=
                          record["maxZoom"]]
                expected = (int(active[-1]["surfaceFillColor"][1:], 16)
                            if active else 0)
                compiled = [row for row in rows
                            if row[0] == class_code and row[1] == subkey and
                            row[2] <= provider_zoom <= row[3]]
                actual = compiled[-1][4] if compiled else 0
                self.assertEqual(expected, actual,
                                 (class_code, subkey, display_zoom))

        buildings = emit_buildings(styles)
        building_rows = [
            (int(sub), int(minimum), int(maximum),
             int(roof, 16), int(wall, 16))
            for sub, minimum, maximum, roof, wall in re.findall(
                r"\{(\d+), (\d+), (\d+), 0x([0-9A-F]+)u, "
                r"0x([0-9A-F]+)u\}", buildings)]
        expected_buildings = [
            (record["subKey"], record["minZoom"], record["maxZoom"],
             int(record["roofColor"][1:], 16),
             int(record["wallColor"][1:], 16))
            for record in styles["55001"]
            if record.get("roofColor") is not None and
            record.get("wallColor") is not None and
            record["minZoom"] <= record["maxZoom"]]
        self.assertEqual(expected_buildings, building_rows)

        conflicting = {
            "30001": [
                {"subKey": 1, "minZoom": 3, "maxZoom": 7,
                 "surfaceFillColor": "#ff000000"},
                {"subKey": 1, "minZoom": 6, "maxZoom": 8,
                 "surfaceFillColor": "#ffffffff"},
            ]}
        with self.assertRaisesRegex(RuntimeError,
                                    "surface contracts disagree"):
            emit_surfaces(conflicting)

    def test_poi_flag_generator_emits_only_unambiguous_reachable_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "poi.inc"
            self.run_generator(
                "generate_amap_poi_data.py", output, STYLE,
                "--runtime-js", RUNTIME, "--access-oversea", "1")
            generated = output.read_text()
            block = re.search(
                r"kPoiFlagRecords\[\] = \{(.*?)\n\};", generated,
                re.DOTALL)
            self.assertIsNotNone(block)
            records = [tuple(map(int, row)) for row in re.findall(
                r"\{(\d+), (\d+), (\d+), (\d+), (\d+)\}",
                block.group(1))]
            self.assertTrue(records)
            grouped = {}
            for cls, sub, minimum, maximum, flags in records:
                self.assertLess(minimum, maximum)
                grouped.setdefault((cls, sub), []).append(
                    (minimum, maximum, flags))
            for windows in grouped.values():
                for previous, current in zip(windows, windows[1:]):
                    self.assertLessEqual(previous[1], current[0])

            self.assertNotIn((12025, 6), grouped)
            self.assertEqual([(1, 16, 4), (16, 30, 4)],
                             grouped[(12025, 4)])

    def test_poi_label_generator_removes_unreachable_and_redundant_windows(self):
        styles = extract(STYLE.read_bytes())["styles"]
        generated = emit_label_records(styles)
        self.assertNotIn("{120250007, 19, 18,", generated)
        self.assertNotIn("{120250004, 13, 14,", generated)
        self.assertEqual(1, generated.count("{120250004, 2, 16,"))

        conflicting = {
            "10001": [
                {"subKey": 1, "minZoom": 3, "maxZoom": 7,
                 "labelSize": 10, "poiLabelColor": "#ff000000",
                 "poiLabelCasingColor": "#ffffffff"},
                {"subKey": 1, "minZoom": 6, "maxZoom": 8,
                 "labelSize": 12, "poiLabelColor": "#ff000000",
                 "poiLabelCasingColor": "#ffffffff"},
            ]}
        with self.assertRaisesRegex(RuntimeError, "label contracts disagree"):
            emit_label_records(conflicting)

    def test_poi_icon_anchor_offsets_follow_runtime_nn_an_fields(self):
        styles = extract(STYLE.read_bytes())["styles"]
        point_records = [record for class_code, records in styles.items()
                         if 10000 <= int(class_code) < 20000
                         for record in records]
        self.assertTrue(point_records)
        self.assertTrue(all(record.get("iconAnchorOffsetX") is None and
                            record.get("iconAnchorOffsetY") is None
                            for record in point_records))

        record = next(record.copy() for record in point_records
                      if record.get("iconCellWidth") and
                      record.get("iconDisplayWidthRetina"))
        record["iconAnchorOffsetX"] = 6
        record["iconAnchorOffsetY"] = -3
        generated = icon_row(10001, record, False)
        self.assertIsNotNone(generated)
        self.assertTrue(generated.endswith(", 6.0f, -3.0f},"))

    def test_q8t_alternate_scale_consumes_official_transient_formula(self):
        styles = extract(STYLE.read_bytes())["styles"]
        records = [record for class_code, class_records in styles.items()
                   if 10000 <= int(class_code) < 20000
                   for record in class_records
                   if record.get("alternateScale") is not None]
        self.assertEqual(18, len(records))
        self.assertEqual({2}, {record["alternateScale"]
                               for record in records})
        sizes = {q8t_provisional_size(record) for record in records}
        self.assertIn((32, 32), sizes)
        self.assertIn((64, 64), sizes)
        for record in records:
            generated = icon_row(10001, record, True)
            self.assertIsNotNone(generated)
            self.assertRegex(generated, r", 0\.0f, 0\.0f(?:,|\})")

        all_alternates = [
            record for class_code, class_records in styles.items()
            if 10000 <= int(class_code) < 20000
            for record in class_records
            if record.get("alternateIconAtlas") and
            record.get("alternateIconIndex") and
            record.get("alternateIconCellWidth") and
            record.get("alternateIconCellHeight") and
            record.get("alternateIconAtlasWidth") and
            record.get("alternateIconAtlasHeight")]
        self.assertEqual(27, len(all_alternates))
        unscaled = [record for record in all_alternates
                    if record.get("alternateScale") is None]
        self.assertEqual(9, len(unscaled))
        for record in unscaled:
            self.assertEqual(
                (record["alternateIconCellWidth"],
                 record["alternateIconCellHeight"]),
                q8t_provisional_size(record))

    def test_q8t_alternate_scale_rejects_missing_or_non_positive_values(self):
        base = {
            "alternateIconCellWidth": 64,
            "alternateIconCellHeight": 128,
        }
        self.assertEqual((64, 128), q8t_provisional_size(base))
        for scale in (0, -1):
            with self.subTest(scale=scale):
                record = dict(base)
                record["alternateScale"] = scale
                with self.assertRaisesRegex(
                        RuntimeError, "alternateScale must be a positive"):
                    q8t_provisional_size(record)

    def test_every_poi_contract_matches_official_at_every_display_zoom(self):
        styles = extract(STYLE.read_bytes())["styles"]
        point_records = [(int(class_code), record)
                         for class_code, records in styles.items()
                         if 10000 <= int(class_code) < 20000
                         for record in records]
        committed = (OUTPUTS / "AmapClassicPoiData.inc").read_text()

        def block(name):
            match = re.search(rf"{name}\[\] = \{{(.*?)\n\}};", committed,
                              re.DOTALL)
            self.assertIsNotNone(match, name)
            return match.group(1)

        label_rows = [
            (int(identity_value), int(minimum), int(maximum), float(size),
             int(text, 16), int(halo, 16))
            for identity_value, minimum, maximum, size, text, halo in re.findall(
                r"\{(\d+), (\d+), (\d+), ([0-9.]+)f, "
                r"0x([0-9A-F]+)u, 0x([0-9A-F]+)u\}",
                block("kPoiLabelRecords"))]
        icon_pattern = (
            r"\{(\d+), (\d+), (\d+), (\d+), (\d+), (\d+), "
            r"(\d+), (\d+), (\d+), (\d+), ([0-9.]+)f, "
            r"([0-9.]+)f(?:, (-?[0-9.]+)f, (-?[0-9.]+)f)?\}")
        def icon_rows(name):
            return [
                (int(cls), int(sub), int(minimum), int(maximum), int(atlas),
                 int(index), int(cell_w), int(cell_h), int(atlas_w),
                 int(atlas_h), float(display_w), float(display_h),
                 float(anchor_x or 0), float(anchor_y or 0))
                for cls, sub, minimum, maximum, atlas, index, cell_w,
                    cell_h, atlas_w, atlas_h, display_w, display_h,
                    anchor_x, anchor_y in re.findall(icon_pattern, block(name))]
        fixed_rows = icon_rows("kPoiIconRecords")
        dynamic_rows = icon_rows("kPoiDynamicTextBackgroundRecords")
        flag_rows = [tuple(map(int, row)) for row in re.findall(
            r"\{(\d+), (\d+), (\d+), (\d+), (\d+)\}",
            block("kPoiFlagRecords"))]

        identities = {(cls, record["subKey"])
                      for cls, record in point_records}
        for cls, sub in sorted(identities):
            raw = [record for record_cls, record in point_records
                   if record_cls == cls and record["subKey"] == sub]
            for zoom in range(30):
                valid_labels = [record for record in raw
                                if record.get("labelSize") is not None and
                                record["minZoom"] <= record["maxZoom"]]
                expected_label = None
                active_labels = [record for record in valid_labels
                                 if record["minZoom"] - 1 <= zoom <
                                 record["maxZoom"]]
                if active_labels:
                    chosen = max(active_labels,
                                 key=lambda record: record["minZoom"])
                    expected_label = (
                        float(chosen["labelSize"]),
                        int(chosen["poiLabelColor"][1:], 16),
                        int((chosen["poiLabelCasingColor"] or
                             "#00000000")[1:], 16))
                compiled_labels = [row for row in label_rows
                                   if row[0] == cls * 10000 + sub and
                                   row[1] - 1 <= zoom < row[2]]
                actual_label = None
                if compiled_labels:
                    chosen = max(compiled_labels, key=lambda row: row[1])
                    actual_label = (chosen[3], chosen[4], chosen[5])
                self.assertEqual(expected_label, actual_label,
                                 (cls, sub, zoom, "label"))

                def expected_icon(alternate):
                    prefix = "alternateIcon" if alternate else "icon"
                    candidates = [record for record in raw
                                  if record["minZoom"] - 1 <= zoom <
                                  record["maxZoom"] and
                                  record.get(prefix + "Atlas") and
                                  record.get(prefix + "Index") and
                                  all(record.get(prefix + suffix) for suffix in
                                      ("CellWidth", "CellHeight",
                                       "AtlasWidth", "AtlasHeight"))]
                    if not candidates:
                        return None
                    record = candidates[0]
                    atlas = int(record[prefix + "Atlas"].removeprefix("icons_"))
                    index = record[prefix + "Index"]
                    if not alternate and atlas == 1 and index == 128:
                        return None
                    display_w = (0.0 if alternate else
                                 record["iconDisplayWidthRetina"] / 2.0)
                    display_h = (0.0 if alternate else
                                 record["iconDisplayHeightRetina"] / 2.0)
                    return (atlas, index, record[prefix + "CellWidth"],
                            record[prefix + "CellHeight"],
                            record[prefix + "AtlasWidth"],
                            record[prefix + "AtlasHeight"], display_w,
                            display_h,
                            10.0 - 20.0 * (record.get("iconAnchorOffsetX") or 0) / 24.0,
                            10.0 - 20.0 * (record.get("iconAnchorOffsetY") or 0) / 24.0)

                def compiled_icon(rows):
                    candidates = [row for row in rows if row[0] == cls and
                                  row[1] == sub and row[2] <= zoom < row[3]]
                    if not candidates:
                        return None
                    row = candidates[0]
                    return (row[4], row[5], row[6], row[7], row[8], row[9],
                            row[10], row[11],
                            10.0 - 20.0 * row[12] / 24.0,
                            10.0 - 20.0 * row[13] / 24.0)

                expected_dynamic = expected_icon(True)
                actual_dynamic = compiled_icon(dynamic_rows)
                self.assertEqual(expected_dynamic, actual_dynamic,
                                 (cls, sub, zoom, "dynamic"))
                expected_fixed = None if expected_dynamic else expected_icon(False)
                actual_fixed = None if actual_dynamic else compiled_icon(fixed_rows)
                self.assertEqual(expected_fixed, actual_fixed,
                                 (cls, sub, zoom, "fixed"))

                expected_flags = next(
                    (record.get("iconFlags") or 0 for record in raw
                     if record["minZoom"] - 1 <= zoom < record["maxZoom"] and
                     (record.get("iconFlags") or 0)), 0)
                actual_flags = next(
                    (flags for row_cls, row_sub, minimum, maximum, flags
                     in flag_rows if row_cls == cls and row_sub == sub and
                     minimum <= zoom < maximum), 0)
                self.assertEqual(expected_flags, actual_flags,
                                 (cls, sub, zoom, "flags"))

        runtime = RUNTIME.read_text()
        expected_cities, expected_directions = extract_runtime_layout(runtime)
        actual_cities = re.findall(
            r'\{"([^"]+)", ([0-9.]+), ([0-9.]+)\}',
            block("kPoiCityWindowRecords"))
        direction_names = {"0": "center", "1": "right", "2": "left",
                           "3": "top", "4": "bottom"}
        actual_directions = [
            (name, direction_names[direction], x, y)
            for name, direction, x, y in re.findall(
                r'\{"([^"]+)", (\d+), (-?[0-9.]+)f, (-?[0-9.]+)f\}',
                block("kPoiDirectionRecords"))]
        self.assertEqual(expected_cities, actual_cities)
        self.assertEqual(
            [(name, direction, float(x), float(y))
             for name, direction, x, y in expected_directions],
            [(name, direction, float(x), float(y))
             for name, direction, x, y in actual_directions])

    def test_every_road_label_record_is_a_complete_contiguous_contract(self):
        styles = extract(STYLE.read_bytes())["styles"]
        identities = label_identities(styles)
        fields = ("labelSize", "roadLabelColor",
                  "roadLabelCasingColor")
        identity_count = 0
        record_count = 0
        for class_code, subkeys in identities.items():
            for subkey in subkeys:
                identity_count += 1
                records = [record for record in styles[str(class_code)]
                           if record["subKey"] == subkey and
                           any(record.get(field) is not None
                               for field in fields)]
                record_count += len(records)
                self.assertTrue(records)
                self.assertTrue(all(all(record.get(field) is not None
                                         for field in fields)
                                    for record in records))
                covered = set()
                for record in records:
                    self.assertLessEqual(record["minZoom"],
                                         record["maxZoom"])
                    covered.update(range(record["minZoom"],
                                         record["maxZoom"] + 1))
                self.assertEqual(set(range(min(covered), max(covered) + 1)),
                                 covered)
        self.assertEqual(285, identity_count)
        self.assertEqual(3329, record_count)

    def test_every_drawable_road_identity_has_valid_contiguous_windows(self):
        styles = extract(STYLE.read_bytes())["styles"]
        identities = road_identities(styles)
        identity_count = 0
        for class_code, subkeys in identities.items():
            for subkey in subkeys:
                identity_count += 1
                records = [record for record in styles[str(class_code)]
                           if record["subKey"] == subkey]
                self.assertTrue(records)
                previous_max = None
                drawable_zooms = set()
                for record in records:
                    self.assertLessEqual(record["minZoom"],
                                         record["maxZoom"])
                    if previous_max is not None:
                        self.assertLess(previous_max, record["minZoom"])
                    previous_max = record["maxZoom"]
                    if record_drawable(record):
                        drawable_zooms.update(range(record["minZoom"],
                                                    record["maxZoom"] + 1))
                self.assertTrue(drawable_zooms)
                self.assertEqual(
                    set(range(min(drawable_zooms),
                              max(drawable_zooms) + 1)),
                    drawable_zooms)
        self.assertEqual(429, identity_count)

    def test_every_road_contract_matches_official_at_every_provider_zoom(self):
        styles = extract(STYLE.read_bytes())["styles"]
        identities = road_identities(styles)
        visibility = dict(
            (style_group, (minimum, maximum))
            for style_group, minimum, maximum in visibility_windows(
                styles, identities, record_drawable))
        fields = (
            ("lineWidth", 0, False),
            ("casingWidth", 0, False),
            ("roadColor", None, True),
            ("casing", None, True),
            ("lineType", 0, False),
            ("casingLineType", 0, False),
        )
        evaluations = 0
        for field, reset, reset_before_active in fields:
            generated = dict(road_curves(
                styles, identities, field, 30, reset,
                reset_before_active))
            for class_code, subkeys in identities.items():
                for subkey in subkeys:
                    style_group = class_code * 1000 + subkey
                    records = [record for record in styles[str(class_code)]
                               if record["subKey"] == subkey]
                    minimum, maximum = visibility[style_group]
                    for provider_zoom in range(minimum, maximum + 1):
                        active = [record for record in records
                                  if record["minZoom"] <= provider_zoom <=
                                  record["maxZoom"]]
                        expected = (active[-1].get(field)
                                    if active else reset)
                        if expected is None:
                            expected = reset
                        actual = reset
                        for stop_zoom, value in generated[style_group]:
                            if stop_zoom <= provider_zoom:
                                actual = value
                        self.assertEqual(
                            expected, actual,
                            (class_code, subkey, provider_zoom, field))
                        evaluations += 1
        self.assertGreater(evaluations, 50000)

        expected_casing = {
            (class_code, subkey)
            for class_code, subkeys in identities.items()
            for subkey in subkeys
            if any(record["subKey"] == subkey and
                   ((record.get("lineWidth") or 0) +
                    (record.get("casingWidth") or 0)) > 0 and
                   record.get("casing") is not None
                   for record in styles[str(class_code)])}
        actual_casing = {
            (class_code, subkey)
            for class_code, subkeys in casing_identities(
                styles, identities).items()
            for subkey in subkeys}
        self.assertEqual(expected_casing, actual_casing)

        label_identity_map = label_identities(styles)
        label_predicate = lambda record: any(
            record.get(field) is not None for field in
            ("labelSize", "roadLabelColor", "roadLabelCasingColor"))
        label_visibility = dict(
            (style_group, (minimum, maximum))
            for style_group, minimum, maximum in visibility_windows(
                styles, label_identity_map, label_predicate))
        for field in ("labelSize", "roadLabelColor",
                      "roadLabelCasingColor"):
            generated = dict(label_curves(
                styles, label_identity_map, field, 30))
            for class_code, subkeys in label_identity_map.items():
                for subkey in subkeys:
                    style_group = class_code * 1000 + subkey
                    records = [record for record in styles[str(class_code)]
                               if record["subKey"] == subkey]
                    minimum, maximum = label_visibility[style_group]
                    for provider_zoom in range(minimum, maximum + 1):
                        active = [record for record in records
                                  if record["minZoom"] <= provider_zoom <=
                                  record["maxZoom"]]
                        expected = active[-1].get(field) if active else None
                        actual = None
                        for stop_zoom, value in generated.get(style_group, []):
                            if stop_zoom <= provider_zoom:
                                actual = value
                        self.assertEqual(
                            expected, actual,
                            (class_code, subkey, provider_zoom, field))


if __name__ == "__main__":
    unittest.main()
