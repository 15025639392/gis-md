#include "RenderDeviceGLES.h"
#include "../../renderer/RenderCommand.h"
#include "../../debug/PerfTimer.h"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <array>
#include <string>

namespace earth_engine {
namespace {

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

// glGetString(GL_EXTENSIONS)/glGetFloatv 是驱动线程同步点且每张瓦片纹理都会
// 触发，结果对同一 GPU 恒定 —— 首次查询后缓存（surface 重建不换 GPU）。
bool supportsTextureAnisotropy() {
    static const bool supported = [] {
        const char* extensions =
            reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        return extensions &&
               std::strstr(extensions, "GL_EXT_texture_filter_anisotropic") !=
                   nullptr;
    }();
    return supported;
}

GLfloat deviceMaxTextureAnisotropy() {
    static const GLfloat maxAnisotropy = [] {
        GLfloat value = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &value);
        return value;
    }();
    return maxAnisotropy;
}

// GLES fragment shaders are limited to GL_MAX_TEXTURE_IMAGE_UNITS texture units
// (spec floor 16; Adreno enforces exactly 16). The backend-shared textures
// vector (see RenderCommand.h) places advanced PBR-extension textures at indices
// 5-14, raster overlays at kGltfRasterOverlayTextureBase..(+3) and the water
// mask at kGltfWaterMaskTextureSlot (15-19). The GLES glTF fragment shader
// aliases the extension samplers to the base-color sampler, so those 10 slots
// carry no live sampler and the raster/water textures can be compacted into the
// freed 5-9 range to stay within 16 units. Metal keeps the full 0-19 layout.
constexpr int kGltfExtensionSamplerSlots = kGltfRasterOverlayTextureBase - 5;  // 10
constexpr int kGlesGltfRasterUnitBase =
    kGltfRasterOverlayTextureBase - kGltfExtensionSamplerSlots;  // 5
constexpr int kGlesGltfWaterUnit =
    kGltfWaterMaskTextureSlot - kGltfExtensionSamplerSlots;      // 9

// Maps a shared textures-vector index to the compacted GLES fragment texture
// unit for glTF / terrain commands. Returns -1 for the aliased extension slots
// (5-14), which carry no live sampler on GLES and must not be bound.
int glesGltfTextureUnit(size_t vecIndex) {
    if (vecIndex <= 4) return static_cast<int>(vecIndex);
    if (vecIndex >= static_cast<size_t>(kGltfRasterOverlayTextureBase)) {
        return static_cast<int>(vecIndex) - kGltfExtensionSamplerSlots;
    }
    return -1;
}

// VAO 缓存上限：working set 约为可见瓦片数（几十~一两百条），512 足够
// 宽裕；触顶时逐出最久未用的一条，防止长时间运行 VAO 无限增长。
constexpr size_t kMaxVaoCacheEntries = 512;

} // namespace

// ============================================================
// GLVaoInvalidationRegistry
// ============================================================

void GLVaoInvalidationRegistry::notifyBufferDeleted(unsigned int bufferId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deviceAlive_) {
        pendingDeletedBuffers_.push_back(bufferId);
    }
}

void GLVaoInvalidationRegistry::markDeviceDead() {
    std::lock_guard<std::mutex> lock(mutex_);
    deviceAlive_ = false;
    pendingDeletedBuffers_.clear();
}

std::vector<unsigned int> GLVaoInvalidationRegistry::takePendingDeletedBuffers() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<unsigned int> taken;
    taken.swap(pendingDeletedBuffers_);
    return taken;
}

// ============================================================
// GLTexture
// ============================================================

GLTexture::GLTexture(unsigned int id,
                     int width,
                     int height,
                     size_t sizeBytes)
    : id_(id),
      width_(width),
      height_(height),
      sizeBytes_(sizeBytes) {}

GLTexture::~GLTexture() {
    if (id_) {
        glDeleteTextures(1, &id_);
    }
}

// ============================================================
// GLBuffer
// ============================================================

GLBuffer::GLBuffer(unsigned int id, size_t size, unsigned int target,
                   std::shared_ptr<GLVaoInvalidationRegistry> vaoRegistry)
    : id_(id), size_(size), target_(target),
      vaoRegistry_(std::move(vaoRegistry)) {}

GLBuffer::~GLBuffer() {
    if (id_) {
        // 先登记失效：device 下一次 submit 开头会清除引用本 buffer 的 VAO
        // （VAO 持有 buffer 引用，GL 名字可被复用，不清除则悬空）。
        if (vaoRegistry_) {
            vaoRegistry_->notifyBufferDeleted(id_);
        }
        glDeleteBuffers(1, &id_);
    }
}

// ============================================================
// GLShaderProgram
// ============================================================

GLShaderProgram::GLShaderProgram(unsigned int programId)
    : id_(programId) {}

GLShaderProgram::~GLShaderProgram() {
    if (id_) {
        glDeleteProgram(id_);
    }
}

int GLShaderProgram::uniformLocation(const std::string& name) {
    auto it = uniformCache_.find(name);
    if (it != uniformCache_.end()) {
        return it->second;
    }
    int loc = glGetUniformLocation(id_, name.c_str());
    uniformCache_[name] = loc;
    return loc;
}

const std::vector<int>& GLShaderProgram::gltfBlockLocations() {
    if (!gltfBlockLocationsResolved_) {
        const auto& table = gltfUniformTable();
        gltfBlockLocations_.resize(table.size());
        for (size_t i = 0; i < table.size(); ++i) {
            gltfBlockLocations_[i] = glGetUniformLocation(id_, table[i].name);
        }
        gltfBlockLocationsResolved_ = true;
    }
    return gltfBlockLocations_;
}

// ============================================================
// RenderDeviceGLES
// ============================================================

RenderDeviceGLES::RenderDeviceGLES()
    : vaoRegistry_(std::make_shared<GLVaoInvalidationRegistry>()) {}

