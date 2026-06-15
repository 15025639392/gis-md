#pragma once

#include <string>
#include <memory>

namespace earth_engine {

class ImageryProvider;
class TileScheme;

/// cesium-native RasterOverlay equivalent.
///
/// Pure configuration layer — holds the imagery data source, tile scheme,
/// and loading options. Runtime state lives in ActivatedRasterOverlay and
/// RasterOverlayTileProvider.
///
/// Referenced by RasterOverlayTileProvider via getOwner().
class RasterOverlay {
public:
    struct Options {
        /// Maximum simultaneous tile loads (aligned with RasterOverlayOptions).
        int maximumSimultaneousTileLoads;

        /// Maximum screen space error for LOD selection.
        double maximumScreenSpaceError;

        /// Minimum/maximum zoom overrides (0 = use provider defaults).
        int minimumZoom;
        int maximumZoom;

        /// Runtime presentation flags used by the unified Tileset renderer.
        bool visible = true;
        float opacity = 1.0f;
    };

    /// @param provider  The imagery data source (ownership transferred).
    /// @param scheme    The tile coordinate scheme (ownership transferred).
    /// @param options   Loading options.
    RasterOverlay(std::unique_ptr<ImageryProvider> provider,
                  std::unique_ptr<TileScheme> scheme,
                  Options options);

    ~RasterOverlay();

    RasterOverlay(const RasterOverlay&) = delete;
    RasterOverlay& operator=(const RasterOverlay&) = delete;

    /// The imagery data source.
    ImageryProvider& getProvider() { return *provider_; }
    const ImageryProvider& getProvider() const { return *provider_; }

    /// The tile coordinate scheme.
    TileScheme& getTileScheme() { return *scheme_; }
    const TileScheme& getTileScheme() const { return *scheme_; }

    /// Loading options.
    const Options& getOptions() const { return options_; }
    Options& getOptions() { return options_; }

    /// Unique overlay identifier (from provider id).
    const std::string& getID() const { return id_; }

    bool visible() const { return options_.visible; }
    void setVisible(bool visible) { options_.visible = visible; }

    float opacity() const { return options_.opacity; }
    void setOpacity(float opacity);

private:
    std::unique_ptr<ImageryProvider> provider_;
    std::unique_ptr<TileScheme> scheme_;
    Options options_;
    std::string id_;
};

} // namespace earth_engine
