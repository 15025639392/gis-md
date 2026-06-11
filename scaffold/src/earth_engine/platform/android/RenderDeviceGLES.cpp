#include "RenderDeviceGLES.h"
#include "../../renderer/RenderCommand.h"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>

namespace earth_engine {

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
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat,
                 desc.width, desc.height, 0,
                 format, type, desc.data);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    desc.minFilter == TextureDesc::Filter::Linear
                        ? (desc.mipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR)
                        : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    desc.minFilter == TextureDesc::Filter::Linear ? GL_LINEAR : GL_NEAREST);

    GLint wrapS = desc.wrapS == TextureDesc::Wrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    GLint wrapT = desc.wrapT == TextureDesc::Wrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);

    if (desc.mipmap && desc.data) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    return std::make_unique<GLTexture>(id, desc.width, desc.height);
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

    GLuint currentProgram = 0;
    GLuint currentArrayBuffer = 0;
    GLuint currentElementArrayBuffer = 0;
    GLuint currentTexture0 = 0;
    GLuint currentTexture1 = 0;
    bool attrib0Enabled = false;
    bool attrib1Enabled = false;
    bool attrib2Enabled = false;
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
        auto* program = static_cast<GLShaderProgram*>(cmd.shader);
        auto* vb = static_cast<GLBuffer*>(cmd.vertexBuffer);
        auto* ib = static_cast<GLBuffer*>(cmd.indexBuffer);

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
                glUniform1i(waterMaskLoc, 1);
            }
        }

        // ---- 顶点属性设置 ----
        if (currentArrayBuffer != vb->glId()) {
            currentArrayBuffer = vb->glId();
            glBindBuffer(GL_ARRAY_BUFFER, currentArrayBuffer);
        }

        if (cmd.kind == RenderCommandKind::SurfaceTile) {
            constexpr int kSurfaceStride = 20;  // pos(12) + uv(8)
            setAttribEnabled(0, attrib0Enabled, true);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kSurfaceStride,
                                  reinterpret_cast<void*>(0));
            // normal disabled — computed in vertex shader from position
            setAttribEnabled(1, attrib1Enabled, false);
            setAttribEnabled(2, attrib2Enabled, true);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kSurfaceStride,
                                  reinterpret_cast<void*>(12));
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
        }

        const GLuint elementArrayBuffer = ib ? ib->glId() : 0;
        if (currentElementArrayBuffer != elementArrayBuffer) {
            currentElementArrayBuffer = elementArrayBuffer;
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, currentElementArrayBuffer);
        }

        // ---- 纹理绑定 ----
        if (!cmd.textures.empty() && cmd.textures[0]) {
            auto* glTex = static_cast<GLTexture*>(cmd.textures[0]);
            if (currentTexture0 != glTex->glId()) {
                currentTexture0 = glTex->glId();
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, currentTexture0);
            }
        }
        if (cmd.textures.size() > 1 && cmd.textures[1]) {
            auto* glTex = static_cast<GLTexture*>(cmd.textures[1]);
            if (currentTexture1 != glTex->glId()) {
                currentTexture1 = glTex->glId();
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, currentTexture1);
            }
        }

        // ---- Uniforms ----
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
            glDrawElements(mode, cmd.indexCount, indexType,
                           reinterpret_cast<void*>(static_cast<intptr_t>(cmd.indexOffset)));
        } else {
            glDrawArrays(mode, 0, cmd.vertexCount);
        }
    }

    // Batch-level cleanup keeps RenderDevice ownership explicit without
    // thrashing GL state between adjacent SurfaceTile commands.
    if (attrib0Enabled) glDisableVertexAttribArray(0);
    if (attrib1Enabled) glDisableVertexAttribArray(1);
    if (attrib2Enabled) glDisableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (submitCount <= 1 || submitCount % 300 == 0) {
        GLenum err = glGetError();
        __android_log_print(ANDROID_LOG_INFO, "GLES",
            "submit #%d: %zu commands, glError=%d",
            submitCount, commands.size(), err);
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
