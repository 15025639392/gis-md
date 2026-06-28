#include <gtest/gtest.h>

#include "earth_engine/core/gltf/AccessorView.h"

#include <cstdint>
#include <vector>

using namespace earth_engine;

TEST(AccessorViewTest, DefaultConstructedIsInvalid) {
    AccessorView<float> view;
    EXPECT_EQ(AccessorViewStatus::InvalidAccessorIndex, view.status());
    EXPECT_EQ(0, view.size());
    EXPECT_FALSE(static_cast<bool>(view));
}

TEST(AccessorViewTest, ConstructWithStatus) {
    AccessorView<float> view(AccessorViewStatus::BufferTooSmall);
    EXPECT_EQ(AccessorViewStatus::BufferTooSmall, view.status());
    EXPECT_EQ(0, view.size());
    EXPECT_FALSE(static_cast<bool>(view));
}

TEST(AccessorViewTest, ValidViewReturnsData) {
    std::vector<uint8_t> buffer = {
        0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,
    };
    AccessorView<int32_t> view(buffer.data(), 4, 0, 3);
    EXPECT_TRUE(static_cast<bool>(view));
    EXPECT_EQ(AccessorViewStatus::Valid, view.status());
    EXPECT_EQ(3, view.size());
    EXPECT_EQ(1, view[0]);
    EXPECT_EQ(2, view[1]);
    EXPECT_EQ(3, view[2]);
}

TEST(AccessorViewTest, ThrowsOnOutOfRange) {
    std::vector<uint8_t> buffer(4, 0);
    AccessorView<int32_t> view(buffer.data(), 4, 0, 1);
    EXPECT_THROW(view[1], std::range_error);
    EXPECT_THROW(view[-1], std::range_error);
}

TEST(AccessorViewTest, HandlesStride) {
    // Interleaved data: [a0, a1, b0, b1, c0, c1]
    // Stride = 6, each element = 4 bytes (only first 4 bytes of stride used)
    std::vector<uint8_t> buffer = {
        0x01, 0x00, 0x00, 0x00, 0xFF, 0xFF,
        0x02, 0x00, 0x00, 0x00, 0xEE, 0xEE,
    };
    AccessorView<int32_t> view(buffer.data(), 6, 0, 2);
    EXPECT_EQ(2, view.size());
    EXPECT_EQ(1, view[0]);
    EXPECT_EQ(2, view[1]);
}

TEST(AccessorViewTest, HandlesOffset) {
    std::vector<uint8_t> buffer = {
        0xFF, 0xFF, 0xFF, 0xFF,  // padding
        0x0A, 0x00, 0x00, 0x00,  // element 0
        0x14, 0x00, 0x00, 0x00,  // element 1
    };
    AccessorView<int32_t> view(buffer.data(), 4, 4, 2);
    EXPECT_EQ(2, view.size());
    EXPECT_EQ(10, view[0]);
    EXPECT_EQ(20, view[1]);
}

TEST(AccessorViewTest, AccessorTypesScalar) {
    std::vector<uint8_t> buffer(4, 0);
    AccessorTypes::SCALAR<float>* p =
        reinterpret_cast<AccessorTypes::SCALAR<float>*>(buffer.data());
    p->value[0] = 3.14f;
    AccessorView<AccessorTypes::SCALAR<float>> view(buffer.data(), 4, 0, 1);
    EXPECT_FLOAT_EQ(3.14f, view[0].value[0]);
}

TEST(AccessorViewTest, AccessorTypesVec3) {
    std::vector<uint8_t> buffer(12, 0);
    AccessorTypes::VEC3<float>* p =
        reinterpret_cast<AccessorTypes::VEC3<float>*>(buffer.data());
    p->value[0] = 1.0f;
    p->value[1] = 2.0f;
    p->value[2] = 3.0f;
    AccessorView<AccessorTypes::VEC3<float>> view(buffer.data(), 12, 0, 1);
    EXPECT_FLOAT_EQ(1.0f, view[0].value[0]);
    EXPECT_FLOAT_EQ(2.0f, view[0].value[1]);
    EXPECT_FLOAT_EQ(3.0f, view[0].value[2]);
}

TEST(AccessorViewTest, DataPointer) {
    std::vector<uint8_t> buffer(8, 0);
    AccessorView<int32_t> view(buffer.data(), 4, 4, 1);
    EXPECT_EQ(buffer.data() + 4, view.data());
}

TEST(AccessorViewTest, StrideAndOffsetAccessors) {
    std::vector<uint8_t> buffer(16, 0);
    AccessorView<int32_t> view(buffer.data(), 8, 4, 2);
    EXPECT_EQ(8, view.stride());
    EXPECT_EQ(4, view.offset());
}
