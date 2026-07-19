#import "RenderDeviceMetal.h"

#include "../../renderer/RenderCommand.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <string>
#include <stdexcept>
#include <cstring>
#include <unordered_map>

namespace earth_engine {

// ============================================================
// Internal Metal resource wrappers
// ============================================================

class MetalTexture : public Texture {
public:
    MetalTexture(id<MTLTexture> tex, id<MTLSamplerState> sampler)
        : tex_(tex), sampler_(sampler) {}
    int width() const override { return static_cast<int>(tex_.width); }
    int height() const override { return static_cast<int>(tex_.height); }
    size_t sizeBytes() const override {
        return static_cast<size_t>(tex_.allocatedSize);
    }
    id<MTLTexture> mtl() const { return tex_; }
    id<MTLSamplerState> sampler() const { return sampler_; }
private:
    id<MTLTexture> tex_;
    id<MTLSamplerState> sampler_;
};

class MetalBuffer : public Buffer {
public:
    MetalBuffer(id<MTLBuffer> buf) : buf_(buf) {}
    size_t size() const override { return buf_.length; }
    id<MTLBuffer> mtl() const { return buf_; }
    // updateBuffer 的 orphan 式换存储:在飞 command buffer 持有旧 id 的
    // 强引用,换新后 CPU 写不再与 GPU 读竞争(P1-5)。
    void replaceStorage(id<MTLBuffer> buf) { buf_ = buf; }
private:
    id<MTLBuffer> buf_;
};

class MetalShaderProgram : public ShaderProgram {
public:
    MetalShaderProgram(id<MTLRenderPipelineState> opaquePso,
                       id<MTLRenderPipelineState> blendedPso,
                       id<MTLDepthStencilState> depthState,
                       bool isTerrain = false)
        : opaquePso_(opaquePso), blendedPso_(blendedPso),
          depthState_(depthState), isTerrain_(isTerrain) {}
    id<MTLRenderPipelineState> pso(bool blend) const {
        return blend ? blendedPso_ : opaquePso_;
    }
    id<MTLDepthStencilState> depthState() const { return depthState_; }
    // True when this program uses the lightweight terrain shader
    // (terrainVertex/terrainFragment). The terrain shader uses a compact
    // fragment buffer table distinct from the glTF one, so submit() must bind
    // its uniforms at the terrain indices instead of the glTF indices.
    bool isTerrain() const { return isTerrain_; }
private:
    id<MTLRenderPipelineState> opaquePso_;
    id<MTLRenderPipelineState> blendedPso_;
    id<MTLDepthStencilState> depthState_;
    bool isTerrain_ = false;
};

/// 离屏 framebuffer:color 恒为可采样 MTLTexture(经 MetalTexture 包装,
/// 可直接进 RenderCommand::textures);depth 独立 Depth32Float 纹理,与主
/// pass 深度同格式(reverse-Z)。pass descriptor 每次 beginPass 现建。
class MetalFramebuffer : public Framebuffer {
public:
    MetalFramebuffer(std::unique_ptr<MetalTexture> color,
                     id<MTLTexture> depth,
                     std::unique_ptr<MetalTexture> depthSampleable,
                     int width,
                     int height)
        : color_(std::move(color)), depth_(depth),
          depthSampleable_(std::move(depthSampleable)),
          width_(width), height_(height) {}
    int width() const override { return width_; }
    int height() const override { return height_; }
    Texture* colorTexture() const override { return color_.get(); }
    // 仅当 depthSampleable 时非空(否则 depth 仅作 render target)。
    Texture* depthTexture() const override { return depthSampleable_.get(); }
    id<MTLTexture> colorMtl() const { return color_->mtl(); }
    id<MTLTexture> depthMtl() const { return depth_; }
private:
    std::unique_ptr<MetalTexture> color_;
    id<MTLTexture> depth_;  // nil = 无 depth attachment;render-pass depth 句柄
    std::unique_ptr<MetalTexture> depthSampleable_;  // 非空 = 可采样 depth 包装
    int width_, height_;
};

// ============================================================
// RenderDeviceMetal::Impl
// ============================================================

struct RenderDeviceMetal::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> commandQueue = nil;
    CAMetalLayer* metalLayer = nil;
    id<MTLRenderCommandEncoder> currentEncoder = nil;
    id<MTLCommandBuffer> currentCommandBuffer = nil;
    id<CAMetalDrawable> currentDrawable = nil;
    // in-flight 帧上限(P1-5):CPU 最多领先 GPU kMaxFramesInFlight 帧,
    // completed handler 归还名额。防止编码无界领先导致 drawable 饥饿与
    // 输入延迟累积。
    dispatch_semaphore_t inFlightSemaphore = nil;
    id<MTLTexture> depthTexture = nil;
    id<MTLSamplerState> linearClampSampler = nil;
    // sampler 按配置去重(P2-15):Apple GPU 存活 sampler 有 1024/2048 硬
    // 上限,逼近时静默回落默认配置;全项目配置种类只有个位数,按打包 key
    // 复用让存活数与纹理数解耦。仅渲染线程访问,无锁。
    std::unordered_map<uint64_t, id<MTLSamplerState>> samplerCache;
    id<MTLDepthStencilState> depthReadWrite = nil;
    id<MTLDepthStencilState> depthReadOnly = nil;
    id<MTLDepthStencilState> depthDisabled = nil;
    int viewportWidth = 0;
    int viewportHeight = 0;
    // Sky clear color pushed by Engine each frame via setClearColor().
    // Defaults match FrameState (dark night blue) until the first update.
    double clearR = 0.02;
    double clearG = 0.02;
    double clearB = 0.08;
    double clearA = 1.0;
};

static id<MTLDepthStencilState> makeDepthState(id<MTLDevice> device,
                                               bool enabled,
                                               bool write) {
    MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
    // Reverse-Z: greater depth = closer. Matches OpenGlobus reverseDepth:true.
    desc.depthCompareFunction = enabled ? MTLCompareFunctionGreaterEqual : MTLCompareFunctionAlways;
    desc.depthWriteEnabled = enabled && write;
    return [device newDepthStencilStateWithDescriptor:desc];
}

static MTLSamplerMinMagFilter toMetalFilter(TextureDesc::Filter filter) {
    return filter == TextureDesc::Filter::Nearest
        ? MTLSamplerMinMagFilterNearest
        : MTLSamplerMinMagFilterLinear;
}

static MTLSamplerAddressMode toMetalAddressMode(TextureDesc::Wrap wrap) {
    switch (wrap) {
        case TextureDesc::Wrap::Repeat:
            return MTLSamplerAddressModeRepeat;
        case TextureDesc::Wrap::MirroredRepeat:
            return MTLSamplerAddressModeMirrorRepeat;
        case TextureDesc::Wrap::Clamp:
        default:
            return MTLSamplerAddressModeClampToEdge;
    }
}

// ============================================================
// RenderDeviceMetal
// ============================================================

// CPU 允许领先 GPU 的帧数。2 = 编码一帧、GPU 执行一帧(Apple 低延迟推荐
// 值);drawable 池为 3,留一个余量给 present 队列。
static constexpr intptr_t kMaxFramesInFlight = 2;