RenderDeviceGLES::~RenderDeviceGLES() {
    // 先声明设备死亡：此后 GLBuffer 析构不再登记。登记表由 shared_ptr
    // 保活，晚于 device 析构的 buffer 也不会访问已释放内存。
    vaoRegistry_->markDeviceDead();
    onSurfaceDestroyed();
}

int RenderDeviceGLES::maxTextureSize() const {
    GLint size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);
    return size;
}

int RenderDeviceGLES::maxDrawBuffers() const {
    GLint count = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &count);
    return count;
}

bool RenderDeviceGLES::supportsFloatTextures() const {
    // GLES 3.0 要求支持 GL_OES_texture_float（通过扩展或核心）
    // 简化：检查 GL_EXT_color_buffer_float
    return true;  // GLES 3.0+ 基本都支持
}

bool RenderDeviceGLES::supportsInstancing() const {
    return true;  // GLES 3.0 核心特性
}

std::string RenderDeviceGLES::rendererString() const {
    const char* s = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    return s ? std::string(s) : "Unknown GLES";
}

// ============================================================
// 资源创建
// ============================================================

std::unique_ptr<Texture> RenderDeviceGLES::createTexture(const TextureDesc& desc) {
    GLuint id = 0;
    glGenTextures(1, &id);
    if (!id) return nullptr;

    glBindTexture(GL_TEXTURE_2D, id);

    // 格式映射
    GLenum internalFormat = GL_RGBA8;
    GLenum format = GL_RGBA;
    switch (desc.format) {
        case TextureDesc::Format::RGBA8:
            internalFormat = GL_RGBA8;
            format = GL_RGBA;
            break;
        case TextureDesc::Format::RGB8:
            internalFormat = GL_RGB8;
            format = GL_RGB;
            break;
        case TextureDesc::Format::R8:
            internalFormat = GL_R8;
            format = GL_RED;
            break;
        case TextureDesc::Format::Depth32F:
            internalFormat = GL_DEPTH_COMPONENT32F;
            format = GL_DEPTH_COMPONENT;
            break;
    }

    GLenum type = GL_UNSIGNED_BYTE;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat,
                 desc.width, desc.height, 0,
                 format, type, desc.data);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    desc.minFilter == TextureDesc::Filter::Linear
                        ? (desc.mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR)
                        : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    desc.magFilter == TextureDesc::Filter::Linear ? GL_LINEAR : GL_NEAREST);

    if (desc.maxAnisotropy > 1.0f && supportsTextureAnisotropy()) {
        const GLfloat anisotropy = std::clamp(
            static_cast<GLfloat>(desc.maxAnisotropy),
            1.0f,
            deviceMaxTextureAnisotropy());
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);
    }

    auto toGlWrap = [](TextureDesc::Wrap wrap) {
        switch (wrap) {
            case TextureDesc::Wrap::Repeat:
                return GL_REPEAT;
            case TextureDesc::Wrap::MirroredRepeat:
                return GL_MIRRORED_REPEAT;
            case TextureDesc::Wrap::Clamp:
            default:
                return GL_CLAMP_TO_EDGE;
        }
    };
    GLint wrapS = toGlWrap(desc.wrapS);
    GLint wrapT = toGlWrap(desc.wrapT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);

    const bool allocatedMipChain = desc.mipmap && desc.data;
    if (allocatedMipChain) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    const size_t bytesPerPixel =
        desc.format == TextureDesc::Format::R8
            ? 1u
            : (desc.format == TextureDesc::Format::RGB8 ? 3u : 4u);
    size_t allocatedBytes = 0;
    int levelWidth = std::max(1, desc.width);
    int levelHeight = std::max(1, desc.height);
    do {
        allocatedBytes +=
            static_cast<size_t>(levelWidth) *
            static_cast<size_t>(levelHeight) *
            bytesPerPixel;
        if (!allocatedMipChain ||
            (levelWidth == 1 && levelHeight == 1)) {
            break;
        }
        levelWidth = std::max(1, levelWidth / 2);
        levelHeight = std::max(1, levelHeight / 2);
    } while (true);
    return std::make_unique<GLTexture>(
        id,
        desc.width,
        desc.height,
        allocatedBytes);
}

bool RenderDeviceGLES::updateTextureRegion(Texture* texture,
                                           int x,
                                           int y,
                                           int width,
                                           int height,
                                           const uint8_t* data,
                                           size_t rowBytes) {
    auto* glTexture = static_cast<GLTexture*>(texture);
    if (!glTexture || !data || width <= 0 || height <= 0) {
        return false;
    }
    if (x < 0 || y < 0 ||
        x + width > glTexture->width() ||
        y + height > glTexture->height()) {
        return false;
    }
    if (rowBytes != static_cast<size_t>(width) * 4u) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, glTexture->glId());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    x,
                    y,
                    width,
                    height,
                    GL_RGBA,
                    GL_UNSIGNED_BYTE,
                    data);
    glBindTexture(GL_TEXTURE_2D, 0);
    // glGetError 在 threaded-GL 驱动下是同步点，热路径仅 debug 保留。
#ifndef NDEBUG
    return glGetError() == GL_NO_ERROR;
#else
    return true;
#endif
}

std::unique_ptr<Buffer> RenderDeviceGLES::createBuffer(const BufferDesc& desc) {
    GLuint id = 0;
    glGenBuffers(1, &id);
    if (!id) return nullptr;

    GLenum target = (desc.type == BufferDesc::Type::Index)
                        ? GL_ELEMENT_ARRAY_BUFFER
                        : GL_ARRAY_BUFFER;

    glBindBuffer(target, id);
    GLenum usage = (desc.usage == BufferDesc::Usage::Dynamic)
                       ? GL_DYNAMIC_DRAW
                       : GL_STATIC_DRAW;
    glBufferData(target, static_cast<GLsizeiptr>(desc.size), desc.data, usage);
    glBindBuffer(target, 0);

    return std::make_unique<GLBuffer>(id, desc.size, target, vaoRegistry_);
}

