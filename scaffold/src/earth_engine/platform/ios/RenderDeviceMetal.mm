#import "RenderDeviceMetal.h"

#include "../../renderer/RenderCommand.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreFoundation/CoreFoundation.h>
#import <objc/runtime.h>
#include <algorithm>
#include <string>
#include <stdexcept>
#include <cstring>

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
private:
    id<MTLBuffer> buf_;
};

class MetalShaderProgram : public ShaderProgram {
public:
    MetalShaderProgram(id<MTLRenderPipelineState> pso,
                       id<MTLDepthStencilState> depthState)
        : pso_(pso), depthState_(depthState) {}
    id<MTLRenderPipelineState> pso() const { return pso_; }
    id<MTLDepthStencilState> depthState() const { return depthState_; }
private:
    id<MTLRenderPipelineState> pso_;
    id<MTLDepthStencilState> depthState_;
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
    id<MTLTexture> depthTexture = nil;
    id<MTLSamplerState> linearClampSampler = nil;
    id<MTLDepthStencilState> depthReadWrite = nil;
    id<MTLDepthStencilState> depthReadOnly = nil;
    id<MTLDepthStencilState> depthDisabled = nil;
    int viewportWidth = 0;
    int viewportHeight = 0;
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

RenderDeviceMetal::RenderDeviceMetal(void* metalLayer)
    : impl_(new Impl()) {
    impl_->metalLayer = (__bridge CAMetalLayer*)metalLayer;
    impl_->device = impl_->metalLayer.device;
    impl_->commandQueue = [impl_->device newCommandQueue];
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
    MTLTextureDescriptor* texDesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pixelFormat
                                     width:static_cast<NSUInteger>(desc.width)
                                    height:static_cast<NSUInteger>(desc.height)
                                 mipmapped:desc.mipmap];

    id<MTLTexture> tex = [impl_->device newTextureWithDescriptor:texDesc];
    if (!tex) return nullptr;

    if (desc.data) {
        MTLRegion region = MTLRegionMake2D(0, 0,
                                           static_cast<NSUInteger>(desc.width),
                                           static_cast<NSUInteger>(desc.height));
        [tex replaceRegion:region
               mipmapLevel:0
                 withBytes:desc.data
               bytesPerRow:static_cast<NSUInteger>(
                   desc.width *
                   (desc.format == TextureDesc::Format::R8 ? 1 : 4))];
    }

    MTLSamplerDescriptor* samplerDesc = [MTLSamplerDescriptor new];
    samplerDesc.minFilter = toMetalFilter(desc.minFilter);
    samplerDesc.magFilter = toMetalFilter(desc.magFilter);
    samplerDesc.mipFilter = desc.mipmap
        ? MTLSamplerMipFilterLinear
        : MTLSamplerMipFilterNotMipmapped;
    samplerDesc.maxAnisotropy =
        static_cast<NSUInteger>(std::max(1.0f, desc.maxAnisotropy));
    samplerDesc.sAddressMode = toMetalAddressMode(desc.wrapS);
    samplerDesc.tAddressMode = toMetalAddressMode(desc.wrapT);
    id<MTLSamplerState> sampler =
        [impl_->device newSamplerStateWithDescriptor:samplerDesc];

    return std::make_unique<MetalTexture>(tex, sampler ?: impl_->linearClampSampler);
}

bool RenderDeviceMetal::updateTextureRegion(Texture* texture,
                                            int x,
                                            int y,
                                            int width,
                                            int height,
                                            const uint8_t* data,
                                            size_t rowBytes) {
    auto* metalTexture = static_cast<MetalTexture*>(texture);
    if (!metalTexture || !data || width <= 0 || height <= 0) {
        return false;
    }
    if (x < 0 || y < 0 ||
        x + width > metalTexture->width() ||
        y + height > metalTexture->height()) {
        return false;
    }
    MTLRegion region = MTLRegionMake2D(static_cast<NSUInteger>(x),
                                       static_cast<NSUInteger>(y),
                                       static_cast<NSUInteger>(width),
                                       static_cast<NSUInteger>(height));
    [metalTexture->mtl() replaceRegion:region
                           mipmapLevel:0
                             withBytes:data
                           bytesPerRow:static_cast<NSUInteger>(rowBytes)];
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
    std::memcpy(static_cast<uint8_t*>(metalBuffer->mtl().contents) + offset,
                data,
                size);
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
        Tile,
        Gltf,
        GltfInstanced,
        Color,
        DebugLine
    };
    PipelineLayout layout = PipelineLayout::Surface;

    // 尝试识别 shader 类型（globe / tile / vector color / debug line）
    id<MTLFunction> vertexFunc = [library newFunctionWithName:@"globeVertex"];
    id<MTLFunction> fragmentFunc = [library newFunctionWithName:@"globeFragment"];

    if (!vertexFunc || !fragmentFunc) {
        fprintf(stderr, "[createShader] trying tileVertex / tileFragment\n");
        vertexFunc = [library newFunctionWithName:@"tileVertex"];
        fragmentFunc = [library newFunctionWithName:@"tileFragment"];
        if (vertexFunc && fragmentFunc) layout = PipelineLayout::Tile;
        fprintf(stderr, "[createShader] tileVertex=%p tileFragment=%p\n", (void*)vertexFunc, (void*)fragmentFunc);
    }
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

    // Vertex descriptor: Globe and SurfaceTile share position/normal/uv layout.
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
    pipeDesc.colorAttachments[0].blendingEnabled = YES;
    pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pipeDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    pipeDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

    id<MTLRenderPipelineState> pso =
        [impl_->device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
    if (!pso) {
        if (error) {
            NSLog(@"Metal pipeline error: %@", error.localizedDescription);
        }
        return nullptr;
    }

    return std::make_unique<MetalShaderProgram>(pso, impl_->depthReadWrite);
}

