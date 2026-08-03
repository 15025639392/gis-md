#include "MvtVectorSource.h"

#include "MvtFeatureConverter.h"
#include "../core/async/AsyncSystem.h"

#include <algorithm>
#include <unordered_set>

namespace earth_engine {

MvtVectorSource::MvtVectorSource(Options options, FetchFn fetch,
                                 ThreadPool* decodePool)
    : options_(std::move(options)),
      fetch_(std::move(fetch)),
      decodePool_(decodePool),
      tree_(options_.tree),
      inbox_(std::make_shared<Inbox>()) {}

void MvtVectorSource::update(const Rectangle& viewRect,
                             double cameraHeightMeters) {
    ingestInbox();

    VectorTileTree::UpdateResult result =
        tree_.update(viewRect, cameraHeightMeters);

    // 发缺瓦片请求。回调持 shared_ptr 收件箱,本对象析构后迟到安全。
    for (const TileKey& key : result.requestTiles) {
        std::weak_ptr<Inbox> weakInbox = inbox_;
        ThreadPool* pool = decodePool_;
        fetch_(key, [key, weakInbox, pool](int statusCode,
                                           std::vector<uint8_t> body) {
            auto decodeAndDeliver = [key, weakInbox,
                                     body = std::move(body), statusCode]() {
                auto inbox = weakInbox.lock();
                if (!inbox) {
                    return;
                }
                MvtTile tile;
                bool ok = statusCode == 200 && !body.empty() &&
                          decodeMvtTile(body.data(), body.size(), tile);
                std::lock_guard<std::mutex> lock(inbox->mutex);
                if (ok) {
                    inbox->decoded.emplace_back(key, std::move(tile));
                } else {
                    inbox->failed.push_back(key);
                }
            };
            if (pool) {
                pool->enqueue(std::move(decodeAndDeliver));
            } else {
                decodeAndDeliver();
            }
        });
    }

    // 渲染集差分:进集激活,出集移除
    std::unordered_set<TileKey> renderSet(result.renderTiles.begin(),
                                          result.renderTiles.end());
    std::vector<TileKey> toDeactivate;
    for (const auto& [key, ids] : activeTiles_) {
        if (!renderSet.count(key)) {
            toDeactivate.push_back(key);
        }
    }
    for (const TileKey& key : toDeactivate) {
        deactivateTile(key);
    }
    for (const TileKey& key : result.renderTiles) {
        if (!activeTiles_.count(key)) {
            activateTile(key);
        }
    }
}

void MvtVectorSource::ingestInbox() {
    std::vector<std::pair<TileKey, MvtTile>> decoded;
    std::vector<TileKey> failed;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        decoded.swap(inbox_->decoded);
        failed.swap(inbox_->failed);
    }
    for (auto& [key, tile] : decoded) {
        tree_.provide(key, std::move(tile));
    }
    for (const TileKey& key : failed) {
        tree_.markFailed(key);
    }
}

void MvtVectorSource::activateTile(const TileKey& key) {
    const MvtTile* tile = tree_.loadedTile(key);
    if (tile == nullptr) {
        return;
    }
    std::vector<FeatureId>& ids = activeTiles_[key];
    for (const MvtLayer& layer : tile->layers) {
        if (!options_.includeLayers.empty() &&
            std::find(options_.includeLayers.begin(),
                      options_.includeLayers.end(),
                      layer.name) == options_.includeLayers.end()) {
            continue;
        }
        for (Feature& f : mvtLayerToFeatures(layer, key)) {
            ids.push_back(store_.addFeature(std::move(f)));
        }
    }
}

void MvtVectorSource::deactivateTile(const TileKey& key) {
    auto it = activeTiles_.find(key);
    if (it == activeTiles_.end()) {
        return;
    }
    for (FeatureId id : it->second) {
        store_.removeFeature(id);
    }
    activeTiles_.erase(it);
}

} // namespace earth_engine
