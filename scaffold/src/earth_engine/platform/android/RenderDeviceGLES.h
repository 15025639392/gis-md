#pragma once

#include "../../renderer/RenderDevice.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

namespace earth_engine {

/// GLBuffer 析构 → VAO 失效登记表。
/// VAO 记录了对顶点/索引缓冲的引用：瓦片卸载时 GLBuffer 析构，若引用它的
/// VAO 不同步清除，GL 名字复用后继续绘制是未定义行为。GLBuffer 析构时把
/// 自己的 buffer id 投递到这里，device 在下一次 submit() 开头（GL 线程）
/// 批量清除含该 id 的 VAO 条目——GL 对象删除永远只发生在 GL 线程。
/// 生命周期：device 与所有 GLBuffer 通过 shared_ptr 共同持有本表，
/// GLBuffer 晚于 device 析构时表仍然存活，只是 deviceAlive 已置假、
/// 登记直接丢弃（消费者已不存在）。
class GLVaoInvalidationRegistry {
public:
    /// GLBuffer 析构时调用（只入队，不碰 GL）。
    void notifyBufferDeleted(unsigned int bufferId);
    /// device 析构时调用：此后的登记直接丢弃。
    void markDeviceDead();
    /// device 在 submit 开头取走待清理的 buffer id 列表。
    std::vector<unsigned int> takePendingDeletedBuffers();

private:
    std::mutex mutex_;
    std::vector<unsigned int> pendingDeletedBuffers_;
    bool deviceAlive_ = true;
};

/// OpenGL ES 3.0 渲染后端。
/// 假设调用者已创建并激活 EGL context。
class RenderDeviceGLES : public RenderDevice {
public:
    RenderDeviceGLES();
    ~RenderDeviceGLES() override;

    // ---- 能力查询 ----
    Backend backendType() const override { return Backend::OpenGLES; }
    int maxTextureSize() const override;
    int maxDrawBuffers() const override;
    bool supportsFloatTextures() const override;
    bool supportsInstancing() const override;
    std::string rendererString() const override;

    // ---- 资源创建 ----
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override;
    bool updateTextureRegion(Texture* texture,
                             int x,
                             int y,
                             int width,
                             int height,
                             const uint8_t* data,
                             size_t rowBytes) override;
    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override;
    bool updateBuffer(Buffer* buffer,
                      size_t offset,
                      const void* data,
                      size_t size) override;
    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc& desc) override;
    std::unique_ptr<Framebuffer> createFramebuffer(const FramebufferDesc& desc) override;

    // ---- 帧操作 ----
    void setClearColor(float r, float g, float b, float a) override;
    void beginFrame() override;
    bool beginPass(Framebuffer* target) override;
    void endPass() override;
    void submit(const RenderCommandList& commands) override;
    void endFrame() override;

    // ---- 生命周期 ----
    void onSurfaceCreated() override;
    void onSurfaceChanged(int width, int height) override;
    void onSurfaceDestroyed() override;

private:
    // ---- VAO 缓存 ----
    // key =（vertexBuffer id, indexBuffer id, instanceBuffer id, 布局种类,
    // stride）。GL_ELEMENT_ARRAY_BUFFER 绑定属于 VAO 状态，index buffer
    // 必须进 key 和 VAO 记录。布局分派只在 VAO 首次创建时录制一次（见
    // recordVaoLayout），之后同 key 命令一次 glBindVertexArray 即完成，
    // 不再每 draw 全量重发 glVertexAttribPointer/glVertexAttribDivisor。
    enum class VertexLayoutKind : unsigned char {
        Surface32,          ///< 32B：POSITION(12)+NORMAL(12)+TEXCOORD(8)
        Surface32Instanced, ///< 32B + 7 条 instance 矩阵属性（attrib 3-9）
        TerrainCompact28,   ///< 28B：POSITION(f32)+NORMAL(snorm16)+packed TEXCOORD_0/1(unorm16)
        Gltf120,            ///< 120B glTF：POSITION/NORMAL + packed TEXCOORD + COLOR/TANGENT
        Gltf120Instanced,   ///< 120B glTF + 7 条 instance 矩阵属性
        Terrain20,          ///< 20B：pos(12)+uv(8)，normal 由 shader 计算
        SimpleStride,       ///< 单属性显式 stride（8=vec2、12=vec3，其余按 vec3）
    };
    struct VaoKey {
        unsigned int vertexBuffer = 0;
        unsigned int indexBuffer = 0;    ///< 0 = 无索引（glDrawArrays 路径）
        unsigned int instanceBuffer = 0; ///< 仅 *Instanced 布局非 0
        VertexLayoutKind layout = VertexLayoutKind::Surface32;
        unsigned int vertexStride = 0;
        bool operator==(const VaoKey& o) const {
            return vertexBuffer == o.vertexBuffer &&
                   indexBuffer == o.indexBuffer &&
                   instanceBuffer == o.instanceBuffer &&
                   layout == o.layout &&
                   vertexStride == o.vertexStride;
        }
    };
    struct VaoKeyHash {
        size_t operator()(const VaoKey& key) const;
    };
    struct VaoEntry {
        unsigned int vao = 0;
        uint64_t lastUse = 0;  ///< LRU 逐出用的单调使用序号
    };

