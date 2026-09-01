#!/usr/bin/env python3
import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "extract_amap_sdf_contract", ROOT / "tools/extract_amap_sdf_contract.py")
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class AmapSdfContractTest(unittest.TestCase):
    def test_fixed_official_response(self):
        response = (ROOT / "tests/fixtures/amap/sdf-chongqing.json").read_bytes()
        result = MODULE.decode_response(response)
        self.assertEqual(24, result["canonicalSizePx"])
        self.assertEqual(1, result["letterSpacingPxAtCanonicalSize"])
        self.assertEqual(24, result["metrics"]["37325"]["horiAdvance"])
        self.assertEqual(24, result["metrics"]["24198"]["horiAdvance"])
        fragment = result["fragmentStyle"]
        self.assertEqual(0.78125, fragment["smallFontEdge"])
        self.assertEqual(205 / 256, fragment["normalFontEdge"])
        self.assertIn("1.4142", fragment["gammaFormula"])
        self.assertIn("10.1", fragment["borderBufferFormula"])
        self.assertIn("1.5 / 256", fragment["bufferFormula"])
        self.assertEqual(
            "9c412dd8ac1a09326be6f103b7350afc6b2f5ed82fc2d14b8abfdceca45869be",
            result["pngSha256"],
        )

    def test_missing_or_malformed_provider_contract_fails_closed(self):
        with self.assertRaises(ValueError):
            MODULE.decode_response(b'{"code":1,"url":"","info":{}}')
        with self.assertRaises(ValueError):
            MODULE.decode_response(
                b'{"code":1,"url":"data:image/png;base64,iVBORw0KGgo=",'
                b'"info":{"37325":[22,21,1]}}'
            )


if __name__ == "__main__":
    unittest.main()