std::unique_ptr<Framebuffer> RenderDeviceMetal::createFramebuffer(const FramebufferDesc& /*desc*/) {
    return nullptr;  // MVP 不需要
}

// ============================================================
// 帧操作
// ============================================================

void RenderDeviceMetal::beginFrame() {
    impl_->currentCommandBuffer = [impl_->commandQueue commandBuffer];

    // 获取 current drawable
    id<CAMetalDrawable> drawable = [impl_->metalLayer nextDrawable];
    if (!drawable) {
        fprintf(stderr, "[Metal] beginFrame: nextDrawable returned NIL!\n");
        fprintf(stderr, "[Metal] metalLayer=%p viewport=%dx%d\n",
            (__bridge void*)impl_->metalLayer,
            impl_->viewportWidth, impl_->viewportHeight);
        impl_->currentCommandBuffer = nil;
        return;
    }

    MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
    passDesc.colorAttachments[0].texture = drawable.texture;
    passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
    // Clear color: sky-horizon blue (fullscreen atmosphere pass covers this).
    // TODO: pass frameState.clearR/G/B from Engine after beginFrame() reorder.
    passDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.3, 0.6, 1.0);
    passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
    passDesc.depthAttachment.texture = impl_->depthTexture;
    passDesc.depthAttachment.loadAction = MTLLoadActionClear;
    passDesc.depthAttachment.clearDepth = 0.0;  // Reverse-Z: clear to 0 (farthest)
    passDesc.depthAttachment.storeAction = MTLStoreActionDontCare;

    impl_->currentEncoder =
        [impl_->currentCommandBuffer renderCommandEncoderWithDescriptor:passDesc];

    // 设置视口
    [impl_->currentEncoder
        setViewport:(MTLViewport){0.0, 0.0,
                                  static_cast<double>(impl_->viewportWidth),
                                  static_cast<double>(impl_->viewportHeight),
                                  0.0, 1.0}];

    // 存储 drawable 以便 endFrame 时 present
    [impl_->currentCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        // drawable retained by command buffer
    }];
    // 强引用 drawable 避免提前释放
    CFRetain((__bridge CFTypeRef)drawable);
    // 通过 associated object 存到 command buffer
    objc_setAssociatedObject(impl_->currentCommandBuffer,
                            "drawable",
                            (__bridge id)drawable,
                            OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

void RenderDeviceMetal::submit(const RenderCommandList& commands) {
    if (!impl_->currentEncoder) return;

    for (const auto& cmd : commands) {
        auto* program = static_cast<MetalShaderProgram*>(cmd.shader);
        auto* vb = static_cast<MetalBuffer*>(cmd.vertexBuffer);
        auto* ib = static_cast<MetalBuffer*>(cmd.indexBuffer);
        auto* instanceBuffer = static_cast<MetalBuffer*>(cmd.instanceBuffer);

        if (!program || !vb) continue;

        [impl_->currentEncoder setRenderPipelineState:program->pso()];
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
        } else {
            setUniform("u_modelViewProjection", 1);
            setUniform("u_model", 2);
            setUniform("u_tileBounds", 2);
            setUniform("u_tileUV", 3);
            setUniform("u_cameraRelativeOrigin", 4);
        }

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

        // 纹理绑定
        const NSUInteger maxMaterialTextures = 14;
        const NSUInteger materialTextureCount =
            std::min<NSUInteger>(cmd.textures.size(), maxMaterialTextures);
        for (NSUInteger textureIndex = 0;
             textureIndex < materialTextureCount;
             ++textureIndex) {
            if (!cmd.textures[textureIndex]) continue;
            auto* metalTex =
                static_cast<MetalTexture*>(cmd.textures[textureIndex]);
            [impl_->currentEncoder setFragmentTexture:metalTex->mtl()
                                              atIndex:textureIndex];
            [impl_->currentEncoder setFragmentSamplerState:metalTex->sampler()
                                                   atIndex:textureIndex];
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
        // 获取并释放 drawable
        id<CAMetalDrawable> drawable = objc_getAssociatedObject(impl_->currentCommandBuffer, "drawable");
        if (drawable) {
            [impl_->currentCommandBuffer presentDrawable:drawable];
            CFRelease((__bridge CFTypeRef)drawable);
        }
        [impl_->currentCommandBuffer commit];
        impl_->currentCommandBuffer = nil;
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
    // 释放 Metal 资源
    impl_->currentEncoder = nil;
    impl_->currentCommandBuffer = nil;
    impl_->depthTexture = nil;
}

} // namespace earth_engine
