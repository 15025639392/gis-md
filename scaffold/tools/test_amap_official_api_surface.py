#!/usr/bin/env python3
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_HEADER = ROOT / "src/earth_engine/data/AmapVectorSource.h"
GEOMETRY_HEADER = ROOT / "src/earth_engine/data/AmapGeometry.h"
TILE_HEADER = ROOT / "src/earth_engine/data/AmapVectorTile.h"
INTERNAL_HEADER = ROOT / "src/earth_engine/data/AmapVectorSourceInternal.h"
ENGINE_SOURCE = ROOT / "src/earth_engine/Engine.cpp"
ENGINE_HEADER = ROOT / "src/earth_engine/Engine.h"
SCENE_SOURCE = ROOT / "src/earth_engine/scene/Scene.cpp"
FACADE_SOURCE = ROOT / "src/earth_engine/sdk/EarthEngineSdkFacade.cpp"
RUNTIME_HEADER = ROOT / "src/earth_engine/style/AmapClassicRuntime.h"
RUNTIME_SOURCE = ROOT / "src/earth_engine/style/AmapClassicRuntime.cpp"
TRANSPORT_SOURCE = ROOT / "src/earth_engine/style/AmapClassicTransport.cpp"
MANIFEST_HEADER = ROOT / "src/earth_engine/data/AmapTileManifest.h"
LABEL_PUBLIC_HEADER = ROOT / "src/earth_engine/style/AmapClassicLabelStyle.h"
LABEL_INTERNAL_HEADER = ROOT / "src/earth_engine/style/AmapClassicLabelStyleInternal.h"
METAL_DEVICE_SOURCE = ROOT / "src/earth_engine/platform/ios/RenderDeviceMetal.mm"
FEATURE_LAYER_SOURCE = ROOT / "src/earth_engine/layers/FeatureRenderLayer.cpp"