bool RenderDeviceGLES::updateBuffer(Buffer* buffer,
                                    size_t offset,
                                    const void* data,
                                    size_t size) {
    auto* glBuffer = static_cast<GLBuffer*>(buffer);
    if (!glBuffer || !data || size == 0 || offset + size > glBuffer->size()) {
        return false;
    }

    glBindBuffer(glBuffer->target(), glBuffer->glId());
    glBufferSubData(glBuffer->target(),
                    static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(size),
                    data);
    glBindBuffer(glBuffer->target(), 0);
#ifndef NDEBUG
    return glGetError() == GL_NO_ERROR;
#else
    return true;
#endif
}

std::unique_ptr<ShaderProgram> RenderDeviceGLES::createShader(const ShaderDesc& desc) {
    // 编译 vertex shader
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    const char* vsSrc = desc.vertexSource.c_str();
    glShaderSource(vs, 1, &vsSrc, nullptr);
    glCompileShader(vs);

    GLint compiled = GL_FALSE;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint logLen = 0;
        glGetShaderiv(vs, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 1) {
            std::string log(logLen, '\0');
            glGetShaderInfoLog(vs, logLen, nullptr, log.data());
            __android_log_print(ANDROID_LOG_ERROR, "GLES",
                "Vertex shader compile error: %s", log.c_str());
        }
        __android_log_print(ANDROID_LOG_ERROR, "GLES",
            "Vertex shader source (first 200 chars): %.200s", vsSrc);
        glDeleteShader(vs);
        return nullptr;
    }

    // 编译 fragment shader
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char* fsSrc = desc.fragmentSource.c_str();
    glShaderSource(fs, 1, &fsSrc, nullptr);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint logLen = 0;
        glGetShaderiv(fs, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 1) {
            std::string log(logLen, '\0');
            glGetShaderInfoLog(fs, logLen, nullptr, log.data());
            __android_log_print(ANDROID_LOG_ERROR, "GLES",
                "Fragment shader compile error: %s", log.c_str());
        }
        __android_log_print(ANDROID_LOG_ERROR, "GLES",
            "Fragment shader source (first 200 chars): %.200s", fsSrc);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return nullptr;
    }

    // 链接 program
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint logLen = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 1) {
            std::string log(logLen, '\0');
            glGetProgramInfoLog(program, logLen, nullptr, log.data());
            __android_log_print(ANDROID_LOG_ERROR, "GLES",
                "Program link error: %s", log.c_str());
        }
    }

    // 链接后可释放 shader
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!linked) {
        glDeleteProgram(program);
        return nullptr;
    }

    return std::make_unique<GLShaderProgram>(program);
}

// ============================================================
// GLFramebuffer
// ============================================================

GLFramebuffer::GLFramebuffer(unsigned int fboId,
                             std::unique_ptr<GLTexture> color,
                             unsigned int depthRenderbufferId,
                             std::unique_ptr<GLTexture> depthTexture,
                             int width,
                             int height)
    : fboId_(fboId),
      color_(std::move(color)),
      depthRenderbufferId_(depthRenderbufferId),
      depthTexture_(std::move(depthTexture)),
      width_(width),
      height_(height) {}

GLFramebuffer::~GLFramebuffer() {
    if (fboId_) {
        glDeleteFramebuffers(1, &fboId_);
    }
    if (depthRenderbufferId_) {
        glDeleteRenderbuffers(1, &depthRenderbufferId_);
    }
    // color_ / depthTexture_ 的 GLTexture 析构自删纹理。
}

std::unique_ptr<Framebuffer> RenderDeviceGLES::createFramebuffer(
    const FramebufferDesc& desc) {
    if (desc.width <= 0 || desc.height <= 0 || !desc.hasColor) {
        __android_log_print(ANDROID_LOG_ERROR, "GLES",
            "createFramebuffer: invalid desc %dx%d hasColor=%d",
            desc.width, desc.height, desc.hasColor ? 1 : 0);
        return nullptr;
    }
    if (desc.samples > 1) {
        // v1 不支持 MSAA(resolve 未实现),按单采样处理而不是假装支持。
        __android_log_print(ANDROID_LOG_WARN, "GLES",
            "createFramebuffer: samples=%d unsupported, using 1", desc.samples);
    }

    GLuint colorTex = 0;
    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, desc.width, desc.height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    const size_t attachmentBytes =
        static_cast<size_t>(desc.width) *
        static_cast<size_t>(desc.height) * 4u;
    auto color = std::make_unique<GLTexture>(
        colorTex,
        desc.width,
        desc.height,
        attachmentBytes);

    // depth:renderbuffer(默认,不可采样)或纹理(depthSampleable)。两者
    // 都用 32F 匹配主 pass reverse-Z 精度。深度纹理必须 NEAREST 过滤
    // (深度值不可线性插值)。
    GLuint depthRb = 0;
    std::unique_ptr<GLTexture> depthTex;
    if (desc.hasDepth && desc.depthSampleable) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F,
                       desc.width, desc.height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        depthTex = std::make_unique<GLTexture>(
            tex,
            desc.width,
            desc.height,
            attachmentBytes);
    } else if (desc.hasDepth) {
        glGenRenderbuffers(1, &depthRb);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F,
                              desc.width, desc.height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, colorTex, 0);
    if (depthTex) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, depthTex->glId(), 0);
    } else if (depthRb) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, depthRb);
    }
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        __android_log_print(ANDROID_LOG_ERROR, "GLES",
            "createFramebuffer: incomplete (status=0x%x) %dx%d",
            status, desc.width, desc.height);
        glDeleteFramebuffers(1, &fbo);
        if (depthRb) {
            glDeleteRenderbuffers(1, &depthRb);
        }
        return nullptr;  // color/depth 纹理由 GLTexture 析构回收
    }
    return std::make_unique<GLFramebuffer>(
        fbo, std::move(color), depthRb, std::move(depthTex),
        desc.width, desc.height);
}