RenderDeviceMetal::RenderDeviceMetal(void* metalLayer)
    : impl_(new Impl()) {
    impl_->metalLayer = (__bridge CAMetalLayer*)metalLayer;
    impl_->device = impl_->metalLayer.device;
    impl_->commandQueue = [impl_->device newCommandQueue];
    impl_->inFlightSemaphore = dispatch_semaphore_create(kMaxFramesInFlight);
    impl_->depthReadWrite = makeDepthState(impl_->device, true, true);
    impl_->depthReadOnly = makeDepthState(impl_->device, true, false);
    impl_->depthDisabled = makeDepthState(impl_->device, false, false);

    MTLSamplerDescriptor* samplerDesc = [MTLSamplerDescriptor new];
    samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.mipFilter = MTLSamplerMipFilterLinear;
    samplerDesc.maxAnisotropy = 4;
    samplerDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    samplerDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    impl_->linearClampSampler = [impl_->device newSamplerStateWithDescriptor:samplerDesc];
}

RenderDeviceMetal::~RenderDeviceMetal() {
    onSurfaceDestroyed();
    delete impl_;
}

int RenderDeviceMetal::maxTextureSize() const {
    return 4096;  // Metal 2 minimum guaranteed
}

int RenderDeviceMetal::maxDrawBuffers() const {
    return 4;
}

bool RenderDeviceMetal::supportsFloatTextures() const {
    return true;
}

bool RenderDeviceMetal::supportsInstancing() const {
    return true;
}

std::string RenderDeviceMetal::rendererString() const {
    return impl_->device.name.UTF8String ?: "Unknown Metal";
}

// ============================================================
// 资源创建
// ============================================================

std::unique_ptr<Texture> RenderDeviceMetal::createTexture(const TextureDesc& desc) {
    MTLPixelFormat pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (desc.format == TextureDesc::Format::R8) {
        pixelFormat = MTLPixelFormatR8Unorm;
    }

    // ---- texture2DArray 路径(合成方案页存储:一页一层)----
    // 只分配层存储,各层随后经 updateTextureRegion(layer) 的 slice 维上传。
    // Step 2 骨架不建 array mip 链(每层独立 mip 属 §12.5 #3 后续缓解)。
    const bool isArray = desc.arrayLayers > 1;
    MTLTextureDescriptor* texDesc = nil;
    if (isArray) {
        texDesc = [[MTLTextureDescriptor alloc] init];
        texDesc.textureType = MTLTextureType2DArray;
        texDesc.pixelFormat = pixelFormat;
        texDesc.width = static_cast<NSUInteger>(desc.width);
        texDesc.height = static_cast<NSUInteger>(desc.height);
        texDesc.arrayLength = static_cast<NSUInteger>(desc.arrayLayers);
        texDesc.mipmapLevelCount = 1;
    } else {
        texDesc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:pixelFormat
                                         width:static_cast<NSUInteger>(desc.width)
                                        height:static_cast<NSUInteger>(desc.height)
                                     mipmapped:desc.mipmap];
    }

    id<MTLTexture> tex = [impl_->device newTextureWithDescriptor:texDesc];
    if (!tex) return nullptr;

    // 数组纹理创建时不带初始 data(逐层经 updateTextureRegion 上传)。
    if (desc.data && !isArray) {
        MTLRegion region = MTLRegionMake2D(0, 0,
                                           static_cast<NSUInteger>(desc.width),
                                           static_cast<NSUInteger>(desc.height));
        [tex replaceRegion:region
               mipmapLevel:0
                 withBytes:desc.data
               bytesPerRow:static_cast<NSUInteger>(
                   desc.width *
                   (desc.format == TextureDesc::Format::R8 ? 1 : 4))];
        // mip 链已按 mipmapped 分配,必须 blit 生成(P1-6),否则三线性
        // 缩小采样读到未初始化 mip。语义对齐 GLES createTexture 的
        // glGenerateMipmap(仅 create-with-data 生成,区域更新不重生成)。
        // 同一 queue 上后续帧的采样天然排在本 blit 之后,无需同步等待。
        if (desc.mipmap && tex.mipmapLevelCount > 1) {
            id<MTLCommandBuffer> blitCommandBuffer =
                [impl_->commandQueue commandBuffer];
            id<MTLBlitCommandEncoder> blit =
                [blitCommandBuffer blitCommandEncoder];
            [blit generateMipmapsForTexture:tex];
            [blit endEncoding];
            [blitCommandBuffer commit];
        }
    }

    const NSUInteger anisotropy =
        static_cast<NSUInteger>(std::max(1.0f, desc.maxAnisotropy));
    const uint64_t samplerKey =
        (static_cast<uint64_t>(desc.minFilter) << 0) |
        (static_cast<uint64_t>(desc.magFilter) << 4) |
        (static_cast<uint64_t>(desc.mipmap ? 1 : 0) << 8) |
        (static_cast<uint64_t>(desc.wrapS) << 9) |
        (static_cast<uint64_t>(desc.wrapT) << 13) |
        (static_cast<uint64_t>(anisotropy) << 17);
    id<MTLSamplerState> sampler = nil;
    auto cachedSampler = impl_->samplerCache.find(samplerKey);
    if (cachedSampler != impl_->samplerCache.end()) {
        sampler = cachedSampler->second;
    } else {
        MTLSamplerDescriptor* samplerDesc = [MTLSamplerDescriptor new];
        samplerDesc.minFilter = toMetalFilter(desc.minFilter);
        samplerDesc.magFilter = toMetalFilter(desc.magFilter);
        samplerDesc.mipFilter = desc.mipmap
            ? MTLSamplerMipFilterLinear
            : MTLSamplerMipFilterNotMipmapped;
        samplerDesc.maxAnisotropy = anisotropy;
        samplerDesc.sAddressMode = toMetalAddressMode(desc.wrapS);
        samplerDesc.tAddressMode = toMetalAddressMode(desc.wrapT);
        sampler = [impl_->device newSamplerStateWithDescriptor:samplerDesc];
        if (sampler) {
            impl_->samplerCache[samplerKey] = sampler;
        }
    }

    return std::make_unique<MetalTexture>(tex, sampler ?: impl_->linearClampSampler);
}

bool RenderDeviceMetal::updateTextureRegion(Texture* texture,
                                            int x,
                                            int y,
                                            int width,
                                            int height,
                                            const uint8_t* data,
                                            size_t rowBytes,
                                            int layer) {
    auto* metalTexture = static_cast<MetalTexture*>(texture);
    if (!metalTexture || !data || width <= 0 || height <= 0) {
        return false;
    }
    if (x < 0 || y < 0 ||
        x + width > metalTexture->width() ||
        y + height > metalTexture->height()) {
        return false;
    }
    id<MTLTexture> tex = metalTexture->mtl();
    const bool isArray = tex.textureType == MTLTextureType2DArray;
    // 普通 2D 只接受 slice 0;数组纹理 slice 须落在已分配层内。
    if (layer < 0 ||
        (isArray ? static_cast<NSUInteger>(layer) >= tex.arrayLength
                 : layer != 0)) {
        return false;
    }
    MTLRegion region = MTLRegionMake2D(static_cast<NSUInteger>(x),
                                       static_cast<NSUInteger>(y),
                                       static_cast<NSUInteger>(width),
                                       static_cast<NSUInteger>(height));
    // 合成方案页上传:slice = 目标 layer(2D 纹理恒为 0)。
    [tex replaceRegion:region
           mipmapLevel:0
                 slice:static_cast<NSUInteger>(layer)
             withBytes:data
           bytesPerRow:static_cast<NSUInteger>(rowBytes)
         bytesPerImage:0];
    return true;
}

