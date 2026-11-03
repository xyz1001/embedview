# embedview

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20(X11)-lightgrey.svg)]()
[![License](https://img.shields.io/badge/License-MIT-green.svg)]()

`embedview` 是一个基于轻量级 [webview](https://github.com/webview/webview) 封装的现代 C++17 窗口嵌入适配库。

与上游主要用于创建独立主窗口不同，本库专注于解决 **“在已有宿主窗口（Win32 / X11 / Qt 窗口）中嵌入 Webview 子视图”** 的场景，提供统一简洁的生命周期管理、导航控制、脚本注入以及开箱即用的 **Qt Widgets** 容器支持，避免为了轻量内嵌 Web 需求引入庞大臃肿的 CEF 或 QtWebEngine。

---

## 核心特性

- **轻量嵌入**：直接嵌入已有宿主窗口或 Qt 控件区域，无额外庞大运行时开销。
- **开箱即用的 Qt 支持**：提供 `embedview::WebViewWidget`（继承自 `QWidget`），原生嵌入 Qt 布局系统，自动同步大小与显示状态。
- **现代化 C++17 接口**：标准类型与 `std::error_code` 错误处理，无冗余平台宏泄漏。
- **灵活的页面控制**：
  - **脚本预注入**：支持在 DOM 准备阶段（`document-start`）注入预置 JavaScript。
  - **导航拦截**：支持同步审查请求 URL，实现页面跳转白名单拦截。
  - **加载通知**：精准感知页面引擎加载完成与异常状态。
  - **媒体与环境**：支持 User-Agent 自定义、上下文菜单开关、无手势自动播放配置。
- **完备的生命周期与安全防护**：
  - 异步就绪模型与幂等安全的 `Close()` 释放。
  - 针对 Linux 嵌入场景内置有界 GLib 事件泵，防止主循环卡死与递归重入。

---

## 系统与平台支持

| 平台 | 底层渲染后端 | 依赖要求 | 说明 |
| :--- | :--- | :--- | :--- |
| **Windows** | Microsoft Edge WebView2 | WebView2 运行时 | 需在 COM STA 线程中运行 |
| **Linux** | WebKitGTK | GTK 3, WebKit2GTK | 仅限 X11/xcb 环境 |

> [!NOTE]
> 当前版本暂不支持 macOS 与 Linux Wayland。

---

## 快速上手

### 1. 在 Qt 项目中嵌入（推荐）

通过 `embedview::WebViewWidget`，可以像使用普通 `QWidget` 一样将其添加到任意 Qt 布局中：

```cpp
#include <embedview/qt/webview_widget.h>
#include <QVBoxLayout>
#include <QWidget>
#include <iostream>

class MyWindow : public QWidget {
public:
    MyWindow(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        auto *browser = new embedview::WebViewWidget(this);
        layout->addWidget(browser);

        // 配置参数
        embedview::Options options;
        options.context_menu_enabled = true; // 启用右键菜单
        options.allow_autoplay = true;       // 允许无手势自动播放

        // 设置生命周期与导航回调
        embedview::Callbacks callbacks;
        callbacks.initialized = [browser](std::error_code ec) {
            if (!ec) {
                // 初始化就绪后加载页面
                browser->Navigate("https://www.example.com");
            } else {
                std::cerr << "Webview 初始化失败: " << ec.message() << std::endl;
            }
        };

        callbacks.navigation_requested = [](const std::string &url) -> bool {
            // 拦截页面导航，返回 false 可阻止跳转
            std::cout << "请求跳转: " << url << std::endl;
            return true;
        };

        callbacks.load_finished = [](std::error_code ec, const std::string &url) {
            if (!ec) {
                std::cout << "页面加载完成: " << url << std::endl;
            }
        };

        // 发起初始化
        browser->Initialize(options, std::move(callbacks));
    }
};
```

### 2. 原生窗口嵌入（Win32 / X11）

如果不使用 Qt，可直接使用原生 `embedview::WebView` 接口获取借用窗口句柄进行嵌入：

```cpp
#include <embedview/native/webview.h>
#include <iostream>

void CreateEmbeddedWebview() {
    auto view = std::make_unique<embedview::WebView>();

    embedview::Options options;
    options.document_start_script = "window.__MY_APP__ = { version: '1.0' };";

    embedview::Callbacks callbacks;
    callbacks.initialized = [&view](std::error_code ec) {
        if (ec) {
            std::cerr << "初始化失败: " << ec.message() << "\n";
            return;
        }

        // 获取底层原生窗口句柄（用于 Win32 SetParent 或 X11 XReparentWindow）
        auto handle = view->GetNativeWindow();
        if (std::holds_alternative<embedview::Win32WindowHandle>(handle)) {
            auto hwnd = reinterpret_cast<HWND>(std::get<embedview::Win32WindowHandle>(handle).value);
            // ::SetParent(hwnd, myHostHwnd);
        }

        view->Resize(800, 600);
        view->SetVisible(true);
        view->Navigate("https://www.example.com");
    };

    view->Initialize(options, std::move(callbacks));
}
```

---

## 核心 API 说明

所有 C++ 接口均位于 `namespace embedview`（同时提供 `namespace webview` 别名兼容）。

### `embedview::Options` 配置项

| 字段 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `document_start_script` | `std::string` | `""` | 页面文档开始加载前注入执行的 JavaScript 代码。 |
| `user_agent` | `std::optional<std::string>` | `std::nullopt` | 自定义 User-Agent 字符串。 |
| `context_menu_enabled` | `bool` | `true` | 是否启用浏览器默认右键上下文菜单。 |
| `allow_autoplay` | `bool` | `false` | 是否放宽音视频无手势自动播放限制。 |

### `embedview::Callbacks` 事件回调

| 回调名 | 签名 | 说明 |
| :--- | :--- | :--- |
| `initialized` | `void(std::error_code)` | 初始化结果通知。为 `0` 时表示引擎就绪，方可进行 `Navigate`。 |
| `navigation_requested` | `bool(const std::string &url)` | 导航请求拦截。返回 `true` 允许加载，返回 `false` 阻止。 |
| `load_finished` | `void(std::error_code, const std::string &url)` | 顶层页面加载完成通知。成功时错误码为 `0`。 |
| `close_requested` | `void()` | 用户端请求关闭窗口（如有配置可拦截）。 |
| `closed` | `void()` | 内部底层安全清理完成时触发。 |

### 接口方法对比

| 方法 | `WebView` (原生) | `WebViewWidget` (Qt) | 说明 |
| :--- | :--- | :--- | :--- |
| `Initialize(options, callbacks)` | 支持 | 支持 | 异步开启初始化流程。 |
| `Navigate(url)` | 支持 | 支持 | 加载目标 URL（需在就绪后调用）。 |
| `IsReady()` | 支持 | 支持 | 查询底层引擎是否已就绪。 |
| `Resize(width, height)` | 支持 | 自动同步 | 调整渲染区域大小（原生像素单位）。Qt 控件自动处理。 |
| `SetVisible(bool)` | 支持 | 自动同步 | 设置显示/隐藏。Qt 控件由 `show()` / `hide()` 驱动。 |
| `Close() / CloseBrowser()` | `Close()` | `CloseBrowser()` | 主动关闭并释放浏览器内核资源。 |
| `GetNativeWindow()` | 支持 | 封装于内部 | 获取 `NativeWindowHandle` 变体类型。 |

---

## 集成方式

### 1. 使用 Conan 2.x

在项目的 `conanfile.py` 或 `conanfile.txt` 中引入：

```python
def requirements(self):
    # 引入 embedview
    self.requires("embedview/0.1.2")

def configure(self):
    # 如需启用 Qt 封装
    self.options["embedview"].with_qt = True
```

配置选项：
- `shared`: `True` / `False`（默认 `False`，静态库）
- `with_qt`: `True` / `False`（默认 `False`，开启后构建 `embedview::qt` 并依赖 Qt 5.15）

命令行安装示例：

```bash
# 默认构建（纯原生，静态库）
conan install . --build=missing

# 启用 Qt 控件支持
conan install . -o "embedview/*:with_qt=True" --build=missing
```

### 2. CMakeLists.txt 集成

安装或生成包后，在 `CMakeLists.txt` 中链接对应的目标：

```cmake
find_package(embedview CONFIG REQUIRED)

# 场景一：仅使用原生接口
add_executable(my_native_app main.cpp)
target_link_libraries(my_native_app PRIVATE embedview::native)

# 场景二：使用 Qt 控件封装
add_executable(my_qt_app main.cpp)
target_link_libraries(my_qt_app PRIVATE embedview::qt)
```

> [!TIP]
> 动态库模式下，`EMBEDVIEW_*_DLL` 等导入导出宏已由 CMake Target 自动传递，消费端无需手动定义。

---

## 注意事项

1. **线程约束**：
   - 所有的创建、调用、查询与析构**必须在 UI 主线程进行**（Windows 上为 COM STA 线程，Linux 上为 GTK 主线程）。
   - 切勿在后台工作线程中跨线程调用 `WebView` 相关方法。
2. **Linux 运行环境**：
   - Linux 宿主系统需要安装 `gtk3` 和 `webkit2gtk`（如 Ubuntu: `libgtk-3-dev`, `libwebkit2gtk-4.0-dev` 或 `4.1`）。
   - Linux Qt 嵌入运行在 X11 环境下，内部通过有界定时器驱动 GLib 消息，无需宿主手动调用 `ProcessEvents()`。
3. **资源释放**：
   - 窗口销毁前建议调用 `Close()` 或 `CloseBrowser()`，确保底层浏览器内核有序退出。
