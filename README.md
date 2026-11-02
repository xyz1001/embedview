# webview-adapter

这是一个使用 `webview_adapter` 命名空间的 C++17 webview 适配器接口草案。

## 当前状态

头文件仅包含声明，尚无已实现或可链接的后端：创建 `WebView` 或
`WebViewWidget` 对象会导致链接失败。这些接口目标不保证任何运行时或平台能力。
拟定的契约及限制参见 [`doc/interface_design.md`](doc/interface_design.md)；
该文档并不表示相关行为已实现。

## 头文件与目标

- `<webview_adapter/native/webview.h>`：`webview_adapter::native`。
- `<webview_adapter/qt/webview_widget.h>`：`webview_adapter::qt`，仅启用 Qt 时可用；
  它还依赖原生接口和 Qt Widgets。

需要 CMake 3.25 或更高版本和 C++17。设置 `WEBVIEW_ADAPTER_WITH_QT=ON` 可启用
Qt 目标并安装其头文件，此时需要 Qt5 Widgets。安装后的 CMake 包可通过
`find_package(webview_adapter CONFIG REQUIRED)` 加载。不启用 Qt 时，不安装 Qt
头文件和目标，安装后的 CMake 包也不会查找 Qt。

Conan 2 配方使用默认关闭的 `with_qt` 选项；启用时使用与当前调用方一致的
`qt/5.15.18`。其 CMakeDeps 包名为 `webview-adapter`。该 Conan 配方目前是头文件库包，
加入实现后必须转换为编译型库。导出或创建包时需提供包版本。Conan 和 CMake 选项
分别用于各自的工作流程；当前配方不配置或构建 CMake。

Conan Qt 组件目前没有传递 Qt Widgets 组件：其 Conan 组件名称尚未核实。在该依赖
接线完成之前，启用 Qt 的 Conan 使用方必须自行链接 `Qt5::Widgets`。

配方保留 `webview/0.12.0` 作为未来私有后端的预留依赖；公共接口草案不使用或暴露
webview 类型。在 Linux 上，webview 包本身可能需要 GTK 和 WebKitGTK。不启用 Qt 时，
配方不会添加 Qt 依赖。
