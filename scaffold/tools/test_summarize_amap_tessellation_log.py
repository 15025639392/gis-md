#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scaffold/tools/summarize_amap_tessellation_log.py"
spec = importlib.util.spec_from_file_location("amap_tess_summary", SCRIPT)
module = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(module)


class AmapTessellationSummaryTest(unittest.TestCase):
    def test_accepts_android_brief_pid_prefix(self):
        result = module.summarize(
            "I/VectorTessSlow(18164): amap-regions z=10 x=817 y=341 "
            "features=20 convert=4.56ms tess=2396.94ms admit=0.42ms "
            "polygon=2383.57ms line=0.02ms polySetup=5.89ms "
            "polyDensify=3.53ms polyIntersect=145.89ms polyCdt=2219.47ms "
            "polyEcef=1.22ms cdtSuper=0.03ms cdtPoint=1962.72ms "
            "cdtConstraint=177.97ms cdtExtract=46.61ms extrude=0.00ms "
            "symbol=0.00ms accepted=7 rejected=13 polyN=6 lineN=1 "
            "extrudeN=0 symbolN=0 rings=472 points=10224 "
            "slowest=2195.77ms class=30001 sub=2 slowRings=65 "
            "slowPoints=6628 polyInput=10069 polyDense=10072 "
            "polyConstraints=10198/10104 polyPairs=12319/25163701 "
            "polyTris=9278 cdtPointTests=48878691 cdtBad=59870 "
            "cdtEdgeLookups=10104 cdtCrossTests=2895181 "
            "cdtConstraints=9829/275 cdtPeakTris=13037 "
            "cdtCapacityGrowths=0/0"
        )
        self.assertEqual(1, result["records"])
        counts = result["layers"]["amap-regions"]["cdtCounts"]
        self.assertEqual(13037, counts["cdtPeakTris"])
        self.assertEqual(0, counts["cdtPointGrowths"])
        self.assertEqual(0, counts["cdtTriGrowths"])

    def test_current_segmented_emulator_capture(self):
        result = module.summarize((
            ROOT / ".codex/artifacts/amap-i09-segmented-emulator-20260830.log"
        ).read_text())
        regions = result["layers"]["amap-regions"]
        self.assertGreaterEqual(regions["samples"], 8)
        self.assertGreater(regions["stageShare"]["polygon"], 0.99)
        self.assertEqual(30001, regions["slowestFeature"]["class"])
        self.assertEqual(2, regions["slowestFeature"]["sub"])
        self.assertEqual(65, regions["slowestFeature"]["rings"])
        self.assertEqual(6628, regions["slowestFeature"]["points"])
        self.assertAlmostEqual(
            455.23, regions["slowestFeature"]["slowest"], places=2)
        self.assertEqual(0.0, regions["polygonBreakdownMs"]["polyCdt"])

    def test_parses_new_polygon_breakdown(self):
        result = module.summarize(
            "VectorTessSlow: amap-regions z=10 x=1 y=2 features=1 "
            "convert=1.00ms tess=20.00ms admit=0.10ms polygon=19.00ms "
            "line=0.00ms polySetup=1.00ms polyDensify=2.00ms "
            "polyIntersect=12.00ms polyCdt=3.00ms polyEcef=1.00ms "
            "extrude=0.00ms symbol=0.00ms accepted=1 rejected=0 "
            "polyN=1 lineN=0 extrudeN=0 symbolN=0 rings=2 points=8 "
            "slowest=19.00ms class=30001 sub=2 slowRings=2 slowPoints=8 "
            "polyInput=8 polyDense=10 polyConstraints=8/8 polyTris=6"
        )
        breakdown = result["layers"]["amap-regions"]["polygonBreakdownMs"]
        self.assertEqual(12.0, breakdown["polyIntersect"])
        self.assertEqual(3.0, breakdown["polyCdt"])
        shares = result["layers"]["amap-regions"]["polygonBreakdownShare"]
        self.assertAlmostEqual(12.0 / 19.0, shares["polyIntersect"])
        self.assertAlmostEqual(3.0 / 19.0, shares["polyCdt"])

    def test_parses_cdt_breakdown_and_counts(self):
        result = module.summarize(
            "VectorTessSlow: amap-regions z=10 x=1 y=2 features=1 "
            "convert=1.00ms tess=20.00ms admit=0.10ms polygon=19.00ms "
            "line=0.00ms polySetup=1.00ms polyDensify=2.00ms "
            "polyIntersect=1.00ms polyCdt=14.00ms polyEcef=1.00ms "
            "cdtSuper=0.10ms cdtPoint=10.00ms cdtConstraint=3.00ms "
            "cdtExtract=0.90ms extrude=0.00ms symbol=0.00ms "
            "accepted=1 rejected=0 polyN=1 lineN=0 extrudeN=0 symbolN=0 "
            "rings=2 points=8 slowest=19.00ms class=30001 sub=2 "
            "slowRings=2 slowPoints=8 polyInput=8 polyDense=10 "
            "polyConstraints=8/8 polyPairs=3/28 polyTris=6 "
            "cdtPointTests=100 cdtBad=20 cdtEdgeLookups=40 "
            "cdtCrossTests=30 cdtConstraints=7/1 cdtPeakTris=12"
        )
        regions = result["layers"]["amap-regions"]
        self.assertEqual(10.0, regions["cdtBreakdownMs"]["cdtPoint"])
        self.assertAlmostEqual(
            10.0 / 14.0, regions["cdtBreakdownShare"]["cdtPoint"])
        self.assertEqual(100, regions["cdtCounts"]["cdtPointTests"])
        self.assertEqual(7, regions["cdtCounts"]["cdtAlready"])
        self.assertEqual(12, regions["cdtCounts"]["cdtPeakTris"])


if __name__ == "__main__":
    unittest.main()