std::unique_ptr<Buffer> RenderDeviceMetal::createBuffer(const BufferDesc& desc) {
    MTLResourceOptions options = MTLResourceStorageModeShared;
    id<MTLBuffer> buf = desc.data
        ? [impl_->device newBufferWithBytes:desc.data
                                      length:desc.size
                                     options:options]
        : [impl_->device newBufferWithLength:desc.size
                                     options:options];
    return buf ? std::make_unique<MetalBuffer>(buf) : nullptr;
}

bool RenderDeviceMetal::updateBuffer(Buffer* buffer,
                                     size_t offset,
                                     const void* data,
                                     size_t size) {
    auto* metalBuffer = static_cast<MetalBuffer*>(buffer);
    if (!metalBuffer || !data || size == 0 || offset + size > metalBuffer->size()) {
        return false;
    }
    // 旧存储可能仍被在飞 command buffer 读取(shared storage,动画 glTF
    // 每帧覆写)——原地 memcpy 是 CPU 写/GPU 读裸竞争(P1-5)。改为 orphan:
    // 换一块新 MTLBuffer,已提交的 command buffer 持有旧 id 的强引用直到
    // 执行完毕;后续编码经 mtl() 取到新存储。调用方唯一热路径是动画顶点
    // 全量重写(newBufferWithBytes 一步到位),部分更新走拷贝合成。
    id<MTLBuffer> fresh = nil;
    if (offset == 0 && size == metalBuffer->size()) {
        fresh = [impl_->device newBufferWithBytes:data
                                           length:size
                                          options:MTLResourceStorageModeShared];
    } else {
        fresh = [impl_->device newBufferWithLength:metalBuffer->size()
                                           options:MTLResourceStorageModeShared];
        if (fresh) {
            std::memcpy(fresh.contents,
                        metalBuffer->mtl().contents,
                        metalBuffer->size());
            std::memcpy(static_cast<uint8_t*>(fresh.contents) + offset,
                        data,
                        size);
        }
    }
    if (!fresh) {
        return false;
    }
    metalBuffer->replaceStorage(fresh);
    return true;
}

