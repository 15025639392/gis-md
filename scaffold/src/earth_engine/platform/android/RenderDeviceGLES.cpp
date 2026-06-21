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

bool supportsTextureAnisotropy() {
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    return extensions &&
           std::string(extensions).find("GL_EXT_texture_filter_anisotropic") != std::string::npos;
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
        GLfloat deviceMaxAnisotropy = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &deviceMaxAnisotropy);
        const GLfloat anisotropy = std::clamp(
            static_cast<GLfloat>(desc.maxAnisotropy),
            1.0f,
            deviceMaxAnisotropy);
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
    return glGetError() == GL_NO_ERROR;
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
    return glGetError() == GL_NO_ERROR;
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

void RenderDeviceGLES::beginFrame() {
    glViewport(0, 0, viewportWidth_, viewportHeight_);
    // Restore frame-global state before clear. Previous overlay/background
    // commands may leave depth writes or blending disabled/enabled; stale
    // depth makes the next frame's surface tiles appear perforated.
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_POLYGON_OFFSET_FILL);
    // Clear color: sky-horizon blue (fullscreen atmosphere pass covers this).
    // TODO: pass frameState.clearR/G/B from Engine after beginFrame() reorder.
    glClearColor(0.1f, 0.3f, 0.6f, 1.0f);
    glClearDepthf(0.0f);   // Reverse-Z: clear to 0 (farthest)
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL); // Reverse-Z: greater depth = closer
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
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
            case RenderCommandKind::GlobeSurface:
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
            int tileSamplerLoc = program->uniformLocation("u_tileTexture");
            if (tileSamplerLoc >= 0) {
                glUniform1i(tileSamplerLoc, 0);
            }
            int waterMaskLoc = program->uniformLocation("u_waterMask");
            if (waterMaskLoc >= 0) {
                glUniform1i(waterMaskLoc, 5);
            }
            for (int i = 0; i < kMaxSurfaceImageryOverlays; ++i) {
                std::string name = "u_overlayTexture" + std::to_string(i);
                int overlaySamplerLoc = program->uniformLocation(name);
                if (overlaySamplerLoc >= 0) {
                    glUniform1i(overlaySamplerLoc, 1 + i);
                }
            }
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
            // Globe vertex: float3 pos + float3 normal + float2 tex = 32 bytes
            constexpr int kGlobeStride = 32;
            setAttribEnabled(0, attrib0Enabled, true);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kGlobeStride,
                                  reinterpret_cast<void*>(0));
            setAttribEnabled(1, attrib1Enabled, true);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kGlobeStride,
                                  reinterpret_cast<void*>(12));
            setAttribEnabled(2, attrib2Enabled, true);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kGlobeStride,
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
        const size_t textureCount =
            std::min(cmd.textures.size(), currentTextures.size());
        for (size_t textureIndex = 0; textureIndex < textureCount; ++textureIndex) {
            if (!cmd.textures[textureIndex]) continue;
            auto* glTex = static_cast<GLTexture*>(cmd.textures[textureIndex]);
            if (currentTextures[textureIndex] != glTex->glId()) {
                currentTextures[textureIndex] = glTex->glId();
                glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(textureIndex));
                glBindTexture(GL_TEXTURE_2D, currentTextures[textureIndex]);
            }
        }
        auto setSampler = [&](const char* name, int unit) {
            int loc = program->uniformLocation(name);
            if (loc >= 0) glUniform1i(loc, unit);
        };
        if (!cmd.textures.empty()) {
            setSampler("u_tileTexture", 0);
            setSampler("u_baseColorTexture", 0);
            setSampler("u_metallicRoughnessTexture", 1);
            setSampler("u_normalTexture", 2);
            setSampler("u_occlusionTexture", 3);
            setSampler("u_emissiveTexture", 4);
            setSampler("u_specularTexture", 5);
            setSampler("u_specularColorTexture", 6);
            setSampler("u_clearcoatTexture", 7);
            setSampler("u_clearcoatRoughnessTexture", 8);
            setSampler("u_clearcoatNormalTexture", 9);
            setSampler("u_sheenColorTexture", 10);
            setSampler("u_sheenRoughnessTexture", 11);
            setSampler("u_anisotropyTexture", 12);
            setSampler("u_specularGlossinessTexture", 13);
            setSampler("u_transmissionTexture", 14);
            for (int i = 0; i < kMaxGltfRasterOverlays; ++i) {
                std::string name =
                    "u_mappedRasterTexture" + std::to_string(i);
                setSampler(
                    name.c_str(),
                    kGltfRasterOverlayTextureBase + i);
            }
            setSampler("u_gltfWaterMaskTexture", kGltfWaterMaskTextureSlot);
        }
        for (int i = 0; i < kMaxSurfaceImageryOverlays; ++i) {
            std::string name = "u_overlayTexture" + std::to_string(i);
            setSampler(name.c_str(), i + 1);
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
            if (cmd.instanceCount > 0) {
                glDrawElementsInstanced(
                    mode,
                    cmd.indexCount,
                    indexType,
                    reinterpret_cast<void*>(static_cast<intptr_t>(cmd.indexOffset)),
                    cmd.instanceCount);
            } else {
                glDrawElements(mode, cmd.indexCount, indexType,
                               reinterpret_cast<void*>(static_cast<intptr_t>(cmd.indexOffset)));
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
    for (int textureUnit = 5; textureUnit >= 0; --textureUnit) {
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
    // 这里可以添加 flush 确保命令提交
    glFlush();
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