// ============================================================
// 帧操作
// ============================================================

void RenderDeviceGLES::setClearColor(float r, float g, float b, float a) {
    clearR_ = r;
    clearG_ = g;
    clearB_ = b;
    clearA_ = a;
}

void RenderDeviceGLES::beginFrame() {
    // 帧获取阶段无事可做(EGL context/surface 由外部管理);pass 的
    // clear + 状态设置在 beginPass() 里逐 pass 执行。
}

bool RenderDeviceGLES::beginPass(Framebuffer* target) {
    if (target) {
        auto* fbo = static_cast<GLFramebuffer*>(target);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo->glId());
        glViewport(0, 0, fbo->width(), fbo->height());
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, viewportWidth_, viewportHeight_);
    }
    // Restore pass-global state before clear. Previous overlay/background
    // commands may leave depth writes or blending disabled/enabled; stale
    // depth makes the next frame's surface tiles appear perforated.
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    // Clear color: sky color for this frame, pushed by Engine via setClearColor()
    // from FrameState before beginFrame(). The fullscreen atmosphere pass covers
    // this on the globe; it shows through at the horizon and empty sky.
    glClearColor(clearR_, clearG_, clearB_, clearA_);
    glClearDepthf(0.0f);   // Reverse-Z: clear to 0 (farthest)
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL); // Reverse-Z: greater depth = closer
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    // Front-face winding is GL's default GL_CCW. This MUST stay opposite to the
    // Metal backend's MTLWindingClockwise. Both backends draw the same geometry
    // with the same (non-y-flipped) projection matrix; Metal's top-left / y-down
    // framebuffer origin reverses on-screen triangle winding relative to GL's
    // bottom-left / y-up origin, and the opposite front-face conventions cancel
    // that out so both backends cull the same geometric face. Do NOT "unify" the
    // two backends onto the same winding — that would invert culling on one side.
    glFrontFace(GL_CCW);
    return true;
}