std::unique_ptr<ShaderProgram> RenderDeviceMetal::createShader(const ShaderDesc& desc) {
    NSError* error = nil;

    // 编译 MSL 源码
    NSString* source = [NSString stringWithUTF8String:desc.vertexSource.c_str()];
    // 实际上需要 vertex + fragment；这里把两者拼接或用 separate libraries
    // 简便做法：把 vertex 和 fragment 源码拼接（MSL 允许多个函数在同一 library）
    fprintf(stderr, "[createShader] vertexSrc length=%zu, first 50 chars: %.50s\n",
        desc.vertexSource.length(), desc.vertexSource.c_str());
    fprintf(stderr, "[createShader] fragSrc length=%zu, first 50 chars: %.50s\n",
        desc.fragmentSource.length(), desc.fragmentSource.c_str());
    NSString* vertexStr = [NSString stringWithUTF8String:desc.vertexSource.c_str()];
    NSString* fragStr = [NSString stringWithUTF8String:desc.fragmentSource.c_str()];
    NSString* combinedSource = [vertexStr stringByAppendingFormat:@"\n%@", fragStr];

    id<MTLLibrary> library = [impl_->device newLibraryWithSource:combinedSource
                                                         options:nil
                                                           error:&error];
    if (!library) {
        if (error) {
            fprintf(stderr, "Metal shader compile error: %s\n",
                [[error localizedDescription] UTF8String]);
            fprintf(stderr, "Combined source:\n%s\n",
                [combinedSource UTF8String]);
        }
        return nullptr;
    }

    enum class PipelineLayout {
        Surface,
        Terrain,
        Tile,
        Gltf,
        GltfInstanced,
        Color,
        DebugLine
    };
    PipelineLayout layout = PipelineLayout::Surface;

    // Probe the terrain lightweight shader FIRST (unique entry points) so it is
    // never mis-detected as tile/gltf.
    id<MTLFunction> vertexFunc = [library newFunctionWithName:@"terrainVertex"];
    id<MTLFunction> fragmentFunc = [library newFunctionWithName:@"terrainFragment"];
    if (vertexFunc && fragmentFunc) {
        layout = PipelineLayout::Terrain;
    }

    // 尝试识别 shader 类型（tile / gltf / vector color / debug line）
    if (!vertexFunc || !fragmentFunc) {
        fprintf(stderr, "[createShader] trying tileVertex / tileFragment\n");
        vertexFunc = [library newFunctionWithName:@"tileVertex"];
        fragmentFunc = [library newFunctionWithName:@"tileFragment"];
        if (vertexFunc && fragmentFunc) layout = PipelineLayout::Tile;
    }
    fprintf(stderr, "[createShader] tileVertex=%p tileFragment=%p\n", (void*)vertexFunc, (void*)fragmentFunc);
    if (!vertexFunc || !fragmentFunc) {
        fprintf(stderr, "[createShader] tile not found, trying colorVertex / colorFragment\n");
    }
    if (!vertexFunc || !fragmentFunc) {
        vertexFunc = [library newFunctionWithName:@"gltfInstancedVertex"];
        fragmentFunc = [library newFunctionWithName:@"gltfFragment"];
        if (vertexFunc && fragmentFunc) layout = PipelineLayout::GltfInstanced;
    }
    if (!vertexFunc || !fragmentFunc) {
        vertexFunc = [library newFunctionWithName:@"gltfVertex"];
        fragmentFunc = [library newFunctionWithName:@"gltfFragment"];
        if (vertexFunc && fragmentFunc) layout = PipelineLayout::Gltf;
    }
    if (!vertexFunc || !fragmentFunc) {
        vertexFunc = [library newFunctionWithName:@"colorVertex"];
        fragmentFunc = [library newFunctionWithName:@"colorFragment"];
        layout = PipelineLayout::Color;
    }
    if (!vertexFunc || !fragmentFunc) {
        vertexFunc = [library newFunctionWithName:@"debugLineVertex"];
        fragmentFunc = [library newFunctionWithName:@"debugLineFragment"];
        layout = PipelineLayout::DebugLine;
    }

    if (!vertexFunc || !fragmentFunc) {
        fprintf(stderr, "[createShader] ALL entry-point lookups FAILED\n");
        return nullptr;
    }

    // Vertex descriptor: SurfaceTile uses the position/normal/uv layout.
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    if (layout == PipelineLayout::DebugLine) {
        vd.attributes[0].format = MTLVertexFormatFloat2;   // texcoord
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.layouts[0].stride = 8;
    } else if (layout == PipelineLayout::Tile) {
        vd.attributes[0].format = MTLVertexFormatFloat3;   // position
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat2;   // texcoord (no normal — computed in shader)
        vd.attributes[1].offset = 12;
        vd.attributes[1].bufferIndex = 0;
        vd.layouts[0].stride = 20;
    } else if (layout == PipelineLayout::Color) {
        vd.attributes[0].format = MTLVertexFormatFloat3;   // position
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.layouts[0].stride = 12;
    } else if (layout == PipelineLayout::Surface) {
        // Surface layout: POSITION(12) + NORMAL(12) + TEXCOORD_0(8).
        vd.attributes[0].format = MTLVertexFormatFloat3;   // position
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat3;   // normal
        vd.attributes[1].offset = 12;
        vd.attributes[1].bufferIndex = 0;
        vd.attributes[2].format = MTLVertexFormatFloat2;   // texcoord
        vd.attributes[2].offset = 24;
        vd.attributes[2].bufferIndex = 0;
        vd.layouts[0].stride = 32;
    } else if (layout == PipelineLayout::Terrain) {
        // Terrain layout (32B): POSITION f32x3(12) + NORMAL snorm16x3+pad(8)
        // + packed TEXCOORD_0/1 unorm16x4(8) + geomorph heightDelta f32(4).
        // Metal has no short3Normalized, so the normal reads as
        // short4Normalized covering the pad short (shader consumes .xyz only).
        vd.attributes[0].format = MTLVertexFormatFloat3;
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatShort4Normalized;
        vd.attributes[1].offset = 12;
        vd.attributes[1].bufferIndex = 0;
        vd.attributes[2].format = MTLVertexFormatUShort4Normalized;
        vd.attributes[2].offset = 20;
        vd.attributes[2].bufferIndex = 0;
        vd.attributes[3].format = MTLVertexFormatFloat;  // geomorph heightDelta
        vd.attributes[3].offset = 28;
        vd.attributes[3].bufferIndex = 0;
        vd.layouts[0].stride = 32;
    } else if (layout == PipelineLayout::Gltf) {
        vd.attributes[0].format = MTLVertexFormatFloat3;   // position
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat3;   // normal
        vd.attributes[1].offset = 12;
        vd.attributes[1].bufferIndex = 0;
        vd.attributes[2].format = MTLVertexFormatFloat4;   // texcoord 0/1
        vd.attributes[2].offset = 24;
        vd.attributes[2].bufferIndex = 0;
        vd.attributes[10].format = MTLVertexFormatFloat4;  // COLOR_0
        vd.attributes[10].offset = 40;
        vd.attributes[10].bufferIndex = 0;
        vd.attributes[11].format = MTLVertexFormatFloat4;  // TANGENT
        vd.attributes[11].offset = 56;
        vd.attributes[11].bufferIndex = 0;
        vd.attributes[12].format = MTLVertexFormatFloat4;  // texcoord 2/3
        vd.attributes[12].offset = 72;
        vd.attributes[12].bufferIndex = 0;
        vd.attributes[13].format = MTLVertexFormatFloat4;  // texcoord 4/5
        vd.attributes[13].offset = 88;
        vd.attributes[13].bufferIndex = 0;
        vd.attributes[14].format = MTLVertexFormatFloat4;  // texcoord 6/7
        vd.attributes[14].offset = 104;
        vd.attributes[14].bufferIndex = 0;
        vd.layouts[0].stride = 120;
    } else if (layout == PipelineLayout::GltfInstanced) {
        vd.attributes[0].format = MTLVertexFormatFloat3;   // position
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat3;   // normal
        vd.attributes[1].offset = 12;
        vd.attributes[1].bufferIndex = 0;
        vd.attributes[2].format = MTLVertexFormatFloat4;   // texcoord 0/1
        vd.attributes[2].offset = 24;
        vd.attributes[2].bufferIndex = 0;
        vd.attributes[10].format = MTLVertexFormatFloat4;  // COLOR_0
        vd.attributes[10].offset = 40;
        vd.attributes[10].bufferIndex = 0;
        vd.attributes[11].format = MTLVertexFormatFloat4;  // TANGENT
        vd.attributes[11].offset = 56;
        vd.attributes[11].bufferIndex = 0;
        vd.attributes[12].format = MTLVertexFormatFloat4;  // texcoord 2/3
        vd.attributes[12].offset = 72;
        vd.attributes[12].bufferIndex = 0;
        vd.attributes[13].format = MTLVertexFormatFloat4;  // texcoord 4/5
        vd.attributes[13].offset = 88;
        vd.attributes[13].bufferIndex = 0;
        vd.attributes[14].format = MTLVertexFormatFloat4;  // texcoord 6/7
        vd.attributes[14].offset = 104;
        vd.attributes[14].bufferIndex = 0;
        vd.layouts[0].stride = 120;

        vd.attributes[3].format = MTLVertexFormatFloat4;   // instance matrix col0
        vd.attributes[3].offset = 0;
        vd.attributes[3].bufferIndex = 7;
        vd.attributes[4].format = MTLVertexFormatFloat4;
        vd.attributes[4].offset = 16;
        vd.attributes[4].bufferIndex = 7;
        vd.attributes[5].format = MTLVertexFormatFloat4;
        vd.attributes[5].offset = 32;
        vd.attributes[5].bufferIndex = 7;
        vd.attributes[6].format = MTLVertexFormatFloat4;
        vd.attributes[6].offset = 48;
        vd.attributes[6].bufferIndex = 7;
        vd.attributes[7].format = MTLVertexFormatFloat3;   // normal matrix col0
        vd.attributes[7].offset = 64;
        vd.attributes[7].bufferIndex = 7;
        vd.attributes[8].format = MTLVertexFormatFloat3;
        vd.attributes[8].offset = 76;
        vd.attributes[8].bufferIndex = 7;
        vd.attributes[9].format = MTLVertexFormatFloat3;
        vd.attributes[9].offset = 88;
        vd.attributes[9].bufferIndex = 7;
        vd.layouts[7].stride = kGltfInstanceMatrixStride;
        vd.layouts[7].stepFunction = MTLVertexStepFunctionPerInstance;
        vd.layouts[7].stepRate = 1;
    }
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* pipeDesc = [MTLRenderPipelineDescriptor new];
    pipeDesc.vertexFunction = vertexFunc;
    pipeDesc.fragmentFunction = fragmentFunc;
    pipeDesc.vertexDescriptor = vd;
    pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    pipeDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

    pipeDesc.colorAttachments[0].blendingEnabled = NO;
    id<MTLRenderPipelineState> opaquePso =
        [impl_->device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
    if (!opaquePso) {
        if (error) {
            NSLog(@"Metal opaque pipeline error: %@", error.localizedDescription);
        }
        return nullptr;
    }

    error = nil;
    pipeDesc.colorAttachments[0].blendingEnabled = YES;
    id<MTLRenderPipelineState> blendedPso =
        [impl_->device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
    if (!blendedPso) {
        if (error) {
            NSLog(@"Metal blended pipeline error: %@", error.localizedDescription);
        }
        return nullptr;
    }

    return std::make_unique<MetalShaderProgram>(
        opaquePso,
        blendedPso,
        impl_->depthReadWrite,
        layout == PipelineLayout::Terrain);
}

std::unique_ptr<Framebuffer> RenderDeviceMetal::createFramebuffer(const FramebufferDesc& desc) {
    if (desc.width <= 0 || desc.height <= 0 || !desc.hasColor) {
        NSLog(@"createFramebuffer: invalid desc %dx%d hasColor=%d",
              desc.width, desc.height, desc.hasColor ? 1 : 0);
        return nullptr;
    }
    if (desc.samples > 1) {
        // v1 不支持 MSAA(resolve 未实现),按单采样处理而不是假装支持。
        NSLog(@"createFramebuffer: samples=%d unsupported, using 1", desc.samples);
    }

    // color 格式必须与既有 PSO 的 colorAttachments[0](BGRA8Unorm)一致,
    // 否则场景 PSO 在离屏 pass 里全部失效。
    MTLTextureDescriptor* colorDesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:desc.width
                                    height:desc.height
                                 mipmapped:NO];
    colorDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    colorDesc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> colorTex = [impl_->device newTextureWithDescriptor:colorDesc];
    if (!colorTex) {
        NSLog(@"createFramebuffer: color texture alloc failed %dx%d",
              desc.width, desc.height);
        return nullptr;
    }

    id<MTLTexture> depthTex = nil;
    if (desc.hasDepth) {
        MTLTextureDescriptor* depthDesc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                         width:desc.width
                                        height:desc.height
                                     mipmapped:NO];
        // depthSampleable 时加 ShaderRead 让后续 pass(如 aerial fog)采样。
        depthDesc.usage = desc.depthSampleable
            ? (MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead)
            : MTLTextureUsageRenderTarget;
        depthDesc.storageMode = MTLStorageModePrivate;
        depthTex = [impl_->device newTextureWithDescriptor:depthDesc];
        if (!depthTex) {
            NSLog(@"createFramebuffer: depth texture alloc failed %dx%d",
                  desc.width, desc.height);
            return nullptr;
        }
    }

    auto color = std::make_unique<MetalTexture>(colorTex,
                                                impl_->linearClampSampler);
    // 可采样深度包装成 MetalTexture,经 depthTexture() 进 textures[] 绑定路径。
    std::unique_ptr<MetalTexture> depthSampleable;
    if (depthTex && desc.depthSampleable) {
        depthSampleable = std::make_unique<MetalTexture>(
            depthTex, impl_->linearClampSampler);
    }
    return std::make_unique<MetalFramebuffer>(
        std::move(color), depthTex, std::move(depthSampleable),
        desc.width, desc.height);
}

