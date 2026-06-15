#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace earth_engine {

inline constexpr std::array<std::string_view, 13> kSupportedGltfExtensions = {
    "KHR_texture_transform",
    "KHR_mesh_quantization",
    "KHR_materials_unlit",
    "KHR_materials_emissive_strength",
    "KHR_materials_ior",
    "KHR_materials_pbrSpecularGlossiness",
    "KHR_materials_transmission",
    "KHR_materials_anisotropy",
    "KHR_materials_specular",
    "KHR_materials_clearcoat",
    "KHR_materials_sheen",
    "EXT_texture_webp",
    "EXT_mesh_gpu_instancing"};

inline constexpr std::array<std::string_view, 12>
    kSupportedGltfObjectExtensions = {
        "KHR_texture_transform",
        "KHR_materials_unlit",
        "KHR_materials_emissive_strength",
        "KHR_materials_ior",
        "KHR_materials_pbrSpecularGlossiness",
        "KHR_materials_transmission",
        "KHR_materials_anisotropy",
        "KHR_materials_specular",
        "KHR_materials_clearcoat",
        "KHR_materials_sheen",
        "EXT_texture_webp",
        "EXT_mesh_gpu_instancing"};

inline bool isSupportedGltfExtension(std::string_view name) {
    return std::find(
               kSupportedGltfExtensions.begin(),
               kSupportedGltfExtensions.end(),
               name) != kSupportedGltfExtensions.end();
}

} // namespace earth_engine
