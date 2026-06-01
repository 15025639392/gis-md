# 安全架构

地球引擎是一个处理多源地理数据、用户编辑内容、网络请求和 GPU 渲染的移动端 C++ 应用。AI 在实现或修改任何涉及数据输入、网络、文件、渲染或身份认证的功能时，必须先阅读本文件。

## 安全原则

- **最小权限**：引擎核心只请求完成任务所必需的系统权限（网络、存储、定位仅按需）。
- **数据隔离**：不同 Provider 的数据不能互相污染缓存或状态。
- **输入验证**：所有外部数据（网络响应、文件、用户输入、GeoJSON）在使用前必须验证合法性。
- **安全存储**：API key、token、用户凭证存储在平台安全区域（iOS Keychain、Android Keystore），不在代码或日志中明文出现。
- **深度防御**：不要依赖单一安全层；每个数据入口点都应有独立验证。

## 威胁模型

### 攻击面

| 攻击面 | 风险 | 缓解措施 |
|--------|------|----------|
| 恶意瓦片图片（PNG/JPEG 炸弹） | 内存耗尽、解码器崩溃 | 限制最大图片尺寸（≤ 4096 px）、最大文件大小（≤ 10 MB）、解码超时（≤ 2 秒） |
| 恶意 GeoJSON（自相交、巨大坐标、无效几何） | 渲染崩溃、CPU 耗尽 | 几何合法性验证、feature 数量上限、坐标范围检查 |
| 恶意 glTF/3D Tiles（递归引用、巨大 mesh） | GPU 资源耗尽、解析崩溃 | 限制最大面数、最大纹理数、解析超时；不加载来自不可信源的模型 |
| API key 泄露 | 配额滥用、费用损失 | Key 存储在 Keychain/Keystore，不在日志/错误消息/URL 模板明文输出 |
| 中间人攻击（HTTP 降级） | 数据篡改、瓦片注入 | 所有生产网络请求强制 HTTPS；证书固定用于核心 Provider |
| 文件路径遍历 | 沙箱逃逸、数据泄露 | 所有文件路径从缓存目录解析，使用 canonical path，拒绝 `../` |
| SQL 注入（如果使用 SQLite 存储） | 数据泄露、缓存破坏 | 使用参数化查询，不拼接 SQL 字符串 |
| 第三方原生库漏洞 | 代码执行 | 通过 vcpkg 固定库版本；定期扫描 CVE；最小化 JNI/ObjC 桥接暴露面 |
| 日志泄露 | 用户数据/坐标/API key 暴露 | 生产构建中禁用 debug 日志；PII 永远不入日志 |

## 网络请求安全

### 传输层

- 所有瓦片请求、API 请求、tileset 请求必须使用 HTTPS。
- PlatformBridge 在 iOS 上使用 App Transport Security (ATS)，默认强制 HTTPS。
- Android 在网络安全配置中设置 `cleartextTrafficPermitted="false"`。
- 对于关键 Provider（自营 API），实施证书固定（certificate pinning）。

### 请求验证

- URL 白名单：Provider 只能请求其配置中声明的域名和 URL 模板。
- 响应大小限制：单个响应 ≤ 10 MB（瓦片图片通常 < 500 KB，异常大的响应可能为攻击）。
- 响应内容类型验证：图片瓦片必须是 `image/png`、`image/jpeg`、`image/webp`；JSON 响应必须是 `application/json`。

### Token 管理

```cpp
// Token 通过 PlatformBridge 注入，不硬编码在 Provider 中
class PlatformBridge {
public:
    virtual std::string getToken(const std::string& providerId) const = 0;
};
```

- iOS: Token 存储在 Keychain (`SecItemAdd/SecItemCopyMatching`)。
- Android: Token 存储在 EncryptedSharedPreferences 或 Android Keystore。
- Token 永不在日志中输出（包括 `__android_log_print` 和 `os_log`）。
- Token 过期时，Provider 进入 paused 状态，通过 PlatformBridge 触发用户重新认证。

## 数据输入验证

### 图片瓦片

- 解码前检查文件头魔数（PNG: `\x89PNG`、JPEG: `\xFF\xD8\xFF`、WebP: `RIFF....WEBP`）。
- 解码器超时 2 秒（可通过 `CancellationToken` 中止）。
- 最大解码后像素尺寸：4096 × 4096（1 亿像素）。
- 拒绝渐进式 JPEG 或动画 PNG（非瓦片常规格式）。
- stb_image 回退解码器在失败时不应崩溃（使用 `stbi_failure_reason()`）。

