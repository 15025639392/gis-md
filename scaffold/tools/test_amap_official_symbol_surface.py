#!/usr/bin/env python3
import pathlib
import subprocess
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_ARCHIVE = ROOT / "build/native-tests/src/libearth_engine_core.a"
ARCHIVE = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ARCHIVE


class AmapOfficialSymbolSurfaceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.symbols = subprocess.check_output(
            ["nm", "-gU", str(ARCHIVE)], text=True
        )
        cls.symbols = subprocess.run(
            ["c++filt"], input=cls.symbols, text=True,
            check=True, capture_output=True
        ).stdout

    def test_old_redeclarable_codec_symbols_are_absent(self):
        for token in (
            "amap_official_internal::decodeType1Tile",
            "amap_official_internal::decodePoiTile",
            "amap_official_internal::decodedPartToFeatures",
        ):
            self.assertNotIn(token, self.symbols)

    def test_codec_symbols_belong_to_private_bundle_impl(self):
        for token in (
            "AmapClassicSourceBundle::Impl::decodeType1",
            "AmapClassicSourceBundle::Impl::decodePoi",
            "AmapClassicSourceBundle::Impl::convertPart",
        ):
            self.assertIn(token, self.symbols)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
