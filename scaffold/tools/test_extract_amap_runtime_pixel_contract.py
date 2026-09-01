#!/usr/bin/env python3

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scaffold/tools/extract_amap_runtime_pixel_contract.py"
spec = importlib.util.spec_from_file_location("amap_pixel_contract", SCRIPT)
module = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(module)


class AmapRuntimePixelContractTest(unittest.TestCase):
    def test_fixed_official_artifacts_replay_complete_chain(self):
        result = module.extract(
            ROOT / ".codex/artifacts/amap-webapi-maps-2.3.5.6.js",
            ROOT / ".codex/artifacts/amap-webgl-render-20260830.js",
            ROOT / ".codex/artifacts/amap-label-probe-20260829/amap-style-normal-official.pbf")
        self.assertEqual(
            "4b41a499c3052fe0242407099b69d8e2798bb4679adc483844cb9734f984de38",
            result["artifacts"]["runtime"]["sha256"])
        self.assertEqual(
            "2d2dd711b09aeb320c4e4279b4322624f0b44a88cbb9e3de01d5675b6de243f5",
            result["artifacts"]["webglRender"]["sha256"])
        signature = result["numericSignature"]
        self.assertEqual(14, signature["providerZoom"])
        self.assertEqual(8.0, signature["roadWidthCssPx"])
        self.assertEqual(2.0, signature["signedBorderWidthCssPx"])
        self.assertEqual(10.0, signature["secondaryCssPx"])
        self.assertEqual([1280, 720],
                         signature["officialRetinaBranches"]["1x"]["physicalViewport"])
        self.assertEqual([2560, 1440],
                         signature["officialRetinaBranches"]["2x"]["physicalViewport"])
        self.assertEqual(16.0,
                         signature["officialRetinaBranches"]["2x"]["centerPhysicalPx"])
        self.assertEqual(20.0,
                         signature["officialRetinaBranches"]["2x"]["secondaryPhysicalPx"])
        self.assertEqual([0, 1, 0, 1],
                         result["poiTextLayoutContract"]["paddingCssPx"])
        self.assertEqual(["Poi", "Guide"],
                         result["poiTextLayoutContract"]["formatterBranches"])
        flags = result["poiFlagContract"]
        self.assertEqual({"canCovered": 1, "Y8t": 4, "X8t": 8},
                         flags["bitMasks"])
        self.assertEqual(7, flags["Y8tOccurrenceCount"])
        self.assertIsNone(flags["observableConsumer"])
        self.assertEqual("decoded-forwarded-no-observable-consumer",
                         flags["semantics"])
        self.assertEqual(4, len(flags["completeConsumersPresent"]))
        self.assertEqual(9, len(result["consumerChain"]))

    def test_duplicate_or_missing_needle_fails_loudly(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "runtime.js"
            path.write_text("roadWidth:t.lineWidth||0" * 2)
            with self.assertRaisesRegex(ValueError, "must match once"):
                module.unique_offsets(path.read_text(), module.MAIN_REQUIRED)

    def test_y8t_new_consumer_fails_loudly(self):
        runtime = (ROOT / ".codex/artifacts/amap-webapi-maps-2.3.5.6.js").read_text()
        with self.assertRaisesRegex(ValueError, "Y8t contract changed"):
            module.official_poi_flag_contract(runtime + "Y8t")


if __name__ == "__main__":
    unittest.main()
