# EmbedView 接口设计

首轮实现以 Windows 和 Linux X11 为目标；原生与 Qt 后端均遵循本设计契约。
不支持 Wayland 和 macOS。
原生接口仅使用 C++17 标准类型；可选的 QWidget 封装依赖原生接口，反之则不依赖。
原生和 Qt C++ 接口位于 `embedview` 命名空间（同时提供 `webview` 别名兼容）；CMake 导入目标
使用 `embedview::native` 和 `embedview::qt`。
接口不包含 dispatch、run、bridge 或业务 API。

## 构建与部署

默认构建静态库；纯 CMake 设置 `BUILD_SHARED_LIBS=ON` 可选择共享库。
Conan 2 的 `shared` 默认 `False`，可独立于默认关闭的 `with_qt` 选项启用：

```sh
conan install . -o "embedview/*:shared=False" --build=missing
conan install . -o "embedview/*:shared=True" --build=missing
```

以上使用默认 profile；需要 Qt 时另加 `-o "embedview/*:with_qt=True"`。
纯 CMake 安装目标和 Conan CMakeDeps 组件目标会自动传递共享库消费端导入宏，
无需调用方手动配置导出宏。原生和 Qt 共享库彼此独立，调用方与共享库应匹配
编译器和 CRT。Windows 的导入库在 `lib`、DLL 在 `bin`，部署时需要提供 DLL；
Linux 的 `.so` 在 `lib`，运行时需配置动态加载器搜索路径。

## 线程与初始化

构造既不初始化浏览器，也不处理消息。创建线程即 UI 线程：Windows 要求 COM STA，
Linux 要求 GTK 主 UI 线程。所有操作、查询、回调和析构都必须在该线程进行。不提供
隐藏线程或跨线程派发；应用程序需自行调度跨线程工作。
返回错误码的操作在错误线程返回 `std::errc::operation_not_permitted`；错误线程调用
`IsReady()` 返回 `false`，`GetNativeWindow()` 返回 `monostate`，`Close()` 不做操作。
错误线程析构无法安全释放 UI 资源，因此终止进程。后端完成通知也必须在创建线程执行。

`Initialize` 返回错误表示请求被拒绝，不调用 `initialized`；返回零表示请求已接受，
即使初始化失败也返回零，并通过 `initialized(error)` 报告。除关闭或析构撤销外，
完成回调在 `Initialize` 返回前恰好执行一次，可能重入，返回后不得补发。就绪前必须确认
document-start 脚本已注册、user-agent 和菜单配置已完成，且原生句柄已就绪；仅提交
脚本注册请求并不足够。不支持重复初始化；就绪前导航会失败。通用错误通过
`std::error_code` 使用 `std::errc`，平台错误保留原有类别。不定义额外的错误枚举。
`initialized` 的零错误码表示原生初始化以及 Qt 包装（如适用）成功，不等待页面加载、
媒体或协议就绪；没有额外浏览器启动或网页等待。Windows 上游 `webview_create` 内部
消息泵可能消费 `WM_QUIT`，且创建期间不能立即中断；本库在创建期间保护状态，返回后
检查关闭。脚本注册等待期间收到 `WM_QUIT` 时保留并重投退出码，报告初始化取消。

## 配置、导航与加载

`document_start_script` 在新顶层文档和子框架文档的 document-start 阶段注册到页面
主世界。不包括 Worker 和已有文档，也不保证特殊 URL 的行为一致。无法满足脚本注册
要求的实现不得静默忽略。可选 user agent 不保证 User-Agent Client Hints 相应变化。
上下文菜单选项仅控制浏览器的默认上下文菜单。

设置 `options.allow_autoplay = true` 可尽量允许无用户手势播放；默认 `false` 不主动
放宽平台限制，但不保证阻止静音媒体或调用方环境允许的自动播放。启用时 Windows 仅在创建 WebView2 期间临时设置进程级
`WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` 并在调用返回后恢复；已有的
`--autoplay-policy=` 无论宽严均保持不变。该设置不是实例级保证。Linux 显式取消媒体
播放手势要求，关闭时则明确要求手势。网站策略、媒体解码或 DRM 仍可能阻止实际播放。