void RenderDeviceGLES::endPass() {
    // 回绑默认 framebuffer;离屏 pass 结束后下一个 beginPass 会重设
    // viewport,主 pass 自身的 endPass 为幂等回绑。
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderDeviceGLES::submit(const RenderCommandList& commands) {
    static int submitCount = 0;
    submitCount++;
    const double submitStartMs = perf::nowMs();
    int surfaceCommands = 0;
    int gltfCommands = 0;
    int instancedCommands = 0;  // [I3DMDIAG] GltfPrimitiveInstanced 命令数
    int totalInstances = 0;     // [I3DMDIAG] 所有实例化命令的实例总数
    int vectorCommands = 0;
    int environmentCommands = 0;

    // 先消化自上次 submit 以来析构的 GLBuffer：删除引用其 id 的 VAO，
    // 防止 dangling VAO（必须在本帧任何 VAO 查询/绑定之前完成——GL 名字
    // 复用后同名 buffer 会命中陈旧条目）。
    purgeVaosForDeletedBuffers();

    GLuint currentProgram = 0;
    GLuint currentVao = 0;
    std::array<GLuint, kGltfWaterMaskTextureSlot + 1> currentTextures{};
    bool depthTestEnabled = true;
    bool blendEnabled = false;
    bool alphaToCoverageEnabled = false;
    bool polygonOffsetEnabled = false;
    bool cullFaceEnabled = true;
    bool depthWriteEnabled = true;

    for (const auto& cmd : commands) {
        switch (cmd.kind) {
            case RenderCommandKind::SurfaceTile:
                ++surfaceCommands;
                break;
            case RenderCommandKind::GltfPrimitive:
                ++gltfCommands;
                break;
            case RenderCommandKind::GltfPrimitiveInstanced:
                ++gltfCommands;
                ++instancedCommands;
                totalInstances += cmd.instanceCount;
                break;
            case RenderCommandKind::VectorOverlay:
                ++vectorCommands;
                break;
            case RenderCommandKind::SkyBackground:
            case RenderCommandKind::AtmosphereBackground:
                ++environmentCommands;
                break;
            case RenderCommandKind::Unknown:
                break;
        }

        auto* program = static_cast<GLShaderProgram*>(cmd.shader);
        auto* vb = static_cast<GLBuffer*>(cmd.vertexBuffer);
        auto* ib = static_cast<GLBuffer*>(cmd.indexBuffer);
        auto* instanceBuffer = static_cast<GLBuffer*>(cmd.instanceBuffer);

        if (!program || !vb) continue;

        if (currentProgram != program->glId()) {
            currentProgram = program->glId();
            glUseProgram(currentProgram);
        }

        // ---- 顶点属性设置（VAO 缓存）----
        // 布局分派逻辑保留在 recordVaoLayout（VAO 首次创建时录制一次），
        // 此处只做布局分类 + 一次 glBindVertexArray；element buffer 绑定
        // 也封在 VAO 里，不再逐 draw 重发。
        const bool isGltfVertexLayout =
            (cmd.kind == RenderCommandKind::GltfPrimitive ||
             cmd.kind == RenderCommandKind::GltfPrimitiveInstanced) &&
            cmd.vertexStride == 120;
        // 与原布局分派一致：instance 属性只挂在 32B/120B 分支上。
        const bool useInstanceAttribs =
            (cmd.vertexStride == 32 || isGltfVertexLayout) &&
            cmd.kind == RenderCommandKind::GltfPrimitiveInstanced &&
            instanceBuffer &&
            cmd.instanceStride == kGltfInstanceMatrixStride;

        VaoKey vaoKey;
        vaoKey.vertexBuffer = vb->glId();
        vaoKey.indexBuffer = ib ? ib->glId() : 0u;
        vaoKey.instanceBuffer =
            useInstanceAttribs ? instanceBuffer->glId() : 0u;
        if (cmd.vertexStride == 40) {
            vaoKey.layout = VertexLayoutKind::Terrain40;
            vaoKey.vertexStride = 40;
        } else if (cmd.vertexStride == 32 || isGltfVertexLayout) {
            vaoKey.layout = isGltfVertexLayout
                ? (useInstanceAttribs ? VertexLayoutKind::Gltf120Instanced
                                      : VertexLayoutKind::Gltf120)
                : (useInstanceAttribs ? VertexLayoutKind::Surface32Instanced
                                      : VertexLayoutKind::Surface32);
            vaoKey.vertexStride = static_cast<unsigned int>(cmd.vertexStride);
        } else if (cmd.vertexStride == 20) {
            // Terrain tile: pos(12) + uv(8), normal computed in shader
            vaoKey.layout = VertexLayoutKind::Terrain20;
            vaoKey.vertexStride = 20;
        } else if (cmd.vertexStride > 0) {
            // 显式 vertex stride（VectorLayer、SkyBox、Atmosphere 等使用）
            vaoKey.layout = VertexLayoutKind::SimpleStride;
            vaoKey.vertexStride = static_cast<unsigned int>(cmd.vertexStride);
        } else {
            // SurfaceTile 未显式给 stride 的兜底：32B 布局
            vaoKey.layout = VertexLayoutKind::Surface32;
            vaoKey.vertexStride = 32;
        }

        const GLuint vao = acquireVao(vaoKey);
        if (currentVao != vao) {
            glBindVertexArray(vao);
            currentVao = vao;
        }

        // ---- 纹理绑定 ----
        // glTF / terrain commands compact their material textures into GLES's
        // ≤16-unit range (see glesGltfTextureUnit): raster overlays move from
        // shared slots 15-18 to units 5-8, the water mask from 19 to 9, and the
        // aliased extension slots 5-14 are skipped. Every other command kind
        // (SurfaceTile, vector, environment) binds 1:1 at its vector index.
        const bool compactGltfUnits =
            cmd.kind == RenderCommandKind::GltfPrimitive ||
            cmd.kind == RenderCommandKind::GltfPrimitiveInstanced;
        const size_t textureCount =
            std::min(cmd.textures.size(), currentTextures.size());
        for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex) {
            if (!cmd.textures[textureIndex]) continue;
            const int unit = compactGltfUnits
                ? glesGltfTextureUnit(textureIndex)
                : static_cast<int>(textureIndex);
            if (unit < 0) continue;  // aliased extension slot — no live sampler
            auto* glTex = static_cast<GLTexture*>(cmd.textures[textureIndex]);
            const size_t unitIdx = static_cast<size_t>(unit);
            if (currentTextures[unitIdx] != glTex->glId()) {
                currentTextures[unitIdx] = glTex->glId();
                glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
                glBindTexture(GL_TEXTURE_2D, currentTextures[unitIdx]);
            }
        }
        // Sampler uniform 是 program 的持久状态且单位分配固定，每个 program
        // 只需设置一次（原先每 draw 重发 ~18 个 glUniform1i + 现场拼接名字）。
        if (!program->samplersConfigured()) {
            auto setSampler = [&](const char* name, int unit) {
                int loc = program->uniformLocation(name);
                if (loc >= 0) glUniform1i(loc, unit);
            };
            setSampler("u_tileTexture", 0);
            // 离屏后处理 aerial fog:depth 采样器绑 unit 1(其它 program
            // 未声明此名 → loc=-1 无副作用)。
            setSampler("u_depthTexture", 1);
            setSampler("u_baseColorTexture", 0);
            setSampler("u_metallicRoughnessTexture", 1);
            setSampler("u_normalTexture", 2);
            setSampler("u_occlusionTexture", 3);
            setSampler("u_emissiveTexture", 4);
            // Advanced PBR-extension samplers (specular / clearcoat / sheen /
            // anisotropy / transmission / specular-glossiness) are aliased to
            // u_baseColorTexture in the GLES glTF shader, so they need no unit
            // of their own. Raster overlays and the water mask are compacted
            // into the freed 5-9 range (mirrors glesGltfTextureUnit above).
            for (int i = 0; i < kMaxGltfRasterOverlays; ++i) {
                std::string name =
                    "u_mappedRasterTexture" + std::to_string(i);
                setSampler(name.c_str(), kGlesGltfRasterUnitBase + i);
            }
            setSampler("u_gltfWaterMaskTexture", kGlesGltfWaterUnit);
            setSampler("u_waterMask", 5);
            for (int i = 0; i < kMaxSurfaceImageryOverlays; ++i) {
                std::string name = "u_overlayTexture" + std::to_string(i);
                setSampler(name.c_str(), i + 1);
            }
            program->markSamplersConfigured();
        }

        // ---- Uniforms ----
        if (cmd.kind == RenderCommandKind::SurfaceTile && cmd.hasSurfaceTileUniforms) {
            auto set1 = [&](const char* name, float value) {
                int loc = program->uniformLocation(name);
                if (loc >= 0) glUniform1f(loc, value);
            };
            auto set3 = [&](const char* name, const std::array<float, 3>& value) {
                int loc = program->uniformLocation(name);
                if (loc >= 0) glUniform3fv(loc, 1, value.data());
            };
            auto set4 = [&](const char* name, const std::array<float, 4>& value) {
                int loc = program->uniformLocation(name);
                if (loc >= 0) glUniform4fv(loc, 1, value.data());
            };
            int mvpLoc = program->uniformLocation("u_modelViewProjection");
            if (mvpLoc >= 0) {
                glUniformMatrix4fv(
                    mvpLoc, 1, GL_FALSE, cmd.surfaceModelViewProjection.data());
            }
            set4("u_tileUV", cmd.surfaceTileUv);
            set4("u_clipUV", cmd.surfaceClipUv);
            for (int i = 0; i < kMaxSurfaceImageryOverlays; ++i) {
                std::string uvName = "u_overlayTileUV" + std::to_string(i);
                std::string opacityName = "u_overlayOpacity" + std::to_string(i);
                set4(uvName.c_str(), cmd.surfaceOverlayTileUvs[i]);
                set1(opacityName.c_str(), cmd.surfaceOverlayOpacities[i]);
            }
            set3("u_lightDir", cmd.surfaceLightDir);
            // u_tileOrigin removed — RTC is now baked into u_modelViewProjection
            // via CPU double-precision matrix multiplication in Scene.cpp.
            set3("u_fogColor", cmd.surfaceFogColor);
            set1("u_fogDensity", cmd.surfaceFogDensity);
            set1("u_tileOpacity", cmd.surfaceTileOpacity);
            set1("u_transitionOpacity", cmd.surfaceTransitionOpacity);
            set1("u_clipEnabled", cmd.surfaceClipEnabled);
            int overlayCountLoc = program->uniformLocation("u_overlayTextureCount");
            if (overlayCountLoc >= 0) {
                glUniform1i(overlayCountLoc, cmd.surfaceOverlayTextureCount);
            }
            set1("u_surfaceGeneration", cmd.surfaceGeneration);
            set1("u_hasWaterMask", cmd.surfaceHasWaterMask);
            set4("u_waterMaskTranslationScale",
                 cmd.surfaceWaterMaskTranslationScale);
            set4("u_waterMaskState", cmd.surfaceWaterMaskState);
        } else if (cmd.hasGltfUniforms) {
            // glTF/terrain 定长块直传：location 表在 program 首次使用时一次
            // 性解析（shader 未声明的名字为 -1 跳过），此后每 draw 零字符串
            // 哈希、零堆分配。
            const auto& table = gltfUniformTable();
            const std::vector<int>& locations = program->gltfBlockLocations();
            const float* block =
                reinterpret_cast<const float*>(&cmd.gltfUniforms);
            for (size_t entryIndex = 0; entryIndex < table.size();
                 ++entryIndex) {
                const int loc = locations[entryIndex];
                if (loc < 0) continue;
                const float* values = block + table[entryIndex].floatOffset;
                switch (table[entryIndex].count) {
                    case 1:
                        glUniform1f(loc, values[0]);
                        break;
                    case 2:
                        glUniform2fv(loc, 1, values);
                        break;
                    case 3:
                        glUniform3fv(loc, 1, values);
                        break;
                    case 4:
                        glUniform4fv(loc, 1, values);
                        break;
                    case 16:
                        glUniformMatrix4fv(loc, 1, GL_FALSE, values);
                        break;
                }
            }
        }
        for (const auto& [name, values] : cmd.uniforms) {
            int loc = program->uniformLocation(name);
            if (loc < 0) continue;

            switch (values.size()) {
                case 1:
                    glUniform1f(loc, values[0]);
                    break;
                case 2:
                    glUniform2fv(loc, 1, values.data());
                    break;
                case 3:
                    glUniform3fv(loc, 1, values.data());
                    break;
                case 4:
                    glUniform4fv(loc, 1, values.data());
                    break;
                case 9:
                    glUniformMatrix3fv(loc, 1, GL_FALSE, values.data());
                    break;
                case 16:
                    glUniformMatrix4fv(loc, 1, GL_FALSE, values.data());
                    break;
            }
        }

        // ---- 渲染状态 ----
        if (depthTestEnabled != cmd.depthTest) {
            if (cmd.depthTest) {
                glEnable(GL_DEPTH_TEST);
            } else {
                glDisable(GL_DEPTH_TEST);
            }
            depthTestEnabled = cmd.depthTest;
        }
        if (depthWriteEnabled != cmd.depthWrite) {
            glDepthMask(cmd.depthWrite ? GL_TRUE : GL_FALSE);
            depthWriteEnabled = cmd.depthWrite;
        }

        if (blendEnabled != cmd.blend) {
            if (cmd.blend) {
                glEnable(GL_BLEND);
            } else {
                glDisable(GL_BLEND);
            }
            blendEnabled = cmd.blend;
        }
        if (cmd.blend) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        if (polygonOffsetEnabled != cmd.blend) {
            if (cmd.blend) {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-1.0f, -1.0f);
            } else {
                glDisable(GL_POLYGON_OFFSET_FILL);
            }
            polygonOffsetEnabled = cmd.blend;
        }

        // Alpha-to-coverage:实例化 blend/透射 primitive 的顺序无关透明(靠 MSAA
        // 覆盖抖动),单 draw 画完所有实例。仅在未开常规 alpha 混合时启用(淡入淡出
        // 期 cmd.blend=true 会走真混合,此时不叠 A2C)。需要多重采样帧缓冲(4x MSAA)。
        const bool wantAlphaToCoverage = cmd.alphaToCoverage && !cmd.blend;
        if (alphaToCoverageEnabled != wantAlphaToCoverage) {
            if (wantAlphaToCoverage) {
                glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            } else {
                glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            }
            alphaToCoverageEnabled = wantAlphaToCoverage;
        }

        if (cullFaceEnabled != cmd.cullFace) {
            if (cmd.cullFace) {
                glEnable(GL_CULL_FACE);
            } else {
                glDisable(GL_CULL_FACE);
            }
            cullFaceEnabled = cmd.cullFace;
        }

        // ---- Draw ----
        GLenum mode = GL_TRIANGLES;
        switch (cmd.primitive) {
            case RenderCommand::PrimitiveType::Triangles:     mode = GL_TRIANGLES; break;
            case RenderCommand::PrimitiveType::TriangleStrip: mode = GL_TRIANGLE_STRIP; break;
            case RenderCommand::PrimitiveType::Lines:         mode = GL_LINES; break;
            case RenderCommand::PrimitiveType::LineStrip:     mode = GL_LINE_STRIP; break;
            case RenderCommand::PrimitiveType::Points:        mode = GL_POINTS; break;
        }

        if (ib) {
            GLenum indexType = (cmd.indexType == RenderCommand::IndexType::UInt32)
                                   ? GL_UNSIGNED_INT
                                   : GL_UNSIGNED_SHORT;
            // cmd.indexOffset counts indices (elements), matching the Metal
            // backend which multiplies it by the index size. glDrawElements takes
            // a byte offset, so scale by the index size here for parity. (Today
            // indexOffset is always 0, so 0 * size == 0 and behavior is unchanged;
            // this keeps the two backends consistent if a nonzero offset is ever
            // set upstream.)
            const GLsizei indexSizeBytes =
                (cmd.indexType == RenderCommand::IndexType::UInt32) ? 4 : 2;
            const intptr_t indexByteOffset =
                static_cast<intptr_t>(cmd.indexOffset) * indexSizeBytes;
            if (cmd.instanceCount > 0) {
                glDrawElementsInstanced(
                    mode,
                    cmd.indexCount,
                    indexType,
                    reinterpret_cast<void*>(indexByteOffset),
                    cmd.instanceCount);
            } else {
                glDrawElements(mode, cmd.indexCount, indexType,
                               reinterpret_cast<void*>(indexByteOffset));
            }
        } else {
            if (cmd.instanceCount > 0) {
                glDrawArraysInstanced(mode, 0, cmd.vertexCount, cmd.instanceCount);
            } else {
                glDrawArrays(mode, 0, cmd.vertexCount);
            }
        }
    }

    // Batch-level cleanup keeps RenderDevice ownership explicit without
    // thrashing GL state between adjacent SurfaceTile commands.
    // 属性启用/divisor/element buffer 状态都封在各 VAO 内部：这里只需解绑
    // VAO，不再逐属性拆除（也绝不能在 VAO 绑定状态下去改全局属性状态，
    // 否则会破坏该 VAO 录制的布局）。
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    // 此时作用于默认 VAO(0) 的 element 绑定，与旧行为一致。
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    // Unbind every unit the frame may have touched — SurfaceTile uses 0-4/5,
    // compacted glTF/terrain uses 0-9 (kGlesGltfWaterUnit).
    for (int textureUnit = kGlesGltfWaterUnit; textureUnit >= 0; --textureUnit) {
        glActiveTexture(GL_TEXTURE0 + textureUnit);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    const double submitMs = perf::nowMs() - submitStartMs;
    if (submitCount <= 1 || submitCount % 120 == 0 || submitMs >= 25.0) {
        GLenum err = glGetError();
        __android_log_print(ANDROID_LOG_INFO, "GLES",
            "submit #%d: %zu commands, ms=%.3f surface=%d gltf=%d inst=%d(%d) vector=%d env=%d glError=%d",
            submitCount,
            commands.size(),
            submitMs,
            surfaceCommands,
            gltfCommands,
            instancedCommands,
            totalInstances,
            vectorCommands,
            environmentCommands,
            err);
    }
}

