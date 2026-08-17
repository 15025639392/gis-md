#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "../data/VectorRasterStyle.h"
#include "../layers/FeatureRenderLayer.h"

namespace earth_engine {

/// V26 二期:统一样式源模型(A 案自建精简子集,拍板 2026-08-18)。
///
/// 架构定位见 docs/issues/vector-style-architecture-2026-08-18.md:
/// 上半借形(声明式文档 + 既有求值内核 StyleExpression/StyleFilter),
/// 下半原创(StyleCompiler 的表示能力契约 + 成本类失效路由)。
/// **本文件只是源模型**:parse 产出它,compile 消费它;它不知道渲染路。
///
/// JSON 形态选**对象风格**而非 maplibre s-expression 数组:A 案的价值
/// 就是不受 spec 约束,对象字段与 builder API 一一对应、解析直白、错误
/// 可指字段名;日后若走 B(v8 兼容),换 parser 层即可,本模型与下游不动。
///
/// 层的渲染路由**编译器按 type 推**(fill→drape / line→场),文档不写路 ——
/// 表示随负载是引擎内部决策(mvt-vector-architecture.md §2),不暴露给
/// 样式作者。
struct StyleDocumentLayer {
    std::string id;           ///< 层唯一名(错误信息定位用)
    std::string type;         ///< "fill" | "line" | "symbol"(三期)
    std::string sourceLayer;  ///< MVT 源图层名(symbol 型见 compiler 注)
    int minZoom = 0;          ///< 生效 zoom 闭区间(语义随路:见 compiler)
    int maxZoom = 24;
    StyleFilter::Ptr filter;  ///< 逐要素过滤(空 = 全收;symbol 型不支持)
    /// paint 属性(按 type 消费,契约在 compiler):
    std::array<uint8_t, 4> fillColor{0, 0, 0, 0};   // fill
    std::array<float, 4> lineColor{0, 0, 0, 0};     // line(FS uniform)
    std::array<float, 4> lineWidthRamp{0, 0, 0, 0}; // line(z0,halfPx0,z1,halfPx1)
    /// symbol 型(三期):点符号/标注样式,字面量或表达式(match/interpolate
    /// 的 JSON 对象形态,见 .cpp parseStyleValueExpr)。直接携带
    /// FeatureRenderStyle 的对应子集 —— parser 填,compiler 原样搬。
    FeatureRenderStyle symbol;
    /// symbol 显式 set 掩码:**文档没写的字段不覆盖目标层现行样式**。
    /// 没有掩码就分不出"默认因为没写"vs"写了恰是默认值"——真机踩过:
    /// 文档只写 point-*,altitudeMode 被洗回 Absolute,符号整批埋进地形
    /// (demo 现行是 ClampToGround)。掩码粒度 = 语义属性(color 含
    /// 字面量+expr 双字段,置位时两者一起覆盖,literal 才能压掉旧 expr)。
    struct SymbolMask {
        bool pointColor = false;
        bool pointSize = false;
        bool pointImage = false;
        bool pointAnchor = false;
        bool labelProperty = false;
        bool labelColor = false;
        bool labelHaloColor = false;
        bool labelSize = false;
        bool labelOffset = false;
        bool labelHalo = false;
    };
    SymbolMask symbolMask;
    /// 该层 Re-bake/Re-tess 指纹:重建相关源字段的规范化序列化。
    /// 成本类路由靠它判"变没变"——表达式/filter 树无值等比较,比编译产物
    /// 不如比源(parser 顺手产出,零额外遍历)。symbol 型 = 整个 paint。
    std::string rebakeFingerprint;
};

struct StyleDocument {
    int version = 1;
    std::vector<StyleDocumentLayer> layers;
    /// line 路的场页 zoom 封顶(全文档级,语义见
    /// TerrainPageStore::Config::roadFieldMaxZoom)。缺省 -1 = 由 compiler
    /// 按「最后一个 zoom 分级档」推导。
    int fieldMaxZoom = -1;
};

/// parse/compile 的错误(fail-loud 契约:凡不认识/画不出,报错拒绝,
/// 不静默吞 —— 设计文档 §4.1)。
struct StyleError {
    std::string where;    ///< "layers[2].paint.line-dasharray" 形态的路径
    std::string message;  ///< 人话:错在哪、为什么、该用什么
};

/// JSON 文本 → StyleDocument。全部错误一次收齐(不首错即停,样式作者
/// 一轮改完);errors 非空时返回的 doc 不可用。
StyleDocument parseStyleDocument(const std::string& jsonText,
                                 std::vector<StyleError>& errors);

/// 编译产物:面/线/点三路的运行时参数(三期全;交互路判缓见设计文档)。
struct CompiledStyle {
    VectorRasterStyle drapeStyle;                    // 面 → drape 生产者
    VectorRasterStyle fieldStyle;                    // 线分级 → 场生产者
    std::array<float, 4> fieldLineColor{0, 0, 0, 0}; // Uniform 类
    std::array<float, 4> fieldWidthRamp{0, 0, 0, 0}; // Uniform 类
    int fieldMaxZoom = 0;                            // Re-bake 类
    /// 点/符号(三期):FeatureRenderLayer::setStyle 消费(其内部已全桶
    /// 重镶 = Re-tess 失效通路现成)。文档无 symbol 层时 hasSymbol=false,
    /// 调用方不得应用空样式(会把符号层洗成默认样式)。
    /// **应用语义 = 以目标层现行样式为底、按 symbolMask 覆盖**(见
    /// mergeSymbolStyle)——文档没写的字段(含 altitudeMode 这类不在契约面
    /// 的几何语义字段)保持现状。
    FeatureRenderStyle symbolStyle;
    StyleDocumentLayer::SymbolMask symbolMask;
    bool hasSymbol = false;
    /// 成本类路由指纹(各路 Re-bake/Re-tess 相关源内容拼接)。
    std::string drapeFingerprint;
    std::string fieldFingerprint;
    std::string symbolFingerprint;
};

/// 成本类失效指令:新旧 CompiledStyle 对比得出,调用方照单执行。
/// Uniform 类恒直写(便宜到不值得 diff);Re-bake/Re-tess 按指纹判。
struct StyleApplyPlan {
    bool rebakeDrape = false;  ///< true → invalidateComposedTerrainPages
    bool rebakeField = false;  ///< true → RoadFieldSource::setStyle + invalidateRoadFieldPages
    bool retessSymbols = false;  ///< true → FeatureRenderLayer::setStyle(内部全桶重镶)
};

/// StyleDocument → CompiledStyle。表示能力契约校验在此(未知 type、
/// line 层带 fill 属性之类 → errors);errors 非空时产物不可用。
CompiledStyle compileStyleDocument(const StyleDocument& doc,
                                   std::vector<StyleError>& errors);

/// 成本类路由:old 为空(首次应用)→ 全 Re-bake。
StyleApplyPlan planStyleApply(const CompiledStyle* oldStyle,
                              const CompiledStyle& newStyle);

/// 按掩码把文档 symbol 样式合到现行样式上:置位字段覆盖(color 类同时覆盖
/// 字面量与 expr,literal 才能压掉旧 expr),未置位字段保持现状 —— 含
/// altitudeMode/heightOffset 这类不在文档契约面的几何语义字段。
FeatureRenderStyle mergeSymbolStyle(
    const FeatureRenderStyle& current, const FeatureRenderStyle& fromDoc,
    const StyleDocumentLayer::SymbolMask& mask);

} // namespace earth_engine
