# webview-adapter

这是一个使用 `webview` 命名空间的 C++17 webview 适配器。

## 当前状态

首轮实现以 Windows 和 Linux X11 为目标，Qt 封装可选；代码正在合并，仍有审查修复，
目前尚未构建或运行验证，
原生及 Qt 后端均需进一步验证，不能视为运行能力保证。没有测试，也不提供播放或性能
保证。契约与限制参见 [`doc/interface_design.md`](doc/interface_design.md)。Linux 的新窗口
导航经通用导航回调允许后在当前页面打开；Windows 阻止新窗口。

## 头文件与目标

- `<webview_adapter/native/webview.h>`：`webview` 原生接口。
- `<webview_adapter/qt/webview_widget.h>`：`webview::WebViewWidget`，仅启用 Qt 时可用；
  它还依赖原生接口和 Qt Widgets。

需要 CMake 3.25 或更高版本和 C++17。设置 `WEBVIEW_ADAPTER_WITH_QT=ON` 可启用
Qt 目标并安装其头文件，此时需要 Qt5 Widgets；该选项默认关闭。安装后的 CMake 包可通过
`find_package(webview_adapter CONFIG REQUIRED)` 加载。不启用 Qt 时，不安装 Qt
头文件和目标，安装后的 CMake 包也不会查找 Qt。

首版仅支持静态库，`BUILD_SHARED_LIBS=ON` 会被拒绝。Conan 2 配方使用默认关闭的
`with_qt` 选项；启用时使用 `qt/5.15.18`，并传递 Qt Widgets 组件依赖。其 CMakeDeps
包名为 `webview-adapter`，版本为 `0.1.2`。Conan 和 CMake 选项分别用于各自的
工作流程；配方通过 CMake 构建和安装。
CMake 导入目标命名空间仍为 `webview_adapter`，目标为
`webview_adapter::native` 和可选的 `webview_adapter::qt`。

原生后端使用 `webview/0.12.0`；公共接口不暴露 webview 类型。Linux 还需要 GTK 和
WebKitGTK；系统 WebKitGTK 版本未锁定，由 pkg-config 实际解析结果决定。不启用 Qt 时，
配方不会添加 Qt 依赖。

导航回调在 Windows 上仅覆盖顶层导航，在 Linux 上覆盖顶层及子框架导航。上游
webview 0.12.0 的 `apply_webkit_dmabuf_workaround` 在特定 Linux X11 NVIDIA 场景下会
设置进程级环境变量 `WEBKIT_DISABLE_DMABUF_RENDERER=1`。本库不会偷偷切换 `GDK_BACKEND`。

设置 `options.allow_autoplay = true` 可尽量允许无用户手势播放；默认 `false` 不主动
放宽平台限制，但不保证阻止静音媒体或调用方环境允许的自动播放。启用时 Windows 在创建 WebView2 时临时设置进程级
`WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS`，调用返回后恢复；已有的 `--autoplay-policy=`
无论宽严均保持不变。这是进程级行为，不保证实例隔离。Linux 显式取消媒体播放手势
要求，关闭时则明确要求手势。网站策略、媒体解码或 DRM 仍可能阻止实际播放。

`initialized` 的零错误码表示初始化成功，但不表示页面加载或协议就绪。
`load_finished` 接收错误码和 URL：零表示成功，失败保留原错误码，取消可能报告
`std::errc::operation_canceled`。`navigation_requested` 接收 URL，返回 `true` 允许、
`false` 取消；未设置回调时允许。使用 `IsReady()` 查询就绪，使用 `Resize` 设置
原生客户区尺寸；`ProcessEvents()` 使用固定的有界预算。