// ============================================================
// VAO 缓存
// ============================================================

size_t RenderDeviceGLES::VaoKeyHash::operator()(const VaoKey& key) const {
    // FNV-1a 变体：各字段依次折入。
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t value) {
        h ^= value;
        h *= 1099511628211ull;
    };
    mix(key.vertexBuffer);
    mix(key.indexBuffer);
    mix(key.instanceBuffer);
    mix(static_cast<uint64_t>(key.layout));
    mix(key.vertexStride);
    return static_cast<size_t>(h);
}

unsigned int RenderDeviceGLES::acquireVao(const VaoKey& key) {
    ++vaoUseCounter_;
    auto it = vaoCache_.find(key);
    if (it != vaoCache_.end()) {
        it->second.lastUse = vaoUseCounter_;
        return it->second.vao;
    }

    // 上限逐出：淘汰最久未用的一条，防止长时间运行 VAO 无限增长。
    // 被逐出的不可能是当前绑定的 VAO（当前绑定者 lastUse 最大）；即使
    // GL 名字被随后的 glGenVertexArrays 复用，新建路径也会无条件
    // glBindVertexArray，绑定状态不会错位。
    if (vaoCache_.size() >= kMaxVaoCacheEntries) {
        auto lru = vaoCache_.begin();
        for (auto candidate = vaoCache_.begin(); candidate != vaoCache_.end();
             ++candidate) {
            if (candidate->second.lastUse < lru->second.lastUse) {
                lru = candidate;
            }
        }
        GLuint staleVao = lru->second.vao;
        glDeleteVertexArrays(1, &staleVao);
        vaoCache_.erase(lru);
    }

    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    recordVaoLayout(key);
    vaoCache_.emplace(key, VaoEntry{vao, vaoUseCounter_});
    return vao;
}