// ============================================================
// 帧操作
// ============================================================

void RenderDeviceMetal::setClearColor(float r, float g, float b, float a) {
    impl_->clearR = r;
    impl_->clearG = g;
    impl_->clearB = b;
    impl_->clearA = a;
}

void RenderDeviceMetal::beginFrame() {
    // in-flight 帧闸门(P1-5)。带超时:GPU 异常悬挂时跳帧而不是把 UI 线程
    // 永久卡死(P1-4);超时未拿到名额则本帧不消耗信号量,直接空帧。
    if (dispatch_semaphore_wait(
            impl_->inFlightSemaphore,
            dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) != 0) {
        impl_->currentCommandBuffer = nil;
        return;
    }

    impl_->currentCommandBuffer = [impl_->commandQueue commandBuffer];

    // 获取 current drawable(layer 侧 allowsNextDrawableTimeout=YES,
    // 超时返回 nil → 跳帧,归还 in-flight 名额)
    id<CAMetalDrawable> drawable = [impl_->metalLayer nextDrawable];
    if (!drawable) {
        dispatch_semaphore_signal(impl_->inFlightSemaphore);
        impl_->currentCommandBuffer = nil;
        return;
    }

    dispatch_semaphore_t inFlight = impl_->inFlightSemaphore;
    [impl_->currentCommandBuffer
        addCompletedHandler:^(id<MTLCommandBuffer>) {
            dispatch_semaphore_signal(inFlight);
        }];

    // 存储 drawable 以便 beginPass 挂 attachment、endFrame 时 present
    // (Impl 强引用成员,替代原先 CFRetain + associated object 的绕远存法)。
    // pass descriptor + encoder 的创建在 beginPass() 里逐 pass 执行。
    impl_->currentDrawable = drawable;
}

bool RenderDeviceMetal::beginPass(Framebuffer* target) {
    // beginFrame 跳帧(信号量超时 / drawable 为 nil)时本帧无 pass 可开。
    if (!impl_->currentCommandBuffer) return false;
    if (impl_->currentEncoder) {
        NSLog(@"beginPass: pass already open (nested passes unsupported)");
        return false;
    }

    MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
    double viewportW, viewportH;
    if (target) {
        auto* fbo = static_cast<MetalFramebuffer*>(target);
        passDesc.colorAttachments[0].texture = fbo->colorMtl();
        // 离屏 color 会被后续 pass 采样,必须 Store。
        passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
        passDesc.depthAttachment.texture = fbo->depthMtl();
        viewportW = fbo->width();
        viewportH = fbo->height();
    } else {
        if (!impl_->currentDrawable) return false;
        passDesc.colorAttachments[0].texture = impl_->currentDrawable.texture;
        passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
        passDesc.depthAttachment.texture = impl_->depthTexture;
        viewportW = impl_->viewportWidth;
        viewportH = impl_->viewportHeight;
    }
    passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
    // Clear color: sky color for this frame, pushed by Engine via setClearColor()
    // from FrameState before beginFrame(). The fullscreen atmosphere pass covers
    // this on the globe; it shows through at the horizon and empty sky.
    passDesc.colorAttachments[0].clearColor =
        MTLClearColorMake(impl_->clearR, impl_->clearG, impl_->clearB, impl_->clearA);
    if (passDesc.depthAttachment.texture) {
        passDesc.depthAttachment.loadAction = MTLLoadActionClear;
        passDesc.depthAttachment.clearDepth = 0.0;  // Reverse-Z: clear to 0 (farthest)
        passDesc.depthAttachment.storeAction = MTLStoreActionDontCare;
    }

    impl_->currentEncoder =
        [impl_->currentCommandBuffer renderCommandEncoderWithDescriptor:passDesc];

    // 设置视口
    [impl_->currentEncoder
        setViewport:(MTLViewport){0.0, 0.0, viewportW, viewportH, 0.0, 1.0}];

    // Front-face winding is Metal's default MTLWindingClockwise. This MUST stay
    // opposite to the GLES backend's GL_CCW. Both backends draw the same geometry
    // with the same (non-y-flipped) projection matrix; Metal's top-left / y-down
    // framebuffer origin reverses on-screen triangle winding relative to GL's
    // bottom-left / y-up origin, and the opposite front-face conventions cancel
    // that out so both backends cull the same geometric face. Do NOT "unify" the
    // two backends onto the same winding — that would invert culling on one side.
    [impl_->currentEncoder setFrontFacingWinding:MTLWindingClockwise];
    return true;
}

void RenderDeviceMetal::endPass() {
    if (impl_->currentEncoder) {
        [impl_->currentEncoder endEncoding];
        impl_->currentEncoder = nil;
    }
}

size_t RenderDeviceMetal::readFramebufferPixels(Framebuffer* source,
                                                int x,
                                                int y,
                                                int width,
                                                int height,
                                                uint8_t* outPixels,
                                                size_t outCapacity) {
    // 北极星 Phase 2b VT PoC feedback 回读:blit 私有 color 纹理 → shared MTLBuffer,
    // 提交独立 command buffer 并 **waitUntilCompleted**(故意同步 → 量最坏 stall,
    // 与 GLES 版语义一致)。生产实现应把 wait 换成 completion handler 延迟消费。
    // ⚠️ 通道序:离屏 color 是 BGRA8Unorm(见 createFramebuffer),读回字节是 BGRA;
    // decode 期望 RGBA。骨架先忠实拷字节;feedback shader 子步落地时,要么 shader
    // 写 swizzle 成 BGRA、要么按后端在 decode 前换 R/B(与 GLES RGBA 对齐)。
    if (!source || !outPixels || width <= 0 || height <= 0) {
        return 0;
    }
    const size_t needed =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (outCapacity < needed) {
        return 0;
    }
    auto* fbo = static_cast<MetalFramebuffer*>(source);
    id<MTLTexture> colorTex = fbo->colorMtl();
    if (!colorTex) {
        return 0;
    }
    const NSUInteger bytesPerRow = static_cast<NSUInteger>(width) * 4u;
    id<MTLBuffer> staging =
        [impl_->device newBufferWithLength:needed
                                  options:MTLResourceStorageModeShared];
    if (!staging) {
        return 0;
    }
    id<MTLCommandBuffer> cb = [impl_->commandQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:colorTex
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(x),
                                        static_cast<NSUInteger>(y), 0)
               sourceSize:MTLSizeMake(static_cast<NSUInteger>(width),
                                      static_cast<NSUInteger>(height), 1)
                 toBuffer:staging
        destinationOffset:0
   destinationBytesPerRow:bytesPerRow
 destinationBytesPerImage:needed];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    std::memcpy(outPixels, [staging contents], needed);
    return needed;
}

