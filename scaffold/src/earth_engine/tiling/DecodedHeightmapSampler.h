#pragma once

namespace earth_engine {

class Rectangle;
struct DecodedHeightmap;

class DecodedHeightmapSampler {
public:
    static float sampleHeight(const DecodedHeightmap& heightmap,
                              const Rectangle& sourceBounds,
                              double longitudeRadians,
                              double latitudeRadians);
};

} // namespace earth_engine