// 在新建 VAO 的绑定状态下录制顶点布局。新 VAO 的所有属性默认 disabled、
// divisor 默认 0，因此只需启用本布局用到的属性。GL_ARRAY_BUFFER 绑定是
// 全局状态（glVertexAttribPointer 捕获调用时刻绑定的 buffer），而
// GL_ELEMENT_ARRAY_BUFFER 绑定属于 VAO 状态，须一并录入。
void RenderDeviceGLES::recordVaoLayout(const VaoKey& key) {
    glBindBuffer(GL_ARRAY_BUFFER, key.vertexBuffer);
    const GLsizei stride = static_cast<GLsizei>(key.vertexStride);
    switch (key.layout) {
        case VertexLayoutKind::Surface32:
        case VertexLayoutKind::Surface32Instanced:
            // Surface: POSITION(12) + NORMAL(12) + TEXCOORD_0(8) = 32 bytes.
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(12));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(24));
            break;
        case VertexLayoutKind::Terrain40:
            // Terrain: POSITION(12) + NORMAL(12) + packed TEXCOORD_0/1(16).
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(12));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(24));
            break;
        case VertexLayoutKind::Gltf120:
        case VertexLayoutKind::Gltf120Instanced:
            // glTF: POSITION/NORMAL + 8 packed TEXCOORD sets + COLOR_0 + TANGENT.
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(12));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(24));
            // attrib 10-14：packed TEXCOORD 2-7 + COLOR_0 + TANGENT，
            // 依次位于字节偏移 40/56/72/88/104。
            for (GLuint i = 0; i < 5; ++i) {
                glEnableVertexAttribArray(10 + i);
                glVertexAttribPointer(
                    10 + i, 4, GL_FLOAT, GL_FALSE, stride,
                    reinterpret_cast<void*>(
                        static_cast<uintptr_t>(40 + 16 * i)));
            }
            break;
        case VertexLayoutKind::Terrain20:
            // Terrain tile: pos(12) + uv(8), normal computed in shader
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(12));
            break;
        case VertexLayoutKind::SimpleStride: {
            // 显式 vertex stride（VectorLayer、SkyBox、Atmosphere 等使用）
            // 根据 stride 推断分量数：8=vec2, 12=vec3
            const int compCount = (key.vertexStride == 8) ? 2 : 3;
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, compCount, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<void*>(0));
            break;
        }
    }

    if (key.layout == VertexLayoutKind::Surface32Instanced ||
        key.layout == VertexLayoutKind::Gltf120Instanced) {
        // Instance 矩阵属性：attrib 3-6 为 4 列 vec4（偏移 0/16/32/48），
        // attrib 7-9 为 3 条 vec3（偏移 64/76/88），逐 instance 前进。
        glBindBuffer(GL_ARRAY_BUFFER, key.instanceBuffer);
        for (GLuint i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(3 + i);
            glVertexAttribPointer(
                3 + i, 4, GL_FLOAT, GL_FALSE, kGltfInstanceMatrixStride,
                reinterpret_cast<void*>(static_cast<uintptr_t>(16 * i)));
            glVertexAttribDivisor(3 + i, 1);
        }
        for (GLuint i = 0; i < 3; ++i) {
            glEnableVertexAttribArray(7 + i);
            glVertexAttribPointer(
                7 + i, 3, GL_FLOAT, GL_FALSE, kGltfInstanceMatrixStride,
                reinterpret_cast<void*>(static_cast<uintptr_t>(64 + 12 * i)));
            glVertexAttribDivisor(7 + i, 1);
        }
    }

    // element buffer 绑定录入 VAO（0 = 无索引，走 glDrawArrays 路径）。
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, key.indexBuffer);
}

