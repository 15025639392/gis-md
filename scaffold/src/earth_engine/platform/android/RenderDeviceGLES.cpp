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

} // namespace

// ============================================================
// GLTexture
// ============================================================

GLTexture::GLTexture(unsigned int id, int width, int height)
    : id_(id), width_(width), height_(height) {}

GLTexture::~GLTexture() {
    if (id_) {
        glDeleteTextures(1, &id_);
    }
}

// ============================================================
// GLBuffer
// ============================================================

GLBuffer::GLBuffer(unsigned int id, size_t size, unsigned int target)
    : id_(id), size_(size), target_(target) {}

GLBuffer::~GLBuffer() {
    if (id_) {
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

RenderDeviceGLES::RenderDeviceGLES() = default;

RenderDeviceGLES::~RenderDeviceGLES() {
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

    if (desc.mipmap && desc.data) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    return std::make_unique<GLTexture>(id, desc.width, desc.height);
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

    return std::make_unique<GLBuffer>(id, desc.size, target);
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

std::unique_ptr<Framebuffer> RenderDeviceGLES::createFramebuffer(const FramebufferDesc& /*desc*/) {
    // Stage 2 MVP 不需要自定义 framebuffer（使用默认 framebuffer）
    return nullptr;
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
    glViewport(0, 0, viewportWidth_, viewportHeight_);
    // Restore frame-global state before clear. Previous overlay/background
    // commands may leave depth writes or blending disabled/enabled; stale
    // depth makes the next frame's surface tiles appear perforated.
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
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
}

void RenderDeviceGLES::submit(const RenderCommandList& commands) {
    static int submitCount = 0;
    submitCount++;
    const double submitStartMs = perf::nowMs();
    int surfaceCommands = 0;
    int gltfCommands = 0;
    int vectorCommands = 0;
    int environmentCommands = 0;

    GLuint currentProgram = 0;
    GLuint currentArrayBuffer = 0;
    GLuint currentElementArrayBuffer = 0;
    std::array<GLuint, kGltfWaterMaskTextureSlot + 1> currentTextures{};
    bool attrib0Enabled = false;
    bool attrib1Enabled = false;
    bool attrib2Enabled = false;
    bool attrib3Enabled = false;
    bool attrib4Enabled = false;
    bool attrib5Enabled = false;
    bool attrib6Enabled = false;
    bool attrib7Enabled = false;
    bool attrib8Enabled = false;
    bool attrib9Enabled = false;
    bool attrib10Enabled = false;
    bool attrib11Enabled = false;
    bool attrib12Enabled = false;
    bool attrib13Enabled = false;
    bool attrib14Enabled = false;
    bool depthTestEnabled = true;
    bool blendEnabled = false;
    bool polygonOffsetEnabled = false;
    bool cullFaceEnabled = true;
    bool depthWriteEnabled = true;

    auto setAttribEnabled = [](GLuint index, bool& cached, bool enabled) {
        if (cached == enabled) return;
        if (enabled) {
            glEnableVertexAttribArray(index);
        } else {
            glDisableVertexAttribArray(index);
        }
        cached = enabled;
    };

    for (const auto& cmd : commands) {
        switch (cmd.kind) {
            case RenderCommandKind::SurfaceTile:
                ++surfaceCommands;
                break;
            case RenderCommandKind::GltfPrimitive:
            case RenderCommandKind::GltfPrimitiveInstanced:
                ++gltfCommands;
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

        // ---- 顶点属性设置 ----
        if (currentArrayBuffer != vb->glId()) {
            currentArrayBuffer = vb->glId();
            glBindBuffer(GL_ARRAY_BUFFER, currentArrayBuffer);
        }

        const bool isGltfVertexLayout =
            (cmd.kind == RenderCommandKind::GltfPrimitive ||
             cmd.kind == RenderCommandKind::GltfPrimitiveInstanced) &&
            cmd.vertexStride == 120;
        if (cmd.vertexStride == 32 || isGltfVertexLayout) {
            // Surface: POSITION(12) + NORMAL(12) + TEXCOORD_0(8) = 32 bytes.
            // glTF: POSITION/NORMAL + 8 packed TEXCOORD sets + COLOR_0 + TANGENT.
            const GLsizei vertexStride =
                static_cast<GLsizei>(cmd.vertexStride);
            setAttribEnabled(0, attrib0Enabled, true);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertexStride,
                                  reinterpret_cast<void*>(0));
            glVertexAttribDivisor(0, 0);
            setAttribEnabled(1, attrib1Enabled, true);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, vertexStride,
                                  reinterpret_cast<void*>(12));
            glVertexAttribDivisor(1, 0);
            setAttribEnabled(2, attrib2Enabled, true);
            glVertexAttribPointer(2,
                                  isGltfVertexLayout ? 4 : 2,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  vertexStride,
                                  reinterpret_cast<void*>(24));
            glVertexAttribDivisor(2, 0);
            if (isGltfVertexLayout) {
                setAttribEnabled(10, attrib10Enabled, true);
                glVertexAttribPointer(10, 4, GL_FLOAT, GL_FALSE, vertexStride,
                                      reinterpret_cast<void*>(40));
                glVertexAttribDivisor(10, 0);
                setAttribEnabled(11, attrib11Enabled, true);
                glVertexAttribPointer(11, 4, GL_FLOAT, GL_FALSE, vertexStride,
                                      reinterpret_cast<void*>(56));
                glVertexAttribDivisor(11, 0);
                setAttribEnabled(12, attrib12Enabled, true);
                glVertexAttribPointer(12, 4, GL_FLOAT, GL_FALSE, vertexStride,
                                      reinterpret_cast<void*>(72));
                glVertexAttribDivisor(12, 0);
                setAttribEnabled(13, attrib13Enabled, true);
                glVertexAttribPointer(13, 4, GL_FLOAT, GL_FALSE, vertexStride,
                                      reinterpret_cast<void*>(88));
                glVertexAttribDivisor(13, 0);
                setAttribEnabled(14, attrib14Enabled, true);
                glVertexAttribPointer(14, 4, GL_FLOAT, GL_FALSE, vertexStride,
                                      reinterpret_cast<void*>(104));
                glVertexAttribDivisor(14, 0);
            } else {
                setAttribEnabled(10, attrib10Enabled, false);
                setAttribEnabled(11, attrib11Enabled, false);
                setAttribEnabled(12, attrib12Enabled, false);
                setAttribEnabled(13, attrib13Enabled, false);
                setAttribEnabled(14, attrib14Enabled, false);
            }
            if (cmd.kind == RenderCommandKind::GltfPrimitiveInstanced &&
                instanceBuffer &&
                cmd.instanceStride == kGltfInstanceMatrixStride) {
                if (currentArrayBuffer != instanceBuffer->glId()) {
                    currentArrayBuffer = instanceBuffer->glId();
                    glBindBuffer(GL_ARRAY_BUFFER, currentArrayBuffer);
                }
                setAttribEnabled(3, attrib3Enabled, true);
                glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE,
                                      kGltfInstanceMatrixStride,
                                      reinterpret_cast<void*>(0));
                glVertexAttribDivisor(3, 1);
                setAttribEnabled(4, attrib4Enabled, true);
                glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE,
                                      kGltfInstanceMatrixStride,
                                      reinterpret_cast<void*>(16));
                glVertexAttribDivisor(4, 1);
                setAttribEnabled(5, attrib5Enabled, true);
                glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE,
                                      kGltfInstanceMatrixStride,
                                      reinterpret_cast<void*>(32));
                glVertexAttribDivisor(5, 1);
                setAttribEnabled(6, attrib6Enabled, true);
                glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE,
                                      kGltfInstanceMatrixStride,
                                      reinterpret_cast<void*>(48));
                glVertexAttribDivisor(6, 1);
                setAttribEnabled(7, attrib7Enabled, true);
                glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE,
                                      kGltfInstanceMatrixStride,
                                      reinterpret_cast<void*>(64));
                glVertexAttribDivisor(7, 1);
                setAttribEnabled(8, attrib8Enabled, true);
                glVertexAttribPointer(8, 3, GL_FLOAT, GL_FALSE,
                                      kGltfInstanceMatrixStride,
                                      reinterpret_cast<void*>(76));
                glVertexAttribDivisor(8, 1);
                setAttribEnabled(9, attrib9Enabled, true);
                glVertexAttribPointer(9, 3, GL_FLOAT, GL_FALSE,
                                      kGltfInstanceMatrixStride,
                                      reinterpret_cast<void*>(88));
                glVertexAttribDivisor(9, 1);
            } else {
                setAttribEnabled(3, attrib3Enabled, false);
                setAttribEnabled(4, attrib4Enabled, false);
                setAttribEnabled(5, attrib5Enabled, false);
                setAttribEnabled(6, attrib6Enabled, false);
                setAttribEnabled(7, attrib7Enabled, false);
                setAttribEnabled(8, attrib8Enabled, false);
                setAttribEnabled(9, attrib9Enabled, false);
            }
        } else if (cmd.vertexStride == 20) {
            // Terrain tile: pos(12) + uv(8), normal computed in shader
            setAttribEnabled(0, attrib0Enabled, true);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20,
                                  reinterpret_cast<void*>(0));
            glVertexAttribDivisor(0, 0);
            setAttribEnabled(1, attrib1Enabled, false);
            setAttribEnabled(2, attrib2Enabled, true);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 20,
                                  reinterpret_cast<void*>(12));
            glVertexAttribDivisor(2, 0);
            setAttribEnabled(3, attrib3Enabled, false);
            setAttribEnabled(4, attrib4Enabled, false);
            setAttribEnabled(5, attrib5Enabled, false);
            setAttribEnabled(6, attrib6Enabled, false);
            setAttribEnabled(7, attrib7Enabled, false);
            setAttribEnabled(8, attrib8Enabled, false);
            setAttribEnabled(9, attrib9Enabled, false);
            setAttribEnabled(10, attrib10Enabled, false);
            setAttribEnabled(11, attrib11Enabled, false);
            setAttribEnabled(12, attrib12Enabled, false);
            setAttribEnabled(13, attrib13Enabled, false);
            setAttribEnabled(14, attrib14Enabled, false);
        } else if (cmd.vertexStride > 0) {
            // 显式 vertex stride（VectorLayer、SkyBox、Atmosphere 等使用）
            // 根据 stride 推断分量数：8=vec2, 12=vec3
            int compCount = 3;
            if (cmd.vertexStride == 8) compCount = 2;   // vec2
            else if (cmd.vertexStride == 12) compCount = 3; // vec3
            setAttribEnabled(0, attrib0Enabled, true);
            glVertexAttribPointer(0, compCount, GL_FLOAT, GL_FALSE, cmd.vertexStride,
                                  reinterpret_cast<void*>(0));
            setAttribEnabled(1, attrib1Enabled, false);
            setAttribEnabled(2, attrib2Enabled, false);
            glVertexAttribDivisor(0, 0);
            setAttribEnabled(3, attrib3Enabled, false);
            setAttribEnabled(4, attrib4Enabled, false);
            setAttribEnabled(5, attrib5Enabled, false);
            setAttribEnabled(6, attrib6Enabled, false);
            setAttribEnabled(7, attrib7Enabled, false);
            setAttribEnabled(8, attrib8Enabled, false);
            setAttribEnabled(9, attrib9Enabled, false);
            setAttribEnabled(10, attrib10Enabled, false);
            setAttribEnabled(11, attrib11Enabled, false);
            setAttribEnabled(12, attrib12Enabled, false);
            setAttribEnabled(13, attrib13Enabled, false);
            setAttribEnabled(14, attrib14Enabled, false);
            glVertexAttribDivisor(3, 0);
            glVertexAttribDivisor(4, 0);
            glVertexAttribDivisor(5, 0);
            glVertexAttribDivisor(6, 0);
            glVertexAttribDivisor(7, 0);
            glVertexAttribDivisor(8, 0);
            glVertexAttribDivisor(9, 0);
            glVertexAttribDivisor(10, 0);
            glVertexAttribDivisor(11, 0);
            glVertexAttribDivisor(12, 0);
            glVertexAttribDivisor(13, 0);
            glVertexAttribDivisor(14, 0);
        } else {
            // SurfaceTile vertex: float3 pos + float3 normal + float2 tex = 32 bytes
            constexpr int kSurfaceStride = 32;
            setAttribEnabled(0, attrib0Enabled, true);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kSurfaceStride,
                                  reinterpret_cast<void*>(0));
            setAttribEnabled(1, attrib1Enabled, true);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kSurfaceStride,
                                  reinterpret_cast<void*>(12));
            setAttribEnabled(2, attrib2Enabled, true);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kSurfaceStride,
                                  reinterpret_cast<void*>(24));
            glVertexAttribDivisor(0, 0);
            glVertexAttribDivisor(1, 0);
            glVertexAttribDivisor(2, 0);
            setAttribEnabled(3, attrib3Enabled, false);
            setAttribEnabled(4, attrib4Enabled, false);
            setAttribEnabled(5, attrib5Enabled, false);
            setAttribEnabled(6, attrib6Enabled, false);
            setAttribEnabled(7, attrib7Enabled, false);
            setAttribEnabled(8, attrib8Enabled, false);
            setAttribEnabled(9, attrib9Enabled, false);
            setAttribEnabled(10, attrib10Enabled, false);
            setAttribEnabled(11, attrib11Enabled, false);
            setAttribEnabled(12, attrib12Enabled, false);
            setAttribEnabled(13, attrib13Enabled, false);
            setAttribEnabled(14, attrib14Enabled, false);
            glVertexAttribDivisor(3, 0);
            glVertexAttribDivisor(4, 0);
            glVertexAttribDivisor(5, 0);
            glVertexAttribDivisor(6, 0);
            glVertexAttribDivisor(7, 0);
            glVertexAttribDivisor(8, 0);
            glVertexAttribDivisor(9, 0);
            glVertexAttribDivisor(10, 0);
            glVertexAttribDivisor(11, 0);
        }

        const GLuint elementArrayBuffer = ib ? ib->glId() : 0;
        if (currentElementArrayBuffer != elementArrayBuffer) {
            currentElementArrayBuffer = elementArrayBuffer;
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, currentElementArrayBuffer);
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
    if (attrib0Enabled) glDisableVertexAttribArray(0);
    if (attrib1Enabled) glDisableVertexAttribArray(1);
    if (attrib2Enabled) glDisableVertexAttribArray(2);
    if (attrib3Enabled) {
        glVertexAttribDivisor(3, 0);
        glDisableVertexAttribArray(3);
    }
    if (attrib4Enabled) {
        glVertexAttribDivisor(4, 0);
        glDisableVertexAttribArray(4);
    }
    if (attrib5Enabled) {
        glVertexAttribDivisor(5, 0);
        glDisableVertexAttribArray(5);
    }
    if (attrib6Enabled) {
        glVertexAttribDivisor(6, 0);
        glDisableVertexAttribArray(6);
    }
    if (attrib7Enabled) {
        glVertexAttribDivisor(7, 0);
        glDisableVertexAttribArray(7);
    }
    if (attrib8Enabled) {
        glVertexAttribDivisor(8, 0);
        glDisableVertexAttribArray(8);
    }
    if (attrib9Enabled) {
        glVertexAttribDivisor(9, 0);
        glDisableVertexAttribArray(9);
    }
    if (attrib10Enabled) {
        glVertexAttribDivisor(10, 0);
        glDisableVertexAttribArray(10);
    }
    if (attrib11Enabled) {
        glVertexAttribDivisor(11, 0);
        glDisableVertexAttribArray(11);
    }
    if (attrib12Enabled) {
        glVertexAttribDivisor(12, 0);
        glDisableVertexAttribArray(12);
    }
    if (attrib13Enabled) {
        glVertexAttribDivisor(13, 0);
        glDisableVertexAttribArray(13);
    }
    if (attrib14Enabled) {
        glVertexAttribDivisor(14, 0);
        glDisableVertexAttribArray(14);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
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
            "submit #%d: %zu commands, ms=%.3f surface=%d gltf=%d vector=%d env=%d glError=%d",
            submitCount,
            commands.size(),
            submitMs,
            surfaceCommands,
            gltfCommands,
            vectorCommands,
            environmentCommands,
            err);
    }
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
}

} // namespace earth_engine
