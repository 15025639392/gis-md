#import "RenderDeviceMetal.h"

#include "../../renderer/RenderCommand.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreFoundation/CoreFoundation.h>
#import <objc/runtime.h>
#include <string>
#include <stdexcept>
#include <cstring>

namespace earth_engine {

// ============================================================
// Internal Metal resource wrappers
// ============================================================

class MetalTexture : public Texture {
public:
    MetalTexture(id<MTLTexture> tex) : tex_(tex) {}
    int width() const override { return static_cast<int>(tex_.width); }
    int height() const override { return static_cast<int>(tex_.height); }
    id<MTLTexture> mtl() const { return tex_; }
private:
    id<MTLTexture> tex_;
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
    desc.depthCompareFunction = enabled ? MTLCompareFunctionLessEqual : MTLCompareFunctionAlways;
    desc.depthWriteEnabled = enabled && write;
    return [device newDepthStencilStateWithDescriptor:desc];
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
    MTLTextureDescriptor* texDesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
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
               bytesPerRow:static_cast<NSUInteger>(desc.width * 4)];
    }

    return std::make_unique<MetalTexture>(tex);
}

std::unique_ptr<Buffer> RenderDeviceMetal::createBuffer(const BufferDesc& desc) {
    MTLResourceOptions options = MTLResourceStorageModeShared;
    id<MTLBuffer> buf = [impl_->device newBufferWithBytes:desc.data
                                                    length:desc.size
                                                   options:options];
    return buf ? std::make_unique<MetalBuffer>(buf) : nullptr;
}

std::unique_ptr<ShaderProgram> RenderDeviceMetal::createShader(const ShaderDesc& desc) {
    NSError* error = nil;

    // 编译 MSL 源码
    NSString* source = [NSString stringWithUTF8String:desc.vertexSource.c_str()];
    // 实际上需要 vertex + fragment；这里把两者拼接或用 separate libraries
    // 简便做法：把 vertex 和 fragment 源码拼接（MSL 允许多个函数在同一 library）
    NSString* combinedSource = [NSString stringWithFormat:@"%s\n%s",
                                desc.vertexSource.c_str(),
                                desc.fragmentSource.c_str()];

    id<MTLLibrary> library = [impl_->device newLibraryWithSource:combinedSource
                                                         options:nil
                                                           error:&error];
    if (!library) {
        if (error) {
            NSLog(@"Metal shader compile error: %@", error.localizedDescription);
        }
        return nullptr;
    }

    // 尝试识别 shader 类型（globe vs tile）
    id<MTLFunction> vertexFunc = [library newFunctionWithName:@"globeVertex"];
    id<MTLFunction> fragmentFunc = [library newFunctionWithName:@"globeFragment"];

    if (!vertexFunc || !fragmentFunc) {
        vertexFunc = [library newFunctionWithName:@"tileVertex"];
        fragmentFunc = [library newFunctionWithName:@"tileFragment"];
    }

    if (!vertexFunc || !fragmentFunc) {
        return nullptr;
    }

    // Vertex descriptor: Globe and SurfaceTile share position/normal/uv layout.
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
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
        impl_->currentCommandBuffer = nil;
        return;
    }

    MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
    passDesc.colorAttachments[0].texture = drawable.texture;
    passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
    passDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.1, 1.0);
    passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
    passDesc.depthAttachment.texture = impl_->depthTexture;
    passDesc.depthAttachment.loadAction = MTLLoadActionClear;
    passDesc.depthAttachment.clearDepth = 1.0;
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

        if (!program || !vb) continue;

        [impl_->currentEncoder setRenderPipelineState:program->pso()];
        id<MTLDepthStencilState> depthState = impl_->depthDisabled;
        if (cmd.depthTest) {
            depthState = cmd.depthWrite ? impl_->depthReadWrite : impl_->depthReadOnly;
        }
        [impl_->currentEncoder setDepthStencilState:depthState];

        // 顶点 buffer
        [impl_->currentEncoder setVertexBuffer:vb->mtl() offset:0 atIndex:0];

        // Uniform data: set vertex/fragment bytes
        auto setUniform = [&](const std::string& name, int bufferIndex) {
            auto it = cmd.uniforms.find(name);
            if (it != cmd.uniforms.end()) {
                [impl_->currentEncoder setVertexBytes:it->second.data()
                                               length:it->second.size() * sizeof(float)
                                              atIndex:static_cast<NSUInteger>(bufferIndex)];
            }
        };

        setUniform("u_modelViewProjection", 1);
        setUniform("u_model", 2);
        setUniform("u_tileBounds", 2);
        setUniform("u_tileUV", 3);
        setUniform("u_cameraRelativeOrigin", 4);

        // Fragment uniforms
        auto fragIt = cmd.uniforms.find("u_lightDir");
        if (fragIt != cmd.uniforms.end()) {
            [impl_->currentEncoder setFragmentBytes:fragIt->second.data()
                                             length:fragIt->second.size() * sizeof(float)
                                            atIndex:0];
        }

        // 纹理绑定
        if (!cmd.textures.empty() && cmd.textures[0]) {
            auto* metalTex = static_cast<MetalTexture*>(cmd.textures[0]);
            [impl_->currentEncoder setFragmentTexture:metalTex->mtl() atIndex:0];
            [impl_->currentEncoder setFragmentSamplerState:impl_->linearClampSampler atIndex:0];
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
            case RenderCommand::PrimitiveType::Lines:     primType = MTLPrimitiveTypeLine; break;
            case RenderCommand::PrimitiveType::LineStrip: primType = MTLPrimitiveTypeLineStrip; break;
            case RenderCommand::PrimitiveType::Points:    primType = MTLPrimitiveTypePoint; break;
        }

        if (ib) {
            MTLIndexType idxType = (cmd.indexType == RenderCommand::IndexType::UInt32)
                                       ? MTLIndexTypeUInt32
                                       : MTLIndexTypeUInt16;
            NSUInteger indexSize = (cmd.indexType == RenderCommand::IndexType::UInt32) ? 4 : 2;
            [impl_->currentEncoder drawIndexedPrimitives:primType
                                              indexCount:static_cast<NSUInteger>(cmd.indexCount)
                                               indexType:idxType
                                             indexBuffer:ib->mtl()
                                       indexBufferOffset:static_cast<NSUInteger>(cmd.indexOffset * static_cast<int>(indexSize))];
        } else {
            [impl_->currentEncoder drawPrimitives:primType
                                       vertexStart:0
                                       vertexCount:static_cast<NSUInteger>(cmd.vertexCount)];
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
