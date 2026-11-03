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

默认构建静态库；纯 CMake 可设置 `BUILD_SHARED_LIBS=ON` 构建共享库。Conan 2 配方的
`shared` 默认 `False`，`with_qt` 独立且默认 `False`；启用 Qt 时使用 `qt/5.15.18`，
并传递 Qt Widgets 组件依赖。例如，使用默认 profile 在项目目录执行：

```sh
conan install . -o "webview-adapter/*:shared=False" --build=missing
conan install . -o "webview-adapter/*:shared=True" --build=missing
```

如需 Qt，另加 `-o "webview-adapter/*:with_qt=True"`。Conan CMakeDeps 包名为
`webview-adapter`，版本为 `0.1.2`；配方通过 CMake 构建和安装。纯 CMake 安装包及
Conan CMakeDeps 的导入目标均为 `webview_adapter::native` 和可选的
`webview_adapter::qt`，共享库的消费端导入宏由目标自动传递，无需手动配置导出宏。
这与 C++ 接口使用的 `webview` 命名空间不同。

共享构建中原生库和 Qt 库各自独立；Windows 下分别生成 DLL 和 `lib` 导入库，部署时
需要提供 DLL（Conan 包的 `bin` 目录）。Linux 共享库位于 `lib`，运行时需确保动态
加载器可找到 `.so`。调用方与共享库需要匹配的编译器和 CRT；Linux 共享构建尚未验证。

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