void RenderDeviceMetal::submit(const RenderCommandList& commands) {
    if (!impl_->currentEncoder) return;

    for (const auto& cmd : commands) {
        auto* program = static_cast<MetalShaderProgram*>(cmd.shader);
        auto* vb = static_cast<MetalBuffer*>(cmd.vertexBuffer);
        auto* ib = static_cast<MetalBuffer*>(cmd.indexBuffer);
        auto* instanceBuffer = static_cast<MetalBuffer*>(cmd.instanceBuffer);

        if (!program || !vb) continue;

        // alpha-to-coverage 命令(实例化 blend/透射):GLES 用真 A2C(MSAA 覆盖);
        // Metal 无多重采样帧缓冲,A2C 会是 no-op,故回落到 blended PSO——单 draw
        // 无序 alpha 混合(仍避免逐实例 draw 爆炸)。cmd.blend 已被抑制,这里补选。
        const bool useBlendedPso = cmd.blend || cmd.alphaToCoverage;
        [impl_->currentEncoder setRenderPipelineState:program->pso(useBlendedPso)];
        id<MTLDepthStencilState> depthState = impl_->depthDisabled;
        if (cmd.depthTest) {
            depthState = cmd.depthWrite ? impl_->depthReadWrite : impl_->depthReadOnly;
        }
        [impl_->currentEncoder setDepthStencilState:depthState];

        // 顶点 buffer
        [impl_->currentEncoder setVertexBuffer:vb->mtl() offset:0 atIndex:0];
        if (cmd.kind == RenderCommandKind::GltfPrimitiveInstanced &&
            instanceBuffer) {
            [impl_->currentEncoder setVertexBuffer:instanceBuffer->mtl()
                                            offset:0
                                           atIndex:7];
        }

        // Uniform data: set vertex/fragment bytes
        auto setUniform = [&](const std::string& name, int bufferIndex) {
            auto it = cmd.uniforms.find(name);
            if (it != cmd.uniforms.end()) {
                [impl_->currentEncoder setVertexBytes:it->second.data()
                                               length:it->second.size() * sizeof(float)
                                              atIndex:static_cast<NSUInteger>(bufferIndex)];
            }
        };

        if (cmd.kind == RenderCommandKind::SurfaceTile && cmd.hasSurfaceTileUniforms) {
            [impl_->currentEncoder setVertexBytes:cmd.surfaceModelViewProjection.data()
                                           length:cmd.surfaceModelViewProjection.size() * sizeof(float)
                                          atIndex:1];
            [impl_->currentEncoder setVertexBytes:cmd.surfaceTileUv.data()
                                           length:cmd.surfaceTileUv.size() * sizeof(float)
                                          atIndex:3];
            [impl_->currentEncoder setVertexBytes:cmd.surfaceCameraRelativeOrigin.data()
                                           length:cmd.surfaceCameraRelativeOrigin.size() * sizeof(float)
                                          atIndex:4];
        } else if (cmd.hasGltfUniforms) {
            // glTF/terrain 定长块：vertex 绑 MVP（buffer(1)）+ geomorph up/factor
            // （buffer(2)，terrain 顶点 morph 用；glTF 顶点 shader 不声明该参则忽略）。
            [impl_->currentEncoder
                setVertexBytes:cmd.gltfUniforms.modelViewProjection.data()
                        length:cmd.gltfUniforms.modelViewProjection.size() *
                               sizeof(float)
                       atIndex:1];
            [impl_->currentEncoder
                setVertexBytes:cmd.gltfUniforms.geomorphUpFactor.data()
                        length:cmd.gltfUniforms.geomorphUpFactor.size() *
                               sizeof(float)
                       atIndex:2];
        } else {
            setUniform("u_modelViewProjection", 1);
            setUniform("u_model", 2);
            setUniform("u_tileBounds", 2);
            setUniform("u_tileUV", 3);
            setUniform("u_cameraRelativeOrigin", 4);
        }

        // glTF/terrain 定长块：整块一次 setFragmentBytes 绑到 buffer(0)。
        // MSL 侧 GltfUniforms struct 与 GltfUniformBlock.h 逐字节镜像（布局
        // 契约见该头文件），取代原先 terrain 24 槽 / glTF 84 槽逐名绑定——
        // 后者把绑定表铺到 buffer(85)，超出 Metal 每 stage 31-buffer 硬上限，
        // 完整 glTF PBR PSO 因此建不出来。
        if (cmd.hasGltfUniforms) {
            [impl_->currentEncoder setFragmentBytes:&cmd.gltfUniforms
                                             length:sizeof(GltfUniformBlock)
                                            atIndex:0];
        } else {

        // Fragment uniforms
        auto fragIt = cmd.uniforms.find("u_lightDir");
        if (cmd.kind == RenderCommandKind::SurfaceTile && cmd.hasSurfaceTileUniforms) {
            [impl_->currentEncoder setFragmentBytes:cmd.surfaceLightDir.data()
                                             length:cmd.surfaceLightDir.size() * sizeof(float)
                                            atIndex:0];
        } else if (fragIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:fragIt->second.data()
                                             length:fragIt->second.size() * sizeof(float)
                                            atIndex:0];
        }
        auto useNormalIt = cmd.uniforms.find("u_useNormalMap");
        if (useNormalIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:useNormalIt->second.data()
                                             length:useNormalIt->second.size() * sizeof(float)
                                            atIndex:1];
        }
        auto debugNormalIt = cmd.uniforms.find("u_debugNormalMap");
        if (debugNormalIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:debugNormalIt->second.data()
                                             length:debugNormalIt->second.size() * sizeof(float)
                                            atIndex:2];
        }
        auto tileOpacityIt = cmd.uniforms.find("u_tileOpacity");
        if (cmd.kind == RenderCommandKind::SurfaceTile && cmd.hasSurfaceTileUniforms) {
            [impl_->currentEncoder setFragmentBytes:&cmd.surfaceTileOpacity
                                             length:sizeof(float)
                                            atIndex:3];
        } else if (tileOpacityIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:tileOpacityIt->second.data()
                                             length:tileOpacityIt->second.size() * sizeof(float)
                                            atIndex:3];
        }
        auto transitionOpacityIt = cmd.uniforms.find("u_transitionOpacity");
        if (cmd.kind == RenderCommandKind::SurfaceTile && cmd.hasSurfaceTileUniforms) {
            [impl_->currentEncoder setFragmentBytes:&cmd.surfaceTransitionOpacity
                                             length:sizeof(float)
                                            atIndex:4];
        } else if (transitionOpacityIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:transitionOpacityIt->second.data()
                                             length:transitionOpacityIt->second.size() * sizeof(float)
                                            atIndex:4];
        }
        auto baseColorIt = cmd.uniforms.find("u_baseColor");
        if (baseColorIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:baseColorIt->second.data()
                                             length:baseColorIt->second.size() * sizeof(float)
                                            atIndex:5];
        }
        auto renderOpacityIt = cmd.uniforms.find("u_renderOpacity");
        if (renderOpacityIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:renderOpacityIt->second.data()
                                             length:renderOpacityIt->second.size() * sizeof(float)
                                            atIndex:6];
        }
        auto hasBaseColorTextureIt =
            cmd.uniforms.find("u_hasBaseColorTexture");
        if (hasBaseColorTextureIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:hasBaseColorTextureIt->second.data()
                                             length:hasBaseColorTextureIt->second.size() * sizeof(float)
                                            atIndex:7];
        }
        auto alphaModeIt = cmd.uniforms.find("u_alphaMode");
        if (alphaModeIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:alphaModeIt->second.data()
                                             length:alphaModeIt->second.size() * sizeof(float)
                                            atIndex:8];
        }
        auto alphaCutoffIt = cmd.uniforms.find("u_alphaCutoff");
        if (alphaCutoffIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:alphaCutoffIt->second.data()
                                             length:alphaCutoffIt->second.size() * sizeof(float)
                                            atIndex:9];
        }
        auto setFragmentUniform = [&](const char* name, NSUInteger index) {
            auto it = cmd.uniforms.find(name);
            if (it != cmd.uniforms.end()) {
                [impl_->currentEncoder setFragmentBytes:it->second.data()
                                                 length:it->second.size() * sizeof(float)
                                                atIndex:index];
            }
        };
        setFragmentUniform("u_materialFactors", 10);
        setFragmentUniform("u_hasMaterialTextures", 11);
        setFragmentUniform("u_emissiveFactor", 12);
        setFragmentUniform("u_baseColorTexOffsetScale", 13);
        setFragmentUniform("u_baseColorTexRotationSinCos", 14);
        setFragmentUniform("u_metallicRoughnessTexOffsetScale", 15);
        setFragmentUniform("u_metallicRoughnessTexRotationSinCos", 16);
        setFragmentUniform("u_normalTexOffsetScale", 17);
        setFragmentUniform("u_normalTexRotationSinCos", 18);
        setFragmentUniform("u_occlusionTexOffsetScale", 19);
        setFragmentUniform("u_occlusionTexRotationSinCos", 20);
        setFragmentUniform("u_emissiveTexOffsetScale", 21);
        setFragmentUniform("u_emissiveTexRotationSinCos", 22);
        setFragmentUniform("u_textureCoordSets", 23);
        setFragmentUniform("u_emissiveTexCoordSet", 24);
        setFragmentUniform("u_unlit", 25);
        setFragmentUniform("u_dielectricSpecularF0", 26);
        setFragmentUniform("u_hasSpecularTextures", 27);
        setFragmentUniform("u_specularFactor", 28);
        setFragmentUniform("u_specularColorFactor", 29);
        setFragmentUniform("u_specularTexOffsetScale", 30);
        setFragmentUniform("u_specularTexRotationSinCos", 31);
        setFragmentUniform("u_specularColorTexOffsetScale", 32);
        setFragmentUniform("u_specularColorTexRotationSinCos", 33);
        setFragmentUniform("u_specularTexCoordSets", 34);
        setFragmentUniform("u_clearcoatFactors", 35);
        setFragmentUniform("u_hasClearcoatTextures", 36);
        setFragmentUniform("u_clearcoatTexOffsetScale", 37);
        setFragmentUniform("u_clearcoatTexRotationSinCos", 38);
        setFragmentUniform("u_clearcoatRoughnessTexOffsetScale", 39);
        setFragmentUniform("u_clearcoatRoughnessTexRotationSinCos", 40);
        setFragmentUniform("u_clearcoatNormalTexOffsetScale", 41);
        setFragmentUniform("u_clearcoatNormalTexRotationSinCos", 42);
        setFragmentUniform("u_clearcoatTexCoordSets", 43);
        setFragmentUniform("u_sheenColorFactor", 44);
        setFragmentUniform("u_sheenRoughnessFactor", 45);
        setFragmentUniform("u_hasSheenTextures", 46);
        setFragmentUniform("u_sheenColorTexOffsetScale", 47);
        setFragmentUniform("u_sheenColorTexRotationSinCos", 48);
        setFragmentUniform("u_sheenRoughnessTexOffsetScale", 49);
        setFragmentUniform("u_sheenRoughnessTexRotationSinCos", 50);
        setFragmentUniform("u_sheenTexCoordSets", 51);
        setFragmentUniform("u_anisotropyFactors", 52);
        setFragmentUniform("u_hasAnisotropyTexture", 53);
        setFragmentUniform("u_anisotropyTexOffsetScale", 54);
        setFragmentUniform("u_anisotropyTexRotationSinCos", 55);
        setFragmentUniform("u_anisotropyTexCoordSet", 56);
        setFragmentUniform("u_specularGlossinessWorkflow", 57);
        setFragmentUniform("u_specularGlossinessFactor", 58);
        setFragmentUniform("u_hasSpecularGlossinessTexture", 59);
        setFragmentUniform("u_specularGlossinessTexOffsetScale", 60);
        setFragmentUniform("u_specularGlossinessTexRotationSinCos", 61);
        setFragmentUniform("u_specularGlossinessTexCoordSet", 62);
        setFragmentUniform("u_transmissionFactor", 63);
        setFragmentUniform("u_hasTransmissionTexture", 64);
        setFragmentUniform("u_transmissionTexOffsetScale", 65);
        setFragmentUniform("u_transmissionTexRotationSinCos", 66);
        setFragmentUniform("u_transmissionTexCoordSet", 67);
        setFragmentUniform("u_mappedRasterTextureCount", 68);
        setFragmentUniform("u_mappedRasterTileUV0", 69);
        setFragmentUniform("u_mappedRasterTileUV1", 70);
        setFragmentUniform("u_mappedRasterTileUV2", 71);
        setFragmentUniform("u_mappedRasterTileUV3", 72);
        setFragmentUniform("u_mappedRasterOpacity0", 73);
        setFragmentUniform("u_mappedRasterOpacity1", 74);
        setFragmentUniform("u_mappedRasterOpacity2", 75);
        setFragmentUniform("u_mappedRasterOpacity3", 76);
        setFragmentUniform("u_mappedRasterTexCoordSet0", 77);
        setFragmentUniform("u_mappedRasterTexCoordSet1", 78);
        setFragmentUniform("u_mappedRasterTexCoordSet2", 79);
        setFragmentUniform("u_mappedRasterTexCoordSet3", 80);
        setFragmentUniform("u_gltfHasWaterMask", 81);
        setFragmentUniform("u_gltfWaterMaskTranslationScale", 82);
        setFragmentUniform("u_gltfWaterMaskState", 83);
        }  // end else (non-terrain fragment uniform table)

        // 纹理绑定 (shared: raster overlays at 15-18, water mask at 19; the
        // terrain shader also samples texture(0) for base color, and the
        // 合成方案页存储 sampler2DArray at slot 20)。纹理槽上限远高于 sampler
        // 上限(16);页存储复用共享 terrain sampler(0),不占新 sampler 槽。
        const NSUInteger maxMaterialTextures = kGltfPageStoreArrayTextureSlot + 1;
        const NSUInteger materialTextureCount =
            std::min<NSUInteger>(cmd.textures.size(), maxMaterialTextures);
        id<MTLSamplerState> sharedTileSampler = nil;
        // Metal 每 stage sampler 上限 16（索引 0-15）。terrain shader 只声明
        // sampler(0) 一个共享 sampler；glTF PBR shader 声明材质 sampler
        // 0-14 各自独立 + raster/water 瓦片纹理（texture 15-19）共享
        // sampler(15)——正好压进 16 个上限。
        const bool gltfSharedRasterSampler =
            !program->isTerrain() && cmd.hasGltfUniforms;
        for (NSUInteger textureIndex = 0;
             textureIndex < materialTextureCount;
             ++textureIndex) {
            if (!cmd.textures[textureIndex]) continue;
            auto* metalTex =
                static_cast<MetalTexture*>(cmd.textures[textureIndex]);
            [impl_->currentEncoder setFragmentTexture:metalTex->mtl()
                                              atIndex:textureIndex];
            if (program->isTerrain()) {
                if (!sharedTileSampler) {
                    sharedTileSampler = metalTex->sampler();
                }
            } else if (gltfSharedRasterSampler &&
                       textureIndex >=
                           static_cast<NSUInteger>(
                               kGltfRasterOverlayTextureBase)) {
                if (!sharedTileSampler) {
                    sharedTileSampler = metalTex->sampler();
                }
            } else {
                [impl_->currentEncoder
                    setFragmentSamplerState:metalTex->sampler()
                                    atIndex:textureIndex];
            }
        }
        if (program->isTerrain()) {
            // Always bind a sampler at slot 0 — the terrain fragment declares
            // u_terrainSampler[[sampler(0)]] unconditionally, so a tile with no
            // texture yet still needs one bound or the draw is invalid.
            [impl_->currentEncoder
                setFragmentSamplerState:(sharedTileSampler
                                             ? sharedTileSampler
                                             : impl_->linearClampSampler)
                                atIndex:0];
        } else if (gltfSharedRasterSampler) {
            // glTF shader 无条件声明 u_tileSharedSampler [[sampler(15)]]。
            [impl_->currentEncoder
                setFragmentSamplerState:(sharedTileSampler
                                             ? sharedTileSampler
                                             : impl_->linearClampSampler)
                                atIndex:15];
        }

        // 混合状态
        if (cmd.blend) {
            [impl_->currentEncoder setBlendColorRed:1.0 green:1.0 blue:1.0 alpha:1.0];
        }

        [impl_->currentEncoder setCullMode:cmd.cullFace ? MTLCullModeBack : MTLCullModeNone];

        // Draw
        MTLPrimitiveType primType = MTLPrimitiveTypeTriangle;
        switch (cmd.primitive) {
            case RenderCommand::PrimitiveType::Triangles: primType = MTLPrimitiveTypeTriangle; break;
            case RenderCommand::PrimitiveType::TriangleStrip: primType = MTLPrimitiveTypeTriangleStrip; break;
            case RenderCommand::PrimitiveType::Lines:     primType = MTLPrimitiveTypeLine; break;
            case RenderCommand::PrimitiveType::LineStrip: primType = MTLPrimitiveTypeLineStrip; break;
            case RenderCommand::PrimitiveType::Points:    primType = MTLPrimitiveTypePoint; break;
        }

        if (ib) {
            MTLIndexType idxType = (cmd.indexType == RenderCommand::IndexType::UInt32)
                                       ? MTLIndexTypeUInt32
                                       : MTLIndexTypeUInt16;
            NSUInteger indexSize = (cmd.indexType == RenderCommand::IndexType::UInt32) ? 4 : 2;
            if (cmd.instanceCount > 0) {
                [impl_->currentEncoder drawIndexedPrimitives:primType
                                                  indexCount:static_cast<NSUInteger>(cmd.indexCount)
                                                   indexType:idxType
                                                 indexBuffer:ib->mtl()
                                           indexBufferOffset:static_cast<NSUInteger>(cmd.indexOffset * static_cast<int>(indexSize))
                                               instanceCount:static_cast<NSUInteger>(cmd.instanceCount)];
            } else {
                [impl_->currentEncoder drawIndexedPrimitives:primType
                                                  indexCount:static_cast<NSUInteger>(cmd.indexCount)
                                                   indexType:idxType
                                                 indexBuffer:ib->mtl()
                                           indexBufferOffset:static_cast<NSUInteger>(cmd.indexOffset * static_cast<int>(indexSize))];
            }
        } else {
            if (cmd.instanceCount > 0) {
                [impl_->currentEncoder drawPrimitives:primType
                                           vertexStart:0
                                           vertexCount:static_cast<NSUInteger>(cmd.vertexCount)
                                         instanceCount:static_cast<NSUInteger>(cmd.instanceCount)];
            } else {
                [impl_->currentEncoder drawPrimitives:primType
                                           vertexStart:0
                                           vertexCount:static_cast<NSUInteger>(cmd.vertexCount)];
            }
        }
    }
}

