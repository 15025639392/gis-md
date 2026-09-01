#!/usr/bin/env python3

import unittest
from pathlib import Path

from extract_amap_style_contract_inventory import inventory, verify_consumer_chains


ROOT = Path(__file__).resolve().parents[2]
PBF = ROOT / ".codex/artifacts/amap-label-probe-20260829/amap-style-normal-official.pbf"


class AmapStyleContractInventoryTest(unittest.TestCase):
    def test_every_observed_field_has_a_final_consumer_classification(self):
        rows = inventory(PBF.read_bytes())
        self.assertEqual(50, len(rows))
        self.assertFalse([row for row in rows
                          if row["classification"] == "missing-consumer"])
        self.assertTrue(all(row["consumer_chain"] for row in rows))
        alternate_scale = next(row for row in rows
                               if row["scope"] == "label" and
                               row["field"] == 16)
        self.assertEqual("consumed-transient-final-overwritten",
                         alternate_scale["classification"])
        verify_consumer_chains(ROOT)

    def test_top_level_fields_have_distinct_semantic_final_consumers(self):
        rows = inventory(PBF.read_bytes())
        top = {row["field"]: row for row in rows
               if row["scope"] == "style" and row["field"] <= 4}
        self.assertEqual("consumed-identity-class", top[1]["classification"])
        self.assertEqual("consumed-identity-subkey", top[2]["classification"])
        self.assertEqual("consumed-window-minimum", top[3]["classification"])
        self.assertEqual("consumed-window-maximum", top[4]["classification"])
        for field, row in top.items():
            self.assertGreaterEqual(len(row["consumer_chain"]), 7, field)
        self.assertNotEqual(top[1]["consumer_chain"], top[2]["consumer_chain"])
        self.assertNotEqual(top[3]["consumer_chain"], top[4]["consumer_chain"])

    def test_wrong_top_level_final_consumer_fails_loud(self):
        target = ROOT / "scaffold/src/earth_engine/style/AmapClassicRoadStyle.cpp"
        original = target.read_text(errors="replace")
        semantic_needle = "maxIt->second != visibility.maxZoom"
        self.assertIn(semantic_needle, original)

        def mutated_read(path):
            text = path.read_text(errors="replace")
            return text.replace(semantic_needle, "max-window-consumer-removed") \
                if path == target else text

        with self.assertRaisesRegex(RuntimeError, "style#4 consumer missing"):
            verify_consumer_chains(ROOT, read_text=mutated_read)

    def test_nested_style_containers_have_distinct_final_render_routes(self):
        rows = inventory(PBF.read_bytes())
        by_key = {(row["scope"], row["field"]): row for row in rows}
        expected = {
            5: "consumed-label-container-route",
            6: "consumed-road-container-route",
            7: "consumed-region-container-route",
            8: "consumed-guide-container-route",
            9: "consumed-building-container-route",
        }
        for field, classification in expected.items():
            self.assertEqual(classification,
                             by_key[("style", field)]["classification"])
            self.assertGreaterEqual(
                len(by_key[("style", field)]["consumer_chain"]), 4)
        self.assertEqual(5, len({by_key[("style", field)]["classification"]
                                 for field in expected}))

    def test_wrong_nested_container_final_route_fails_loud(self):
        label = ROOT / "scaffold/src/earth_engine/style/AmapClassicLabelStyle.cpp"
        layer = ROOT / "scaffold/src/earth_engine/layers/FeatureRenderLayer.cpp"
        surface = ROOT / "scaffold/src/earth_engine/style/AmapClassicSurfaceStyle.cpp"

        def mutate(target, needle):
            def read(path):
                text = path.read_text(errors="replace")
                return text.replace(needle, "semantic-route-removed") \
                    if path == target else text
            return read

        cases = [
            ("style#5 consumer missing", layer,
             "style.labelSizeExprByStyleGroup.find(styleGroup)"),
            ("style#6 consumer missing", layer,
             "auto rangeLineWidthCssPxValue = evalStrokeNumber("),
            ("style#7 consumer missing", layer,
             "cmd.kind = RenderCommandKind::VectorFill"),
            ("style#8 consumer missing", layer,
             "officialIconAtlasDemand_(resolved.officialIconAtlas)"),
            ("style#9 consumer missing", surface,
             "style.extrusionRoofColorByStyleGroup[styleGroup] = color(record.roofArgb)"),
        ]
        for error, target, needle in cases:
            with self.subTest(error=error):
                self.assertIn(needle, target.read_text(errors="replace"))
                with self.assertRaisesRegex(RuntimeError, error):
                    verify_consumer_chains(
                        ROOT, read_text=mutate(target, needle))

    def test_label_and_road_visual_fields_have_distinct_consumers(self):
        rows = inventory(PBF.read_bytes())
        by_key = {(row["scope"], row["field"]): row for row in rows}
        self.assertEqual("consumed-label-size",
                         by_key[("label", 1)]["classification"])
        self.assertEqual("consumed-label-color",
                         by_key[("label", 2)]["classification"])
        self.assertEqual("consumed-label-halo-color",
                         by_key[("label", 3)]["classification"])
        road_classes = [by_key[("road", field)]["classification"]
                        for field in range(1, 10)]
        self.assertEqual(9, len(set(road_classes)))

    def test_wrong_label_or_road_visual_consumer_fails_loud(self):
        label = ROOT / "scaffold/src/earth_engine/style/AmapClassicLabelStyle.cpp"
        road = ROOT / "scaffold/src/earth_engine/style/AmapClassicRoadStyle.cpp"
        label_needle = \
            "style.labelHaloColorExprByStyleGroup[styleGroup] = makeSteps(halos)"
        road_needle = "style.lineCasingColorExprByStyleGroup"

        def mutate(target, needle):
            def read(path):
                text = path.read_text(errors="replace")
                return text.replace(needle, "semantic-consumer-removed") \
                    if path == target else text
            return read

        with self.assertRaisesRegex(RuntimeError, "label#3 consumer missing"):
            verify_consumer_chains(ROOT, read_text=mutate(label, label_needle))
        with self.assertRaisesRegex(RuntimeError, "road#6 consumer missing"):
            verify_consumer_chains(ROOT, read_text=mutate(road, road_needle))

    def test_icon_guide_and_building_fields_have_field_specific_semantics(self):
        rows = inventory(PBF.read_bytes())
        by_key = {(row["scope"], row["field"]): row for row in rows}
        fields = ([('label', field) for field in range(4, 17)] +
                  [('label', 20), ('label', 21)] +
                  [('guide', field) for field in range(1, 9)] +
                  [('building', 1), ('building', 2)])
        classes = [by_key[key]["classification"] for key in fields]
        self.assertEqual(len(classes), len(set(classes)))
        self.assertTrue(all(len(by_key[key]["consumer_chain"]) >= 3
                            for key in fields))

    def test_wrong_atlas_and_building_consumers_fail_loud(self):
        assets = ROOT / "scaffold/src/earth_engine/style/AmapClassicAssets.cpp"
        surface = ROOT / "scaffold/src/earth_engine/style/AmapClassicSurfaceStyle.cpp"

        def mutate(target, needle):
            def read(path):
                text = path.read_text(errors="replace")
                return text.replace(needle, "semantic-consumer-removed") \
                    if path == target else text
            return read

        with self.assertRaisesRegex(RuntimeError, "label#20 consumer missing"):
            verify_consumer_chains(
                ROOT, read_text=mutate(
                    assets, "image->height != frameContract.atlasHeight"))
        with self.assertRaisesRegex(RuntimeError, "label#21 consumer missing"):
            verify_consumer_chains(
                ROOT, read_text=mutate(
                    ROOT / "scaffold/tools/extract_amap_style.py",
                    '"alternateIconAtlasHeight"'))
        with self.assertRaisesRegex(RuntimeError, "building#2 consumer missing"):
            verify_consumer_chains(
                ROOT, read_text=mutate(
                    surface,
                    "style.extrusionWallColorByStyleGroup[styleGroup] = color(record.wallArgb)"))

    def test_wrong_guide_text_final_consumers_fail_loud(self):
        webgl = ROOT / ".codex/artifacts/amap-webgl-render-20260830.js"
        layer = ROOT / "scaffold/src/earth_engine/layers/FeatureRenderLayer.cpp"

        def mutate(target, needle):
            def read(path):
                text = path.read_text(errors="replace")
                return text.replace(needle, "guide-final-consumer-removed") \
                    if path == target else text
            return read

        cases = [
            ("guide#1 consumer missing", webgl,
             "fillColor:F&&Color$1.normalize(F.rgba)"),
            ("guide#2 consumer missing", layer,
             "const float labelSizeCssPx = resolvedLabelSizePx("),
            ("guide#3 consumer missing", ROOT / "scaffold/tests/unit/layers/test_feature_render_layer.cpp",
             "EXPECT_EQ(1, guideIcon.atlas)"),
            ("guide#4 consumer missing", ROOT / "scaffold/tests/unit/layers/test_feature_render_layer.cpp",
             "EXPECT_EQ(114, guideIcon.iconIndex)"),
            ("guide#5 consumer missing", ROOT / "scaffold/tests/unit/layers/test_feature_render_layer.cpp",
             "EXPECT_EQ(64, guideIcon.cellWidth)"),
            ("guide#6 consumer missing", ROOT / "scaffold/tests/unit/layers/test_feature_render_layer.cpp",
             "EXPECT_EQ(64, guideIcon.cellHeight)"),
            ("guide#7 consumer missing", ROOT / "scaffold/tests/unit/layers/test_feature_render_layer.cpp",
             "EXPECT_EQ(512, guideIcon.atlasWidth)"),
            ("guide#8 consumer missing", ROOT / "scaffold/tests/unit/layers/test_feature_render_layer.cpp",
             "EXPECT_EQ(1024, guideIcon.atlasHeight)"),
        ]
        for error, target, needle in cases:
            with self.subTest(error=error):
                self.assertIn(needle, target.read_text(errors="replace"))
                with self.assertRaisesRegex(RuntimeError, error):
                    verify_consumer_chains(
                        ROOT, read_text=mutate(target, needle))

    def test_control_transient_flags_and_region_fields_are_semantically_distinct(self):
        rows = inventory(PBF.read_bytes())
        by_key = {(row["scope"], row["field"]): row for row in rows}
        expected = {
            ("style", 10): "consumed-road-field-reset-control",
            ("style", 11): "consumed-record-continuation-control",
            ("label", 16): "consumed-transient-final-overwritten",
            ("label", 17): "consumed-visible-bit1+decoded-nonrendering-bit4",
            ("region", 1): "consumed-region-fill-color",
        }
        for key, classification in expected.items():
            self.assertEqual(classification, by_key[key]["classification"])
        self.assertEqual(len(expected), len({by_key[key]["classification"]
                                             for key in expected}))

    def test_wrong_control_transient_flags_or_region_consumer_fails_loud(self):
        extractor = ROOT / "scaffold/tools/extract_amap_style.py"
        poi_gen = ROOT / "scaffold/tools/generate_amap_poi_data.py"
        road_gen = ROOT / "scaffold/tools/generate_amap_road_width_data.py"
        label = ROOT / "scaffold/src/earth_engine/style/AmapClassicLabelStyle.cpp"
        layer = ROOT / "scaffold/src/earth_engine/layers/FeatureRenderLayer.cpp"
        layer_test = ROOT / "scaffold/tests/unit/layers/test_feature_render_layer.cpp"

        def mutate(target, needle):
            def read(path):
                text = path.read_text(errors="replace")
                return text.replace(needle, "semantic-consumer-removed") \
                    if path == target else text
            return read

        cases = [
            ("label#4 consumer missing", layer_test,
             "Official label field 4 final atlas consumer."),
            ("label#5 consumer missing", layer_test,
             "Official label field 5 final one-based icon-index consumer."),
            ("label#6 consumer missing", layer_test,
             "Official label field 6 final cell-width consumer."),
            ("label#7 consumer missing", layer_test,
             "Official label field 7 final cell-height consumer."),
            ("label#8 consumer missing", layer_test,
             "Official label field 8 final source-atlas-width consumer."),
            ("label#9 consumer missing", layer_test,
             "Official label field 9 final display-height consumer."),
            ("label#10 consumer missing", layer_test,
             "Official label field 10 final display-width consumer."),
            ("label#11 consumer missing", layer_test,
             "EXPECT_EQ(4, dynamic.atlas)"),
            ("label#12 consumer missing", layer_test,
             "EXPECT_EQ(73, dynamic.iconIndex)"),
            ("label#13 consumer missing", layer_test,
             "EXPECT_EQ(64, dynamic.cellWidth)"),
            ("label#14 consumer missing", layer_test,
             "EXPECT_EQ(64, dynamic.cellHeight)"),
            ("label#15 consumer missing", layer_test,
             "EXPECT_EQ(512, dynamic.atlasWidth)"),
            ("label#1 consumer missing", layer_test,
             "Provider zoom 5 changes only field 1"),
            ("label#2 consumer missing", layer_test,
             "Official label field 2 final text-color consumer."),
            ("label#3 consumer missing", layer_test,
             "Official label field 3 final halo-color consumer."),
            ("road#1 consumer missing", layer_test,
             "AmapOfficialDashUsesBinaryRetinaScaleExactlyOnce"),
            ("road#2 consumer missing", layer_test,
             "OfficialCenterAndCasingLineTypesReachIndependentFinalCommands"),
            ("road#3 consumer missing", layer_test,
             "Official road field 3 final center-color consumer."),
            ("road#4 consumer missing", layer_test,
             "Official road field 4 final signed casing-width consumer."),
            ("road#5 consumer missing", layer_test,
             "Official road field 5 final center-width consumer."),
            ("road#6 consumer missing", layer_test,
             "Official road field 6 final casing-color consumer."),
            ("road#7 consumer missing", layer_test,
             "Official road label field 7 final size consumer:"),
            ("road#8 consumer missing", layer_test,
             "Official road label field 8 final text-color consumer."),
            ("road#9 consumer missing", layer_test,
             "Official road label field 9 final halo-color consumer."),
            ("style#10 consumer missing", road_gen,
             'elif record["fresh"] and (active or reset_before_active):'),
            ("style#11 consumer missing", extractor,
             "current[4] = previous[4] + 1"),
            ("label#16 consumer missing", poi_gen,
             "math.floor(cell_w / scale)"),
            ("label#17 consumer missing", label,
             "out.officialCanCovered = (flags & 1) != 0"),
            ("region#1 consumer missing", layer,
             "cmd.vectorUniforms.color = value->color()"),
        ]
        for error, target, needle in cases:
            with self.subTest(error=error):
                self.assertIn(needle, target.read_text(errors="replace"))
                with self.assertRaisesRegex(RuntimeError, error):
                    verify_consumer_chains(
                        ROOT, read_text=mutate(target, needle))

if __name__ == "__main__":
    unittest.main()
