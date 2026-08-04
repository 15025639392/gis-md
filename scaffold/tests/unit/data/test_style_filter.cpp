#include <gtest/gtest.h>

#include "earth_engine/data/StyleFilter.h"

#include <cmath>

using namespace earth_engine;

namespace {

StyleFilter::PropertyMap props(
    std::initializer_list<std::pair<const std::string, std::string>> init) {
    return StyleFilter::PropertyMap(init);
}

} // namespace

TEST(StyleFilterTest, EqualityComparesRawText) {
    const auto f = StyleFilter::compare("highway", StyleFilter::Compare::Equal,
                                        std::string("motorway"));
    const auto p = props({{"highway", "motorway"}});
    EXPECT_TRUE(f->matches(&p, 10.0));
    const auto q = props({{"highway", "primary"}});
    EXPECT_FALSE(f->matches(&q, 10.0));
}

// 数值比较要求两侧都能数值化,且**整串**被消费 —— 否则 "12abc" < 20 会
// 莫名成立,而 OSM 属性里这类脏值很常见(如 "30 mph")。
TEST(StyleFilterTest, NumericComparisonRejectsPartialParse) {
    const auto f =
        StyleFilter::compare("lanes", StyleFilter::Compare::Less, 4.0);
    const auto good = props({{"lanes", "2"}});
    EXPECT_TRUE(f->matches(&good, 10.0));
    const auto dirty = props({{"lanes", "2 or 3"}});
    EXPECT_FALSE(f->matches(&dirty, 10.0));
}

// double 重载不能因 to_string 的尾随零而与属性原文比不上。
TEST(StyleFilterTest, DoubleOverloadEqualsPlainText) {
    const auto f =
        StyleFilter::compare("level", StyleFilter::Compare::Equal, 12.0);
    const auto p = props({{"level", "12"}});
    EXPECT_TRUE(f->matches(&p, 10.0));
}

// 属性缺失 → false,**NotEqual 也是 false**。若「没有 highway」算作
// 「highway != motorway」成立,排除式过滤会把无关要素全放进来。
TEST(StyleFilterTest, MissingPropertyIsFalseEvenForNotEqual) {
    const auto eq = StyleFilter::compare(
        "highway", StyleFilter::Compare::Equal, std::string("motorway"));
    const auto ne = StyleFilter::compare(
        "highway", StyleFilter::Compare::NotEqual, std::string("motorway"));
    const auto p = props({{"name", "某路"}});
    EXPECT_FALSE(eq->matches(&p, 10.0));
    EXPECT_FALSE(ne->matches(&p, 10.0));
    EXPECT_FALSE(eq->matches(nullptr, 10.0));
    EXPECT_FALSE(ne->matches(nullptr, 10.0));
}

TEST(StyleFilterTest, InMatchesAnyOfSet) {
    const auto f = StyleFilter::in("highway", {"motorway", "trunk", "primary"});
    const auto hit = props({{"highway", "trunk"}});
    const auto miss = props({{"highway", "residential"}});
    EXPECT_TRUE(f->matches(&hit, 10.0));
    EXPECT_FALSE(f->matches(&miss, 10.0));
    EXPECT_FALSE(StyleFilter::in("highway", {})->matches(&hit, 10.0)) << "空集恒 false";
}

TEST(StyleFilterTest, HasAndNotHas) {
    const auto p = props({{"name", "某路"}});
    EXPECT_TRUE(StyleFilter::has("name")->matches(&p, 10.0));
    EXPECT_FALSE(StyleFilter::has("highway")->matches(&p, 10.0));
    EXPECT_TRUE(StyleFilter::notHas("highway")->matches(&p, 10.0));
    EXPECT_FALSE(StyleFilter::notHas("name")->matches(&p, 10.0));
}

// 空集约定:all = true(布尔代数惯例,也让「无过滤」可写成 all({}));
// any = false。写死是因为反过来会让组合过滤在边界情形静默失效。
TEST(StyleFilterTest, EmptyCompositesFollowBooleanAlgebra) {
    const auto p = props({{"highway", "trunk"}});
    EXPECT_TRUE(StyleFilter::all({})->matches(&p, 10.0));
    EXPECT_FALSE(StyleFilter::any({})->matches(&p, 10.0));
}

TEST(StyleFilterTest, CompositesCombine) {
    const auto f = StyleFilter::all(
        {StyleFilter::in("highway", {"motorway", "trunk"}),
         StyleFilter::negate(StyleFilter::has("tunnel"))});
    const auto surface = props({{"highway", "trunk"}});
    const auto tunnel = props({{"highway", "trunk"}, {"tunnel", "yes"}});
    const auto minor = props({{"highway", "residential"}});
    EXPECT_TRUE(f->matches(&surface, 10.0));
    EXPECT_FALSE(f->matches(&tunnel, 10.0));
    EXPECT_FALSE(f->matches(&minor, 10.0));
}

// 空子节点不得让整棵树崩(规则可能来自外部构造)。
TEST(StyleFilterTest, NullChildrenAreSafe) {
    const auto p = props({{"highway", "trunk"}});
    EXPECT_FALSE(StyleFilter::all({nullptr})->matches(&p, 10.0));
    EXPECT_FALSE(StyleFilter::any({nullptr})->matches(&p, 10.0));
    EXPECT_TRUE(StyleFilter::negate(nullptr)->matches(&p, 10.0));
}

// E2 的核心能力:同一条规则在不同瓦片 zoom 下给出不同结果 —— 这才是
// tippecanoe -j 分级过滤的运行时等价物。瓦片 z 固定,故相机缩放不重镶。
TEST(StyleFilterTest, ZoomGradedRoadClasses) {
    using C = StyleFilter::Compare;
    const auto f = StyleFilter::any({
        StyleFilter::all({StyleFilter::zoomCompare(C::Less, 9),
                          StyleFilter::in("highway", {"motorway", "trunk"})}),
        StyleFilter::all({StyleFilter::zoomCompare(C::GreaterEqual, 9),
                          StyleFilter::zoomCompare(C::Less, 12),
                          StyleFilter::in("highway", {"motorway", "trunk",
                                                     "secondary"})}),
        StyleFilter::zoomCompare(C::GreaterEqual, 12),
    });
    const auto trunk = props({{"highway", "trunk"}});
    const auto secondary = props({{"highway", "secondary"}});
    const auto residential = props({{"highway", "residential"}});

    EXPECT_TRUE(f->matches(&trunk, 5));
    EXPECT_FALSE(f->matches(&secondary, 5))   << "粗档只留干线";
    EXPECT_TRUE(f->matches(&secondary, 10))   << "中档放开次干道";
    EXPECT_FALSE(f->matches(&residential, 10));
    EXPECT_TRUE(f->matches(&residential, 13)) << "细档全量";
}

// 无 zoom 上下文时 zoomCompare 取 false,而不是随机通过。
TEST(StyleFilterTest, ZoomCompareWithoutContextIsFalse) {
    const auto f = StyleFilter::zoomCompare(StyleFilter::Compare::Less, 9);
    const auto p = props({{"highway", "trunk"}});
    EXPECT_FALSE(f->matches(&p, std::nan("")));
}