void RenderDeviceMetal::endFrame() {
    if (impl_->currentEncoder) {
        [impl_->currentEncoder endEncoding];
        impl_->currentEncoder = nil;
    }

    if (impl_->currentCommandBuffer) {
        if (impl_->currentDrawable) {
            [impl_->currentCommandBuffer presentDrawable:impl_->currentDrawable];
        }
        // commit 后 completed handler 归还 in-flight 名额
        [impl_->currentCommandBuffer commit];
        impl_->currentCommandBuffer = nil;
        impl_->currentDrawable = nil;
    }
}

// ============================================================
// 生命周期
// ============================================================

void RenderDeviceMetal::onSurfaceCreated() {
    // CAMetalLayer 已通过构造函数设置
}

void RenderDeviceMetal::onSurfaceChanged(int width, int height) {
    fprintf(stderr, "[Metal] onSurfaceChanged: %dx%d\n", width, height);
    impl_->viewportWidth = width;
    impl_->viewportHeight = height;
    impl_->metalLayer.drawableSize = CGSizeMake(width, height);

    MTLTextureDescriptor* depthDesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                     width:static_cast<NSUInteger>(std::max(width, 1))
                                    height:static_cast<NSUInteger>(std::max(height, 1))
                                 mipmapped:NO];
    depthDesc.usage = MTLTextureUsageRenderTarget;
    depthDesc.storageMode = MTLStorageModePrivate;
    impl_->depthTexture = [impl_->device newTextureWithDescriptor:depthDesc];
}

void RenderDeviceMetal::onSurfaceDestroyed() {
    // 释放 Metal 资源。未提交的 command buffer 必须 commit 掉——它的
    // completed handler 持有 in-flight 信号量名额,直接丢弃会永久泄漏名额。
    if (impl_->currentEncoder) {
        [impl_->currentEncoder endEncoding];
        impl_->currentEncoder = nil;
    }
    if (impl_->currentCommandBuffer) {
        [impl_->currentCommandBuffer commit];
        impl_->currentCommandBuffer = nil;
    }
    impl_->currentDrawable = nil;
    impl_->depthTexture = nil;
}

} // namespace earth_engine