void RenderDeviceGLES::purgeVaosForDeletedBuffers() {
    std::vector<unsigned int> deleted =
        vaoRegistry_->takePendingDeletedBuffers();
    if (deleted.empty() || vaoCache_.empty()) {
        return;
    }
    std::sort(deleted.begin(), deleted.end());
    auto isDeleted = [&deleted](unsigned int id) {
        return id != 0 &&
               std::binary_search(deleted.begin(), deleted.end(), id);
    };
    for (auto it = vaoCache_.begin(); it != vaoCache_.end();) {
        const VaoKey& key = it->first;
        if (isDeleted(key.vertexBuffer) || isDeleted(key.indexBuffer) ||
            isDeleted(key.instanceBuffer)) {
            GLuint vao = it->second.vao;
            glDeleteVertexArrays(1, &vao);
            it = vaoCache_.erase(it);
        } else {
            ++it;
        }
    }
}

void RenderDeviceGLES::dropVaoCache(bool deleteGlObjects) {
    if (deleteGlObjects) {
        for (auto& entry : vaoCache_) {
            GLuint vao = entry.second.vao;
            glDeleteVertexArrays(1, &vao);
        }
    }
    vaoCache_.clear();
}

void RenderDeviceGLES::endFrame() {
    // EGL swap 由外部调用者处理（eglSwapBuffers）
    // eglSwapBuffers() 会隐式等待 GPU 完成，不需要显式 glFlush()
    // 移除 glFlush() 避免阻塞 CPU→GPU 并行
}

// ============================================================
// 生命周期
// ============================================================

void RenderDeviceGLES::onSurfaceCreated() {
    // 新 context：旧 context 的 VAO 名字已全部失效，不能在新 context 里
    // glDelete（可能误删新 context 恰好复用的同名对象），直接丢弃缓存；
    // 旧 context 期间析构的 buffer 登记也一并作废。
    dropVaoCache(/*deleteGlObjects=*/false);
    (void)vaoRegistry_->takePendingDeletedBuffers();
    // EGL context 由外部管理，这里只做 GL 状态初始化
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDepthFunc(GL_GEQUAL); // Reverse-Z: greater depth = closer

    // Fragment texture-unit caps gate how many sampler2D a fragment shader may
    // declare. The GLES glTF shader is compacted to ≤16 samplers so it links at
    // the GLES floor (GL_MAX_TEXTURE_IMAGE_UNITS==16, e.g. Adreno). Log both
    // caps once so the assumption is verifiable on-device.
    GLint maxFragUnits = 0;
    GLint maxCombinedUnits = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxFragUnits);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxCombinedUnits);
    __android_log_print(ANDROID_LOG_INFO, "GLES",
        "GL_MAX_TEXTURE_IMAGE_UNITS=%d GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS=%d",
        maxFragUnits, maxCombinedUnits);
}

void RenderDeviceGLES::onSurfaceChanged(int width, int height) {
    viewportWidth_ = width;
    viewportHeight_ = height;
    glViewport(0, 0, width, height);
}

void RenderDeviceGLES::onSurfaceDestroyed() {
    // 注意：此时 EGL context 可能已失效，不要调用 GL 函数
    // GPU 资源由 unique_ptr 析构函数释放，但需要在有效 context 下调用
    // 实际使用时，应在 context 销毁前先销毁 RenderDeviceGLES
    // VAO 缓存同理：只丢弃 CPU 侧记录，GL 对象随 context 一起销毁。
    dropVaoCache(/*deleteGlObjects=*/false);
}

} // namespace earth_engine
