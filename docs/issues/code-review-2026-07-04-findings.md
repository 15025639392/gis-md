# Code Review Findings（2026-07-04，范围 9b4006569..HEAD，17 commits）

本地 max-effort review（10 finder × 1-vote 验证 × sweep）。15 条入榜，正确性优先。
勾选 = 已修复。

## CONFIRMED 正确性（合并 main 前建议全修）

- [x] **1. framePending_ 线程重启不复位 → 旋转/后台恢复后永久冻屏**（8375a41ac；
  注：demo manifest 有 configChanges，旋转不重建 surface，实际触发面=后台恢复；
  竞态窗口 <1 vsync，真机 5 轮 BACK-finish 未自然复现，修复依据代码推理+压测无回归）
  `scaffold/examples/android/MinimalGlobe/GLESView.cpp:276` 区域。
  stop() 时挂起的 AChoreographer 回调随旧线程死亡，flag 留 true；start() 只重置
  tasks_/running_/paused_。新线程 postFrameIfNeeded() 与 setPaused(false) 全部早退，
  无任何路径再注册帧回调。修复：start() 或 threadMain 入口 `framePending_ = false;`。

- [x] **2. raster 节流名额双重释放（abandon vs 完成回调竞态）**（14a14e0b7，
  共享 atomic 令牌 + 9 处释放点全过 exchange；回归测试确定性复现已双向验证）
  `scaffold/src/earth_engine/providers/RasterOverlayTileProvider.cpp:2645`。
  finishOneSource 置 completed=true 后、回调 erase 条目前的窗口内，
  abandonActiveMappedSourceSets（setReady/dtor 触发）移出条目并无条件递减；
  随后 onSuccess/onFailure 六个分支再次无条件递减（不检查 erase 结果、无 completed
  exchange 决定归属）→ 一次 fetch_add 两次释放，CAS 在 0 处钳制静默偷走其他在途
  名额。dtor 变体同样（:2358 + finishAbandonedTileLoad :2203）。
  修复：以 completed/erase 结果的原子交换决定唯一递减方。

- [x] **3. golden 对拍套件 skip-as-pass 退化**（6f258d55e，SKIP→ASSERT；
  负向验证：移走 golden 目录 4 测试 FAIL）
  `scaffold/tests/unit/tiling/test_selector_cesium_golden_diff.cpp:568` +
  `scaffold/tests/CMakeLists.txt:676/679`。
  golden 已入库后，宏拼错/tools 目录移动/golden 误删 → 4 测试全 SKIP，普通 add_test
  无 SKIP_REGULAR_EXPRESSION，CTest 报绿。修复：缺 golden 改 ASSERT 失败（或
  add_test 加 skip 检测属性）。

- [x] **4. IosHttpRequest 缺析构 cancel（RAII 契约破坏）**（8a6f708dd）
  `scaffold/src/earth_engine/platform/ios/IosPlatformBridge.mm:20`。
  PlatformBridge.h:108 文档契约"析构时取消"；Curl 侧 ~RequestHandle{cancel()} 已实现；
  iOS 句柄销毁后 NSURLSessionDataTask 继续下载到完成，QM provider 析构假设失效。
  修复：一行 `~IosHttpRequest() override { cancel(); }`。

- [x] **5. IosPlatformBridge 全文件无 @autoreleasepool**（8a6f708dd，六个
  创建 ObjC 对象的方法全包；iphoneos SDK syntax-only 编译通过）
  `scaffold/src/earth_engine/platform/ios/IosPlatformBridge.mm:126`（decodeImage）
  及 get()/getToken/cacheDirectory。被删的 demo 桥有 pool，移植时丢失。
  AsyncSystem 池线程（裸 std::thread 永不退出）每瓦片解码累积 autoreleased 对象。
  修复：四个方法体包 @autoreleasepool。

- [x] **6. ALooper_wake 跨线程 UAF（无 ALooper_acquire）**（7f3510fd7，
  改互斥保护 looper 生命周期而非 acquire/release——见 commit body）
  `scaffold/examples/android/MinimalGlobe/GLESView.cpp:266`。
  渲染线程可自行观察 running_==false 退出并释放 TLS looper；调用方 wake() 加载
  looper_ 非空后被抢占 → 对已释放 looper 调 ALooper_wake（eventfd 可能已复用）。
  全仓无 ALooper_acquire。窗口窄（surface 销毁期），按 NDK 契约构造性成立。
  修复：wake 侧 acquire/release 或互斥保护 looper 生命周期。

