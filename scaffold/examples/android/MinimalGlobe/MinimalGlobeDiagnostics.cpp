#include "MinimalGlobeDiagnostics.h"

#include "earth_engine/renderer/RenderCommand.h"
#include "earth_engine/scene/Diagnostics.h"
#include "earth_engine/scene/PresentationTrace.h"
#include "earth_engine/scene/Scene.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TilePlan.h"

#include <algorithm>
#include <sstream>

namespace earth_engine::minimal_globe_demo {
namespace {

std::string tileKeyLabel(const TileKey& key) {
    std::ostringstream out;
    out << key.schemeId << "/" << key.z << "/" << key.x << "/" << key.y;
    return out.str();
}

const char* renderCommandKindLabel(RenderCommandKind kind) {
    switch (kind) {
        case RenderCommandKind::SkyBackground: return "sky";
        case RenderCommandKind::AtmosphereBackground: return "atmo";
        case RenderCommandKind::GlobeSurface: return "globe";
        case RenderCommandKind::SurfaceTile: return "surface";
        case RenderCommandKind::GltfPrimitive: return "gltf";
        case RenderCommandKind::GltfPrimitiveInstanced: return "gltf-i";
        case RenderCommandKind::VectorOverlay: return "vector";
        case RenderCommandKind::Unknown:
        default: return "unknown";
    }
}

} // namespace

std::string buildRenderEntryDiagnosticsLine(const Diagnostics& diagnostics) {
    std::ostringstream out;
    out << "Render entries: plan " << diagnostics.terrainRenderEntriesPlanned
        << " (sel " << diagnostics.terrainRenderEntriesSelectedPlanned
        << ", fade " << diagnostics.terrainRenderEntriesFadingPlanned
        << ")  draw " << diagnostics.terrainRenderEntriesDrawn
        << " (sel " << diagnostics.terrainRenderEntriesSelectedDrawn
        << ", fade " << diagnostics.terrainRenderEntriesFadingDrawn
        << ")  miss " << diagnostics.terrainRenderEntriesMissed
        << " (sel " << diagnostics.terrainRenderEntriesSelectedMissed
        << ", fade " << diagnostics.terrainRenderEntriesFadingMissed
        << ")  defer " << diagnostics.terrainRenderEntriesDeferred
        << " (sel " << diagnostics.terrainRenderEntriesSelectedDeferred
        << ", fade " << diagnostics.terrainRenderEntriesFadingDeferred
        << ")  fallback " << diagnostics.terrainRenderEntriesAncestorFallback
        << "  prep " << diagnostics.terrainRenderEntriesSynchronousPrep
        << "/" << diagnostics.terrainRenderEntriesDeferredPrep
        << "  surface/globe/masked "
        << diagnostics.terrainSurfaceCommandsSubmitted
        << "/" << diagnostics.globeFallbackCommands
        << "/" << diagnostics.globeFallbackMaskedTerrainEntries
        << "\n";
    return out.str();
}

std::string buildPresentationTraceSummary(const PresentationTrace& trace) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);

    out << "\n\nPresentation trace\n";
    out << "Camera: center="
        << trace.camera.targetLongitudeDegrees << ","
        << trace.camera.targetLatitudeDegrees
        << " h=" << static_cast<int>(trace.camera.targetHeightMeters)
        << " camH=" << static_cast<int>(trace.camera.cameraHeightMeters)
        << " pitch=" << trace.camera.pitchRadians
        << " heading=" << trace.camera.headingRadians
        << " vp=" << trace.camera.viewportWidthPixels
        << "x" << trace.camera.viewportHeightPixels << "\n";
    out << "Selector views: " << trace.selectorViews.size();
    if (!trace.selectorViews.empty()) {
        out << " firstVpH=" << trace.selectorViews.front().viewportHeightPixels;
    }
    out << "\n";

    size_t surfaceCount = 0;
    out << "Commands:";
    for (const PresentationCommandTrace& command : trace.commands) {
        if (command.kind != RenderCommandKind::SurfaceTile &&
            command.kind != RenderCommandKind::GltfPrimitive &&
            command.kind != RenderCommandKind::GltfPrimitiveInstanced) {
            continue;
        }
        if (surfaceCount >= 4) {
            out << " ...";
            break;
        }
        out << " " << renderCommandKindLabel(command.kind)
            << "(gz=" << command.surfaceGeometryZoom
            << ",tz=" << command.surfaceTextureZoom
            << ",idx=" << command.indexCount
            << "/" << command.surfaceMeshIndexCount
            << ",base=real"
            << ",rs=" << command.surfaceBaseRasterState;
        if (command.surfaceBaseIsMappedRasterTile) {
            out << ",mapped";
        }
        if (command.surfaceSkirtIndexCount > 0) {
            out << ",skirt-" << command.surfaceSkirtIndexCount;
        }
        if (command.surfaceClipEnabled > 0.5f) {
            out << ",clip";
        }
        if (!command.stableKey.empty()) {
            out << ",key=" << command.stableKey;
        }
        out << ")";
        ++surfaceCount;
    }
    if (surfaceCount == 0) {
        out << " none";
    }
    out << "\n";

    const PresentationTilesetTrace* terrainTrace =
        trace.tilesets.empty() ? nullptr : &trace.tilesets.front();
    if (terrainTrace) {
        out << "Tileset: visible=" << terrainTrace->visibleTiles.size()
            << " renderEntries=" << terrainTrace->renderEntries.size()
            << " commands=" << terrainTrace->renderEntryCommandDrawCount
            << "/" << terrainTrace->renderEntryPlannedCommandCount
            << " missed=" << terrainTrace->renderEntryCommandMissedDrawCount
            << " deferred=" << terrainTrace->renderEntryCommandDeferredCount
            << " fallback=" << terrainTrace->renderEntryAncestorFallbackCount
            << " prepSync=" << terrainTrace->renderEntrySynchronousPrepCount
            << " prepDeferred=" << terrainTrace->renderEntryDeferredPrepCount
            << " zoom=" << terrainTrace->minVisibleZoom
            << "-" << terrainTrace->maxVisibleZoom
            << " lod=" << static_cast<int>(terrainTrace->lodSizePixels)
            << " load=" << static_cast<int>(
                   terrainTrace->frameLoadProgressPercentage)
            << "% work=" << terrainTrace->frameProgressLoadingCount
            << "/" << terrainTrace->frameProgressTotalCount
            << " mapped=" << terrainTrace->frameMappedRasterTileLoadingCount
            << "/" << terrainTrace->frameMappedRasterTileCount
            << "\n";
        if (!terrainTrace->frameCredits.empty()) {
            const size_t creditCount =
                std::min<size_t>(terrainTrace->frameCredits.size(), 3);
            out << "Credits:";
            for (size_t i = 0; i < creditCount; ++i) {
                out << " " << terrainTrace->frameCredits[i];
                if (i + 1 < creditCount) {
                    out << ";";
                }
            }
            if (terrainTrace->frameCredits.size() > creditCount) {
                out << "; ...";
            }
            out << "\n";
        }
        const size_t visibleCount =
            std::min<size_t>(terrainTrace->visibleTiles.size(), 4);
        out << "Visible:";
        for (size_t i = 0; i < visibleCount; ++i) {
            out << " " << tileKeyLabel(terrainTrace->visibleTiles[i]);
        }
        if (terrainTrace->visibleTiles.size() > visibleCount) {
            out << " ...";
        }
        out << "\n";

        const size_t entryCount =
            std::min<size_t>(terrainTrace->renderEntries.size(), 4);
        out << "Render:";
        for (size_t i = 0; i < entryCount; ++i) {
            const auto& entry = terrainTrace->renderEntries[i];
            out << " " << tileKeyLabel(entry.selectedKey)
                << "->" << tileKeyLabel(entry.renderKey)
                << "[" << tileRenderEntryReasonLabel(entry.reason) << "]";
            if (entry.usesAncestorFallback) {
                out << "[fallback]";
            }
            if (entry.surfaceClipEnabled) {
                out << "[clip]";
            }
        }
        if (terrainTrace->renderEntries.size() > entryCount) {
            out << " ...";
        }
        out << "\n";
    }

    return out.str();
}

} // namespace earth_engine::minimal_globe_demo
