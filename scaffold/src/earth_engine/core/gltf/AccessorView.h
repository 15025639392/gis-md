#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace earth_engine {

/**
 * @brief Indicates the status of an accessor view.
 *
 * Mirrors cesium-native CesiumGltf::AccessorViewStatus.
 */
enum class AccessorViewStatus {
    Valid,
    InvalidAccessorIndex,
    InvalidBufferViewIndex,
    InvalidBufferIndex,
    BufferViewTooSmall,
    BufferTooSmall,
    WrongSizeT,
    InvalidType,
    InvalidComponentType,
    InvalidByteStride,
};

/**
 * @brief A view on typed data from a raw buffer.
 *
 * Provides type-safe access to elements in a byte buffer with
 * configurable stride and offset. Mirrors cesium-native
 * CesiumGltf::AccessorView<T>.
 *
 * @tparam T The element type (e.g., float, glm::dvec3, etc.)
 */
template <class T>
class AccessorView final {
public:
    using value_type = T;

    AccessorView() noexcept
        : pData_(nullptr), stride_(0), offset_(0), size_(0),
          status_(AccessorViewStatus::InvalidAccessorIndex) {}

    AccessorView(AccessorViewStatus status) noexcept
        : pData_(nullptr), stride_(0), offset_(0), size_(0), status_(status) {}

    AccessorView(const uint8_t* pData, int64_t stride, int64_t offset,
                 int64_t size) noexcept
        : pData_(pData), stride_(stride), offset_(offset), size_(size),
          status_(AccessorViewStatus::Valid) {}

    const T& operator[](int64_t i) const {
        if (i < 0 || i >= size_) {
            throw std::range_error("AccessorView index out of range");
        }
        return *reinterpret_cast<const T*>(pData_ + i * stride_ + offset_);
    }

    int64_t size() const noexcept { return size_; }
    AccessorViewStatus status() const noexcept { return status_; }
    int64_t stride() const noexcept { return stride_; }
    int64_t offset() const noexcept { return offset_; }
    const uint8_t* data() const noexcept { return pData_ + offset_; }

    explicit operator bool() const noexcept {
        return status_ == AccessorViewStatus::Valid && pData_ != nullptr;
    }

private:
    const uint8_t* pData_;
    int64_t stride_;
    int64_t offset_;
    int64_t size_;
    AccessorViewStatus status_;
};

/**
 * @brief Packed struct types for glTF accessor element shapes.
 *
 * Mirrors cesium-native CesiumGltf::AccessorTypes.
 * Uses #pragma pack to ensure tight packing matching glTF binary layout.
 */
struct AccessorTypes {
#pragma pack(push, 1)
    template <typename T> struct SCALAR { T value[1]; };
    template <typename T> struct VEC2   { T value[2]; };
    template <typename T> struct VEC3   { T value[3]; };
    template <typename T> struct VEC4   { T value[4]; };
    template <typename T> struct MAT2   { T value[4]; };
    template <typename T> struct MAT3   { T value[9]; };
    template <typename T> struct MAT4   { T value[16]; };
#pragma pack(pop)
};

namespace CesiumImpl {

template <typename TCallback, typename TElement>
auto createAccessorView(const uint8_t* base, int64_t stride, int64_t offset,
                        int64_t count, int32_t type, TCallback&& callback)
    -> std::invoke_result_t<TCallback, AccessorView<AccessorTypes::SCALAR<TElement>>> {
    using SCALAR_T = AccessorTypes::SCALAR<TElement>;
    switch (type) {
        case 0: return callback(AccessorView<SCALAR_T>(base, stride, offset, count));
        case 1: return callback(AccessorView<AccessorTypes::VEC2<TElement>>(base, stride, offset, count));
        case 2: return callback(AccessorView<AccessorTypes::VEC3<TElement>>(base, stride, offset, count));
        case 3: return callback(AccessorView<AccessorTypes::VEC4<TElement>>(base, stride, offset, count));
        case 4: return callback(AccessorView<AccessorTypes::MAT2<TElement>>(base, stride, offset, count));
        case 5: return callback(AccessorView<AccessorTypes::MAT3<TElement>>(base, stride, offset, count));
        case 6: return callback(AccessorView<AccessorTypes::MAT4<TElement>>(base, stride, offset, count));
        default:
            return callback(AccessorView<SCALAR_T>(AccessorViewStatus::InvalidType));
    }
}

} // namespace CesiumImpl

} // namespace earth_engine