## PLAUSIBLE 正确性（择机修）

- [ ] **7. iOS 非法 URL 回调派主队列 → SDK 主线程用法下 20s 冻结/销毁死锁**
  `IosPlatformBridge.mm:64`。唯一主队列完成路径；fetchBlocking cv 等 20s、
  markDestroyingCancelAndWait 无限等。demo 配置不可达。修复：改后台队列。

- [ ] **8. >512 交互上传门在错误层 + pendingUploads 无上限**
  `RasterOverlayTileProvider.cpp:3470/:37/:3195`。大图源手势期间零上传且白耗
  budget lane 名额；队列仅消费时修剪孤儿、无容量上限。当前 256px 高德源不触发。
  修复：尺寸折算 estimatedCostUnits 交给 budget planner + 跳过项 age-out。

- [ ] **9. gAppContext DeleteGlobalRef 与渲染线程读竞态（多视图潜伏）**
  `GLESView.cpp:357`。demo 单视图不可达（且第二实例会先 std::terminate 于
  gRenderThread 单例）。修复方向：单实例显式断言/文档；SDK 化时按实例持有。

## 清理/分层（CONFIRMED，非阻塞）

- [ ] **10. iOS 桥与生产 macOS 桥近逐行复制且已漂移**（IosPlatformBridge.mm:20 ↔
  platform/macos/MacPlatformBridge.mm；Mac 侧顺带发现 CGBitmapContextCreate 不判空
  的潜伏崩溃 + headers 被静默丢弃）→ 提取共享 Apple 基类，deviceInfo 每平台覆写。
- [ ] **11. SharedMetadataCompletionGuard ≅ ContentCompletionGuard 双写**
  （QuantizedMeshTerrainProvider.cpp:2011 ↔ TileLoadRequestDispatcher.h:143，自注释
  承认"同构"；XYZImageryProvider.cpp:445 回调仍裸奔）→ core/async 提取
  CallOnceGuard<Args...> 模板。
- [ ] **12. ReqDrop 9 计数器 × 6 处手写镜像**（SceneTilesetDiagnostics.cpp:420 等）
  → 内嵌 TileLoadRequestOutcome + operator+=（注意 int/size_t 与 %d 需同步调整）。
- [ ] **13. logcat 映射双写**（AndroidPlatformBridge.cpp:400 ↔ debug/PlatformLog.h:34）
  → 一行 `platformLog(level, tag.c_str(), "%s", message.c_str())`（防格式串注入必须
  "%s" 包装；platformLog 有 1024 截断的微小行为差）。
- [ ] **14. TileLoadScheduler.h:98 死分支**（needsRasterDetailUpsample && !src 被后随
  !src 完全包含，分支体相同）→ 删首分支与局部变量，行为等价。
- [ ] **15. SDK 层 native 契约实现在 demo TU**（GLESView.cpp:211；全仓仅 demo 定义
  Java_com_earthengine_sdk_GLESView_* 符号；gEngine/gRenderThread 等进程级单例使
  第二视图结构性不可能）→ 渲染线程+生命周期下沉 earthsdk 自有 native 库。

## 已证伪（勿重报）

- iOS 传输错误→status 200：空 body 使所有消费者走同一失败分支（仅共享 metadata
  统计计数 cosmetic 差异）。
- runSync broken-promise 假成功：shared_ptr 双持有使 promise 存活，超时返回 false。
- metadata completeSharedRequest TOCTOU UAF：成员解引用严格先于闸门触发 +
  markDestroyingCancelAndWait 覆盖；测试路径全时序化。潜伏契约依赖已记录。
- postInputEvent 每事件堆分配：µs 级，gate 方案反增跨线程竞态面，不动。

## 环境备注

- commit 762f65999 用了 `docs:` 前缀（全局规范 type 列表外）；AsyncSystem.h 顺手删过
  全工程零调用的 Future::then()（commit body 有披露）——均为轻微规范项，随下次提交
  规范即可。