class AmapOfficialApiSurfaceTest(unittest.TestCase):
    def test_official_symbols_do_not_consume_generic_presentation_policy(self):
        feature = FEATURE_LAYER_SOURCE.read_text(encoding="utf-8")
        self.assertIn(
            "!amapOfficialContract &&\n"
            "         presentationPolicy_.symbolDepthPushCameraHeightMeters",
            feature)
        self.assertIn(
            "style_.usesOfficialProviderContract()\n"
            "            ? 0.0f\n"
            "            : presentationPolicy_.symbolOccludedMinOpacity",
            feature)

    def test_metal_vector_label_entry_points_and_layout_are_wired(self):
        metal = METAL_DEVICE_SOURCE.read_text(encoding="utf-8")
        for token in (
            'newFunctionWithName:@"vectorLabelVertex"',
            'newFunctionWithName:@"vectorLabelFragment"',
            'newFunctionWithName:@"vectorLabelBackgroundFragment"',
            "PipelineLayout::VectorLabel",
            "PipelineLayout::VectorLabelBackground",
            "vd.attributes[3].format = MTLVertexFormatFloat2",
            "vd.layouts[0].stride = 44",
            "setFragment(&u.sdfGamma, sizeof(u.sdfGamma), 4)",
        ):
            self.assertIn(token, metal)

        renderer = (ROOT / "src/earth_engine/renderer/Renderer.cpp").read_text(
            encoding="utf-8")
        terrain_fragment = renderer.index("static const char* kTerrainFragmentMSL")
        terrain_msl = renderer[terrain_fragment:
                               renderer.index(")msl\";", terrain_fragment)]
        self.assertIn("packed_float4 clipUV", terrain_msl)
        self.assertNotIn("u.clipUv", terrain_msl)

    def test_poi_resolver_contract_is_runtime_internal(self):
        self.assertFalse(LABEL_PUBLIC_HEADER.exists())
        internal = LABEL_INTERNAL_HEADER.read_text(encoding="utf-8")
        for token in (
            "resolveAmapClassicPoiIconStyle",
            "resolveAmapClassicPoiDynamicBackgroundStyle",
            "amapClassicPoiIconFrames",
            "amapClassicPoiIconAtlases",
            "amapClassicPoiCanCovered",
        ):
            self.assertIn(token, internal)

    def test_typed_official_consumers_are_not_public(self):
        text = SOURCE_HEADER.read_text(encoding="utf-8")
        forbidden = (
            "AmapType1TileCache",
            "AmapRegionsVectorSource",
            "AmapMainVectorSource",
            "AmapPoiVectorSource",
            "AmapRegionsToFeatures",
            "AmapMainToFeatures",
            "AmapPoiToFeatures",
            "AmapPoiDecodedTileDecodeTraits",
            "MvtTileFetchCacheT<",
            "VectorTileSourceT<",
        )
        for token in forbidden:
            self.assertNotIn(token, text, token)

        self.assertIn("class AmapClassicSourceBundle", text)
        self.assertIn("std::unique_ptr<Impl> impl_", text)
        self.assertIn("CacheStats type1CacheStats() const", text)
        self.assertIn("bool hasPendingWork() const", text)
        private = text.index("private:")
        public = text.index("public:", private)
        for token in ("using FetchCallback", "using Type1Fetch", "using PoiFetch"):
            position = text.index(token)
            self.assertGreater(position, private, token)
            self.assertLess(position, public, token)

    def test_official_decode_profile_is_test_only(self):
        def without_line_comments(path):
            return "\n".join(
                line.split("//", 1)[0]
                for line in path.read_text(encoding="utf-8").splitlines()
            )

        geometry = without_line_comments(GEOMETRY_HEADER)
        tile = without_line_comments(TILE_HEADER)
        for text, tokens in (
            (geometry, ("struct AmapDecodedTile", "struct AmapDecodedTileDecodeTraits",
                        "amapDecodedPartToFeatures")),
            (tile, ("decodeAmapTile", "decodeAmapPoiTile")),
        ):
            for token in tokens:
                position = text.index(token)
                testing = text.rfind("#if defined(EARTH_ENGINE_TESTING)", 0,
                                     position)
                closing = text.find("#endif", position)
                self.assertNotEqual(testing, -1, token)
                self.assertNotEqual(closing, -1, token)
                self.assertGreater(position, testing, token)
                self.assertLess(position, closing, token)

    def test_codec_types_are_nested_under_private_impl(self):
        source = SOURCE_HEADER.read_text(encoding="utf-8")
        internal = INTERNAL_HEADER.read_text(encoding="utf-8")
        private = source.index("private:")
        impl = source.index("struct Impl;", private)
        self.assertGreater(impl, private)
        self.assertIn("struct AmapClassicSourceBundle::Impl", internal)
        self.assertIn("struct Type1Traits", internal)
        self.assertIn("struct PoiTraits", internal)
        self.assertIn("using RegionsSource", internal)
        self.assertIn("using MainSource", internal)
        self.assertIn("using PoiSource", internal)

    def test_partial_style_installers_are_test_only(self):
        headers = (
            ROOT / "src/earth_engine/style/AmapClassicRoadStyle.h",
            ROOT / "src/earth_engine/style/AmapClassicLabelStyleInternal.h",
        )
        forbidden = (
            "applyAmapClassicSurfaceCommandStyle",
            "applyAmapClassicTransportStyle",
            "applyAmapClassicRoadLabelPlacementStyle",
            "applyAmapClassicLineLabelStyle",
            "applyAmapClassicAdministrativeLabelStyle",
            "applyAmapClassicPoiLabelStyle",
        )
        for path in headers:
            text = path.read_text(encoding="utf-8")
            for token in forbidden:
                self.assertNotIn(token, text, f"{path.name}: {token}")

        feature_header = (
            ROOT / "src/earth_engine/layers/FeatureRenderLayer.h"
        ).read_text(encoding="utf-8")
        feature_source = (
            ROOT / "src/earth_engine/layers/FeatureRenderLayer.cpp"
        ).read_text(encoding="utf-8")
        style_internal = (
            ROOT / "src/earth_engine/style/AmapClassicStyleInternal.h"
        ).read_text(encoding="utf-8")
        label_source = (
            ROOT / "src/earth_engine/style/AmapClassicLabelStyle.cpp"
        ).read_text(encoding="utf-8")
        for text in (feature_header, feature_source, style_internal, label_source):
            self.assertNotIn("AdministrativeLabel", text)
            self.assertNotIn("applyAdministrativeLabel", text)
        install = feature_header.index("void installAmapClassicProfile")
        testing = feature_header.rfind(
            "#if defined(EARTH_ENGINE_TESTING)", 0, install)
        closing = feature_header.find("#endif", install)
        self.assertNotEqual(testing, -1)
        self.assertGreater(install, testing)
        self.assertLess(install, closing)

    def test_android_official_scene_has_no_generic_vector_or_page_store_path(self):
        android = ROOT / "examples/android"
        gles_view = (android / "MinimalGlobe/GLESView.cpp").read_text(
            encoding="utf-8")
        sources = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (
                android / "CMakeLists.txt",
                android / "earthsdk/src/main/java/com/earthengine/sdk/GLESView.java",
                android / "app/src/main/java/com/earthengine/minimalglobe/MainActivity.java",
            )
        ) + "\n" + gles_view
        self.assertNotIn("MinimalGlobeDemoLayers", sources)
        self.assertNotIn("nativeAddDemoVectorLayer", sources)
        self.assertNotIn("debug.ee.amapvector", gles_view)
        self.assertNotIn("amapVectorEnabled", gles_view)
        self.assertNotIn("setLabelFontData", gles_view)
        self.assertNotIn("nativeSetLabelFontPath", sources)
        self.assertNotIn("nativeInstallAmapOfficialFont", sources)
        self.assertNotIn("installAmapClassicOfficialFont", gles_view)
        config = (android / "MinimalGlobe/MinimalGlobeDemoConfig.cpp").read_text(
            encoding="utf-8")
        config_header = (
            android / "MinimalGlobe/MinimalGlobeDemoConfig.h").read_text(
                encoding="utf-8")
        self.assertNotIn("kEnableAmapVectorDemo", config_header)
        self.assertNotIn("amapClassicVectorLand", config)
        for token in ("DemoSourceOverrides", "sources.json",
                      "kEnableTerrainForDemo", "kEnableRobotExpressiveGltfDemo",
                      "kEnableInstancedI3dmDemo"):
            self.assertNotIn(token, config + config_header + gles_view)
        self.assertIn("config.tileset.enableTerrainFillProxy = false", config)
        self.assertIn("config.aerialFog = false", config)
        self.assertNotIn("debug.ee.aerialfog", gles_view)
        terrain_config = (ROOT / "src/earth_engine/sdk/EarthSceneConfig.h").read_text(
            encoding="utf-8")
        self.assertNotIn("amapClassicVectorLand", terrain_config)
        engine_header = (ROOT / "src/earth_engine/Engine.h").read_text(
            encoding="utf-8")
        self.assertNotIn("removeAmapClassicRuntime", engine_header)
        self.assertLess(
            gles_view.index("installAmapClassicRuntime("),
            gles_view.index("installScene(std::move(sceneConfig))"))
        self.assertIn("config.terrainPageStore = false", config)

    def test_official_runtime_rejects_generic_raster_reentry(self):
        engine = ENGINE_SOURCE.read_text(encoding="utf-8")
        scene = SCENE_SOURCE.read_text(encoding="utf-8")
        facade = FACADE_SOURCE.read_text(encoding="utf-8")
        self.assertIn(
            "reject TerrainPageStore while AMap official runtime is active",
            engine)
        self.assertIn("reject public Tileset mutation", engine)
        self.assertIn("reject generic staged Tileset", engine)
        self.assertIn("reject generic content Tileset", engine)
        self.assertIn("tilesets_->hasAnyTileset()", scene)
        self.assertIn("terrainPageStore_ != nullptr", scene)
        self.assertIn(
            "reject non-official vector/raster/glTF/post-process/experimental scene beside AMap official runtime",
            facade)
        for forbidden in (
                "config.debugOffscreenPassthrough",
                "config.virtualTexturePoc",
                "config.tileCompositeBakePoc",
                "config.vtIndirectionSamplePoc",
                "config.terrainPageStore"):
            self.assertIn(forbidden, facade)
        self.assertIn(
            "reject generic FXAA while AMap official runtime is active",
            engine)
        self.assertIn(
            "reject generic aerial fog while AMap official runtime is active",
            engine)
        self.assertIn(
            "reject generic sunset terrain tint while AMap official runtime is active",
            engine)
        install_start = engine.index("installAmapClassicRuntime(")
        install_end = engine.index("hasAmapClassicRuntime() const", install_start)
        self.assertIn("scene_->setSunsetTerrainTint(0.0f, 0.0f)",
                      engine[install_start:install_end])
        assets_header = (
            ROOT / "src/earth_engine/style/AmapClassicAssets.h"
        ).read_text(encoding="utf-8")
        self.assertNotIn("std::string referer", assets_header)
        self.assertNotIn("credentials.referer", TRANSPORT_SOURCE.read_text(
            encoding="utf-8"))
        self.assertIn(
            "reject custom imagery overlay beside AMap official runtime",
            facade)
        self.assertNotIn("officialAmapCanvas", facade)
        self.assertNotIn("createAmapClassicTerrainRuntimeSources", facade)
        self.assertIn("decorateAmapClassicTerrainContentProvider", facade)
        mesh_header = (ROOT / "src/earth_engine/content/EllipsoidTerrainMeshBuilder.h").read_text(
            encoding="utf-8")
        provider_header = (ROOT / "src/earth_engine/content/EllipsoidTerrainContentProvider.h").read_text(
            encoding="utf-8")
        self.assertNotIn("AmapClassicVectorLand", mesh_header + provider_header)
        self.assertNotIn("amapClassicLandBaseColor", mesh_header + provider_header)
        self.assertNotIn("officialAmapCanvas", mesh_header + provider_header)
        self.assertNotIn("makeAmapClassicCanvasContentProvider",
                         mesh_header + provider_header)
        self.assertFalse(
            (ROOT / "src/earth_engine/style/AmapClassicSurfaceStyle.h").exists())
        terrain_internal = (ROOT / "src/earth_engine/content/AmapClassicTerrainInternal.h").read_text(
            encoding="utf-8")
        self.assertIn("decorateAmapClassicTerrainContentProvider",
                      terrain_internal)
        self.assertNotIn("makeAmapClassicCanvasContentProvider",
                         terrain_internal)
        official_visual_gate = facade.index("if (!officialAmapRuntime) {")
        for token in (
            "setSunsetTerrainTint",
            "setOffscreenPassthroughEnabled",
            "setFxaaEnabled",
            "setAerialFogEnabled",
            "setAerialFogParams",
        ):
            self.assertGreater(facade.index(token, official_visual_gate),
                               official_visual_gate)

    def test_official_runtime_exclusively_owns_amap_transport(self):
        engine = ENGINE_HEADER.read_text(encoding="utf-8")
        install = engine[engine.index("installAmapClassicRuntime("):
                         engine.index("hasAmapClassicRuntime(")]
        self.assertNotIn("Type1Fetch", install)
        self.assertNotIn("PoiFetch", install)

        runtime_header = RUNTIME_HEADER.read_text(encoding="utf-8")
        runtime_source = RUNTIME_SOURCE.read_text(encoding="utf-8")
        transport = TRANSPORT_SOURCE.read_text(encoding="utf-8")
        self.assertIn("class Transport", runtime_header)
        self.assertIn("std::unique_ptr<Transport> transport_", runtime_header)
        self.assertIn("transport_->fetchType1", runtime_source)
        self.assertIn("transport_->fetchPoi", runtime_source)
        self.assertIn("AmapClassicRuntime::Transport::Impl", transport)
        self.assertFalse(
            (ROOT / "src/earth_engine/style/AmapClassicTransport.h").exists())
        manifest = MANIFEST_HEADER.read_text(encoding="utf-8")
        for token in ("AmapManifestConfig", "AmapTileRequest", "AmapTileUrl",
                      "buildGetTileUrl", "buildGetTileBody", "parseTileUrls",
                      "selectAmapTileUrl"):
            self.assertNotIn(token, manifest)

        manifest = MANIFEST_HEADER.read_text(encoding="utf-8")
        for token in ("AmapHttpFetch", "resolveTileVersion",
                      "fetchAmapTileUrls"):
            self.assertNotIn(token, manifest)

        gles_view = (ROOT / "examples/android/MinimalGlobe/GLESView.cpp").read_text(
            encoding="utf-8")
        for token in ("AmapTileManifest", "gAmapFetch", "amapFetchTile",
                      "amapCleanupCompleted"):
            self.assertNotIn(token, gles_view)

        source = SOURCE_HEADER.read_text(encoding="utf-8")
        first_public = source.index("public:", source.index(
            "class AmapClassicSourceBundle"))
        for token in ("using FetchCallback", "using Type1Fetch",
                      "using PoiFetch"):
            self.assertLess(source.index(token), first_public, token)


if __name__ == "__main__":
    unittest.main()
