#include <gtest/gtest.h>

#include "earth_engine/core/net/Uri.h"

using namespace earth_engine;

// ==================================================================
// parse / compose
// ==================================================================

TEST(UriTest, ParseSimpleHttp) {
    auto p = Uri::parse("http://example.com/path/to/file");
    EXPECT_TRUE(p.hasScheme);
    EXPECT_EQ("http", p.scheme);
    EXPECT_EQ("example.com", p.authority);
    EXPECT_EQ("/path/to/file", p.path);
    EXPECT_TRUE(p.query.empty());
    EXPECT_TRUE(p.fragment.empty());
}

TEST(UriTest, ParseWithQuery) {
    auto p = Uri::parse("https://host/path?key=val&a=b");
    EXPECT_EQ("https", p.scheme);
    EXPECT_EQ("host", p.authority);
    EXPECT_EQ("/path", p.path);
    EXPECT_EQ("key=val&a=b", p.query);
}

TEST(UriTest, ParseWithFragment) {
    auto p = Uri::parse("http://x.com/p#frag");
    EXPECT_EQ("http", p.scheme);
    EXPECT_EQ("/p", p.path);
    EXPECT_EQ("frag", p.fragment);
}

TEST(UriTest, ComposeRoundtrip) {
    std::string url = "https://example.com:8080/path?a=1#sec";
    auto p = Uri::parse(url);
    EXPECT_EQ(url, Uri::compose(p));
}

TEST(UriTest, RelativePathNoScheme) {
    auto p = Uri::parse("/relative/path");
    EXPECT_FALSE(p.hasScheme);
    EXPECT_EQ("/relative/path", p.path);
}

// ==================================================================
// resolve
// ==================================================================

TEST(UriTest, ResolveRelativePath) {
    std::string result = Uri::resolve("http://host/a/b/c", "d/e");
    EXPECT_EQ("http://host/a/b/d/e", result);
}

TEST(UriTest, ResolveAbsolutePath) {
    std::string result = Uri::resolve("http://host/a/b/c", "/d/e");
    EXPECT_EQ("http://host/d/e", result);
}

TEST(UriTest, ResolveDotDot) {
    std::string result = Uri::resolve("http://host/a/b/c", "../d");
    EXPECT_EQ("http://host/a/d", result);
}

TEST(UriTest, ResolveAbsoluteUrlReturnsAsIs) {
    std::string result = Uri::resolve("http://host/a", "http://other/x");
    EXPECT_EQ("http://other/x", result);
}

TEST(UriTest, ResolveProtocolRelative) {
    std::string result = Uri::resolve("https://host/a", "//other/b");
    EXPECT_EQ("https://other/b", result);
}

TEST(UriTest, ResolveEmptyRelativeReturnsBase) {
    std::string result = Uri::resolve("http://host/path?a=1", "");
    EXPECT_EQ("http://host/path?a=1", result);
}

TEST(UriTest, ResolveQueryOverride) {
    std::string result = Uri::resolve("http://host/p?x=1", "?y=2");
    EXPECT_EQ("http://host/p?y=2", result);
}

TEST(UriTest, ResolveQueryMerge) {
    std::string result = Uri::resolve("http://host/p?x=1&z=3", "?y=2", true);
    EXPECT_EQ("http://host/p?y=2&x=1&z=3", result);
}

TEST(UriTest, ResolveFragment) {
    std::string result = Uri::resolve("http://host/p", "#sec");
    EXPECT_EQ("http://host/p#sec", result);
}

// ==================================================================
// substituteTemplateParameters
// ==================================================================

TEST(UriTest, SubstituteSimple) {
    std::string result = Uri::substituteTemplateParameters(
        "/tiles/{level}/{x}/{y}.b3dm",
        [](const std::string& p) -> std::string {
            if (p == "level") return "2";
            if (p == "x") return "3";
            if (p == "y") return "4";
            return p;
        });
    EXPECT_EQ("/tiles/2/3/4.b3dm", result);
}

TEST(UriTest, SubstituteUnclosedBrace) {
    std::string result = Uri::substituteTemplateParameters(
        "prefix{unclosed",
        [](const std::string&) { return "X"; });
    EXPECT_EQ("prefix{unclosed", result);
}

TEST(UriTest, SubstituteNoPlaceholders) {
    std::string result = Uri::substituteTemplateParameters(
        "plain/string",
        [](const std::string&) { return ""; });
    EXPECT_EQ("plain/string", result);
}

TEST(UriTest, SubstituteEmptyPlaceholder) {
    std::string result = Uri::substituteTemplateParameters(
        "prefix{}suffix",
        [](const std::string& p) {
            EXPECT_TRUE(p.empty());
            return "X";
        });
    EXPECT_EQ("prefixXsuffix", result);
}