### GeoJSON

- 文件最大大小：10 MB（约等于 10 万个简单 feature 的 GeoJSON）。
- Feature 数量上限：5 万个/图层（可配置）。
- 坐标范围验证：经度 [-180, 180]、纬度 [-90, 90]、高度 [-500, 9000] 米（可配置）。
- 几何合法性：自相交 polygon 应标记为 `DataQualityError`，不直接渲染。
- Ring orientation：由库自动处理或要求 clockwise outer / counter-clockwise holes。
- 禁止 `GeometryCollection` 中包含混合维度（如 Point + Polygon 混在同一 collection）。

### glTF / 3D Tiles

- glTF 文件最大大小：50 MB（含内嵌纹理）。
- 单 mesh 最大顶点数：100 万。
- 单 tileset 最大同时可见 content 数：可配置（默认 500）。
- 拒绝 `external` buffer 指向非白名单域名的 URL。
- 深递归 tileset：限制最大遍历深度（默认 20 层）。

### 用户编辑数据

- 用户绘制的点/线/面：坐标范围同上。
- polygon 顶点数上限：1 万（可配置）。
- 标注文本：最大 500 字符，过滤控制字符（U+0000-U+001F 除 `\n`）。

## 文件系统安全

### 路径约束

所有引擎文件操作必须在以下目录内：

- `cacheDirectory()`：可清理的临时缓存（瓦片、解码后的纹理等）。
- `documentsDirectory()`：持久化数据（离线包、用户编辑、书签）。
- 不访问这两个目录以外的任意路径。

路径规范化：

```cpp
std::string safePath(const std::string& base, const std::string& relative) {
    // 拒绝包含 "../" 或 "..\\" 的相对路径
    // 最终路径必须在 base 前缀内
}
```

### SQLite 缓存

如果使用 SQLite 存储瓦片元数据或离线包索引：

- 使用参数化查询，永不拼接 SQL 字符串。
- 限制单次查询返回行数（默认 1000 行）。
- 数据库文件存储在 `cacheDirectory()`。

## 渲染安全

### Shader 安全

- 动态 shader 编译：仅允许从项目认可的 shader 源编译。不加载、编译或执行来自网络或用户数据的 shader。
- shader 编译超时：Metal/GL ES 的 shader 编译通常 < 100 ms。超过 5 秒视为失败。
- 拒绝包含 `while(true)` 或无限循环结构的自定义 shader 源。

### GPU 资源限制

- 单个纹理最大尺寸：由 `RenderDevice::maxTextureSize()` 运行时查询，不使用硬编码。
- 单个 draw call 最大顶点数：100 万。
- 单帧最大纹理创建数：10（防止恶意内容导致 GPU 内存泄漏）。
- 渲染超时：单帧 GPU 提交超过 2 秒时，记录 warning 并触发帧降级。

## 隐私

### 用户数据

- 用户坐标（当前位置、书签、编辑点）：存储在 `documentsDirectory()`，不自动上传。
- 分析/遥测：默认关闭，需要用户明确 opt-in。
- attribution 日志和版权水印不能泄露其他用户的坐标数据。

### 定位权限

- 如果引擎需要用户当前位置（例如 `flyTo` 当前地点）：
  - iOS: 请求 `NSLocationWhenInUseUsageDescription`。
  - Android: 请求 `ACCESS_FINE_LOCATION` 或 `ACCESS_COARSE_LOCATION`。
- 定位仅在用户触发操作时使用（如点击"我的位置"按钮），不持续跟踪。

## 构建安全

- vcpkg 依赖版本锁定在 `vcpkg.json` 中。
- C++ 编译器标志：`-fstack-protector-strong`、`-D_FORTIFY_SOURCE=2`（Android NDK）、`-fPIE`（位置无关可执行文件）。
- iOS: 启用 ARC (Automatic Reference Counting)，Objective-C++ 桥接代码不手动管理内存。
- 发布构建：禁用断言（`NDEBUG`）、strip 符号、启用 LTO。

## 验收清单

- 所有 Provider 请求使用 HTTPS（mitmproxy 测试验证）。
- 恶意 GeoJSON（自相交、巨大坐标、递归 geometry）不会导致崩溃。
- 超规格大图片（> 4096 px）被拦截并记录错误。
- API token 不在日志中明文出现。
- 文件操作限制在 `cacheDirectory()` 和 `documentsDirectory()` 内。
- 快速重复提交无效数据不会导致 OOM 或拒绝服务。
- 依赖库版本固定，可通过 `vcpkg.json` 复现构建。