    /// 查缓存或新建并录制 VAO；命中只更新 LRU 序号，新建后该 VAO 保持绑定。
    unsigned int acquireVao(const VaoKey& key);
    /// 在新建 VAO 的绑定状态下按布局种类录制顶点属性 + element buffer。
    void recordVaoLayout(const VaoKey& key);
    /// 消化 GLBuffer 析构投递的失效 id，删除引用它们的 VAO（GL 线程调用）。
    void purgeVaosForDeletedBuffers();
    /// 丢弃全部 VAO 缓存。deleteGlObjects=false 用于 context 已（将）失效
    /// 的场景：旧 context 的 VAO 名字不能拿到新 context 里 glDelete。
    void dropVaoCache(bool deleteGlObjects);

    std::unordered_map<VaoKey, VaoEntry, VaoKeyHash> vaoCache_;
    uint64_t vaoUseCounter_ = 0;
    std::shared_ptr<GLVaoInvalidationRegistry> vaoRegistry_;

    int viewportWidth_ = 0;
    int viewportHeight_ = 0;
    // Sky clear color pushed by Engine each frame via setClearColor().
    // Defaults match FrameState (dark night blue) until the first update.
    float clearR_ = 0.02f;
    float clearG_ = 0.02f;
    float clearB_ = 0.08f;
    float clearA_ = 1.0f;
};

// ============================================================
// 内部 GL 资源类型
// ============================================================

class GLTexture : public Texture {
public:
    GLTexture(unsigned int id, int width, int height, size_t sizeBytes);
    ~GLTexture() override;
    int width() const override { return width_; }
    int height() const override { return height_; }
    size_t sizeBytes() const override { return sizeBytes_; }
    unsigned int glId() const { return id_; }
private:
    unsigned int id_;
    int width_, height_;
    size_t sizeBytes_ = 0;
};

/// 离屏 framebuffer:color 恒为可采样 GLTexture(生命周期归本对象)。
/// depth 二选一:默认用 renderbuffer(不需采样,省完备性开销);
/// depthSampleable 时改用可采样 GLTexture(如 aerial fog 采样视距)。
class GLFramebuffer : public Framebuffer {
public:
    GLFramebuffer(unsigned int fboId,
                  std::unique_ptr<GLTexture> color,
                  unsigned int depthRenderbufferId,
                  std::unique_ptr<GLTexture> depthTexture,
                  int width,
                  int height);
    ~GLFramebuffer() override;
    int width() const override { return width_; }
    int height() const override { return height_; }
    Texture* colorTexture() const override { return color_.get(); }
    Texture* depthTexture() const override { return depthTexture_.get(); }
    unsigned int glId() const { return fboId_; }

private:
    unsigned int fboId_;
    std::unique_ptr<GLTexture> color_;
    unsigned int depthRenderbufferId_;      // 0 = 无 renderbuffer depth
    std::unique_ptr<GLTexture> depthTexture_; // 非空 = 可采样 depth
    int width_, height_;
};

class GLBuffer : public Buffer {
public:
    GLBuffer(unsigned int id, size_t size, unsigned int target,
             std::shared_ptr<GLVaoInvalidationRegistry> vaoRegistry);
    ~GLBuffer() override;
    size_t size() const override { return size_; }
    unsigned int glId() const { return id_; }
    unsigned int target() const { return target_; }
private:
    unsigned int id_;
    size_t size_;
    unsigned int target_;  // GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER
    // 析构时向 device 登记 id，令引用本 buffer 的 VAO 失效（见
    // GLVaoInvalidationRegistry）。shared_ptr 保证登记表比 buffer 活得久。
    std::shared_ptr<GLVaoInvalidationRegistry> vaoRegistry_;
};

class GLShaderProgram : public ShaderProgram {
public:
    GLShaderProgram(unsigned int programId);
    ~GLShaderProgram() override;
    unsigned int glId() const { return id_; }

    /// 获取 uniform location（缓存）
    int uniformLocation(const std::string& name);

    /// GltfUniformBlock 描述表对应的 location 数组（与 gltfUniformTable()
    /// 逐条对齐；shader 未声明的名字为 -1）。首次调用时解析一次。
    const std::vector<int>& gltfBlockLocations();

    /// sampler uniform 是 program 持久状态，只需在首个 draw 设置一次。
    bool samplersConfigured() const { return samplersConfigured_; }
    void markSamplersConfigured() { samplersConfigured_ = true; }

private:
    unsigned int id_;
    std::unordered_map<std::string, int> uniformCache_;
    std::vector<int> gltfBlockLocations_;
    bool gltfBlockLocationsResolved_ = false;
    bool samplersConfigured_ = false;
};

} // namespace earth_engine