`navigation_requested` 在 Windows 上仅涉及顶层导航，在 Linux 上覆盖顶层及子框架
导航（沿用旧实现），不是通用网络拦截。回调必须快速、同步、
不抛出异常，且不得进入模态循环或以其他方式引发模态重入。Linux 的新窗口导航经此
回调允许后在当前页面打开；取消则不加载。回调接收 URL，返回 `true` 允许、`false`
取消；未设置回调时允许导航。回调期间关闭也会取消导航。Windows 阻止新窗口。

`load_finished` 报告顶层引擎加载结果，而不是 HTTP 成功或协议就绪。它与 `Navigate`
调用不一定一一对应；成功提交导航也不表示加载完成。回调接收错误码和 URL，成功时
错误码为零，失败时保留原错误码的类别和值；取消可能报告
`std::errc::operation_canceled`。

`close_requested` 仅通知关闭请求，不表示已开始关闭。有处理函数时由所有者决定是否
关闭，也可以忽略；没有处理函数时默认关闭。关闭后撤销该回调，析构不调用它。
Qt 封装负责拦截并处理该请求。

## 生命周期与事件处理

`Close` 是幂等的，可以在初始化期间或回调中调用。逻辑失效与物理释放分离：借用的
原生句柄立即失效，调用方绝不能释放它。尚未开始的回调会被撤销；正在执行的回调可以
返回。`closed` 在清理安全完成后发送一次，但不保证浏览器进程已经退出。初始化期间
关闭会取消初始化，不会补发 `initialized` 回调。析构不调用用户回调。异步清理需要
事件循环持续运行。
`closed` 还须等待活动的公共调用和用户回调全部退出；析构可撤销尚未发送的 `closed`。

初始化可能处理消息并重入删除公共对象。实现必须在此过程中保护内部状态；回调中使用
未经保护的裸 `this` 并不足够。

`ProcessEvents` 是线程级、非阻塞、非递归的事件泵。其迭代次数限制和软时间预算不
提供硬实时保证；固定预算为最多 16 次迭代和 2 毫秒软时间限制，它也可能处理其他对象的事件。不提供独立的 `run` 操作。已有 Windows
消息循环无需额外的事件泵。

`Resize` 使用原生客户区单位，而非 Qt 逻辑像素。原生句柄有明确类型且为借用句柄；
没有有效句柄时使用 `monostate`。Qt 封装通过内部桥接同步边界，避免重复进行 DPI 转换。

## Qt 封装

控件不会自动初始化。只有原生初始化和 Qt 封装均成功后，其 `initialized` 回调才报告
成功。它不暴露对原生对象的可变访问。隐藏不会销毁浏览器。控件关闭被接受时会关闭
浏览器，但不会设置 `WA_DeleteOnClose`。

显示时立即同步边界，并在下一轮事件循环再次同步；延迟工作必须绑定 QObject 的
生命周期。Qt 容器与原生句柄的所有权及销毁顺序需要在后端验证；不承诺未经验证的
分离方法。

在 Linux 上，Qt 嵌入仅限 xcb/X11，且仍需要 GTK。共享的有界 GLib 事件泵必须在初始化
之前启用，并持续运行到清理安全完成为止。不得使用无界的 `while pending` 循环，也
不得通过全局环境变量静默更改平台后端。

Linux 系统 WebKitGTK 的版本未锁定，由 pkg-config 的实际解析结果决定。上游 webview
0.12.0 的 `apply_webkit_dmabuf_workaround` 在特定 Linux X11 NVIDIA 场景下会设置
进程级环境变量 `WEBKIT_DISABLE_DMABUF_RENDERER=1`。本库不会偷偷切换 `GDK_BACKEND`。