// ==================================================================
// addQuery
// ==================================================================

TEST(UriTest, AddQueryFirst) {
    std::string result = Uri::addQuery("http://host/path", "key", "val");
    EXPECT_EQ("http://host/path?key=val", result);
}

TEST(UriTest, AddQuerySecond) {
    std::string result = Uri::addQuery("http://host/path?a=1", "key", "val");
    EXPECT_EQ("http://host/path?a=1&key=val", result);
}

// ==================================================================
// percentEncode / percentDecode
// ==================================================================

TEST(UriTest, PercentEncodeUnreservedStays) {
    EXPECT_EQ("abc123-._~", Uri::percentEncode("abc123-._~"));
}

TEST(UriTest, PercentEncodeSpace) {
    EXPECT_EQ("%20", Uri::percentEncode(" "));
}

TEST(UriTest, PercentEncodeSpecial) {
    EXPECT_EQ("%23%24%25", Uri::percentEncode("#$%"));
}

TEST(UriTest, PercentDecode) {
    EXPECT_EQ("a b", Uri::percentDecode("a%20b"));
}

TEST(UriTest, PercentDecodeRoundtrip) {
    std::string original = "hello world!@#$%^&*()";
    std::string encoded = Uri::percentEncode(original);
    std::string decoded = Uri::percentDecode(encoded);
    EXPECT_EQ(original, decoded);
}

// ==================================================================
// normalizePath
// ==================================================================

TEST(UriTest, NormalizeSimple) {
    EXPECT_EQ("/a/b", Uri::normalizePath("/a/b"));
}

TEST(UriTest, NormalizeDotDot) {
    EXPECT_EQ("/a", Uri::normalizePath("/a/b/../c/.."));
}

TEST(UriTest, NormalizeDot) {
    EXPECT_EQ("/a/c", Uri::normalizePath("/a/./b/../c"));
}

TEST(UriTest, NormalizeRelativeWithDotDot) {
    EXPECT_EQ("../b", Uri::normalizePath("a/../../b"));
}

TEST(UriTest, NormalizeTrailingSlash) {
    EXPECT_EQ("/a/b/", Uri::normalizePath("/a/b///"));
}

TEST(UriTest, NormalizeEmpty) {
    EXPECT_TRUE(Uri::normalizePath("").empty());
}

TEST(UriTest, NormalizeSlashOnly) {
    EXPECT_EQ("/", Uri::normalizePath("/"));
}

// ==================================================================
// hasScheme
// ==================================================================

TEST(UriTest, HasSchemeHttp) {
    EXPECT_TRUE(Uri::hasScheme("http://host"));
}

TEST(UriTest, HasSchemeHttps) {
    EXPECT_TRUE(Uri::hasScheme("https://host"));
}

TEST(UriTest, HasSchemeNo) {
    EXPECT_FALSE(Uri::hasScheme("relative/path"));
}

TEST(UriTest, HasSchemeJustColon) {
    EXPECT_FALSE(Uri::hasScheme(":path"));
}

// ==================================================================
// TileMapServiceUrl compatible behavior
// ==================================================================

TEST(UriTest, ResolveRelativeWithFragment) {
    // TileMapServiceUrl: base ?# dropped, relative ?# preserved
    std::string result = Uri::resolve("http://host/a/b?baseq=1", "c/d#frag");
    EXPECT_EQ("http://host/a/c/d#frag", result);
}

TEST(UriTest, ResolveRelativeDotDot) {
    // TileMapServiceUrl: ../ handling
    std::string result = Uri::resolve("http://host/a/b/c", "../d/e");
    EXPECT_EQ("http://host/a/d/e", result);
}

// ==================================================================
// ImplicitTileIdUtilities compatible behavior
// ==================================================================

TEST(UriTest, SubstituteLevelXY) {
    // ImplicitTileIdUtilities: {level}/{x}/{y}
    std::string result = Uri::substituteTemplateParameters(
        "{level}/{x}/{y}.terrain",
        [](const std::string& p) -> std::string {
            if (p == "level") return "5";
            if (p == "x") return "12";
            if (p == "y") return "7";
            return p;
        });
    EXPECT_EQ("5/12/7.terrain", result);
}

// ==================================================================
// GltfContentProvider compatible behavior
// ==================================================================

TEST(UriTest, ResolveUriWithBaseQueryMerge) {
    // GltfContentProvider: useBaseQuery=true merges base query keys
    std::string result = Uri::resolve("http://host/tileset.json?session=abc&token=xyz",
                                      "model.b3dm", true);
    EXPECT_EQ("http://host/model.b3dm?session=abc&token=xyz", result);
}
