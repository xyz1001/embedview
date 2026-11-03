#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

// 已提供首轮实现，但尚未构建或运行验证，后端尚未验收。
namespace webview_adapter {

namespace detail {
class WebViewAccess;
}

struct Options {
    // 在新顶层文档及子框架文档的 document-start 阶段注册到主世界。
    // 不保证 Worker、已有文档或特殊 URL 的行为；不支持注册时不得静默忽略。
    std::string document_start_script;
    // 不保证 User-Agent Client Hints 相应变化。
    std::optional<std::string> user_agent;
    // 仅控制浏览器的默认上下文菜单。
    bool context_menu_enabled = true;
};

struct Callbacks {
    std::function<void(std::error_code)> initialized;
    // 顶层引擎加载完成，不表示 HTTP 成功或协议就绪；
    // 事件与 Navigate 调用不一定一一对应。
    std::function<void(std::error_code, const std::string &url)> load_finished;
    // Windows 仅处理顶层导航；Linux 覆盖顶层和子框架导航，不是通用网络拦截。
    // 返回 true 允许，false 取消；未设置时允许。必须快速同步返回，
    // 不得抛出异常或发生模态循环重入。
    // Linux 的新窗口导航经此回调允许后在当前页面打开；Windows 阻止新窗口。
    std::function<bool(const std::string &url)> navigation_requested;
    std::function<void()> closed;
    // 仅表示用户请求关闭，并不开始关闭；有处理函数时由所有者决定是否忽略，
    // 无处理函数时默认关闭。关闭后撤销，析构期间不调用。
    std::function<void()> close_requested;
};

struct Win32WindowHandle {
    std::uintptr_t value = 0;
};

struct X11WindowHandle {
    unsigned long value = 0;
};

// 借用句柄；调用方不得释放。Close 开始时立即失效。
// monostate 表示没有可用句柄。
using NativeWindowHandle =
        std::variant<std::monostate, Win32WindowHandle, X11WindowHandle>;

// 所有操作、查询、回调和析构都必须在创建它的 UI 线程进行：Windows 上为 COM
// STA， Linux 上为 GTK 主 UI
// 线程。不提供隐藏的工作线程或跨线程派发；应用须自行调度。
// 构造既不初始化浏览器，也不处理消息。
class WebView {
public:
    WebView();
    ~WebView();

    WebView(const WebView &) = delete;
    WebView &operator=(const WebView &) = delete;
    WebView(WebView &&) = delete;
    WebView &operator=(WebView &&) = delete;

    // 成功返回表示已接受，而非已完成。完成可能同步、重入或异步发生；
    // 一旦接受，除非先关闭，否则恰好完成一次。就绪要求确认脚本已注册、
    // UA 和菜单已配置且句柄已就绪，而不仅是提交脚本注册请求。
    // 不支持重新初始化。通用错误使用 std::errc；平台错误保留原有类别。
    [[nodiscard]] std::error_code Initialize(Options options,
                                             Callbacks callbacks);
    // 未就绪时失败。成功不表示页面加载完成。
    [[nodiscard]] std::error_code Navigate(std::string url);
    [[nodiscard]] std::error_code SetVisible(bool visible);
    // 尺寸使用原生客户区单位，而非 Qt 逻辑像素。
    [[nodiscard]] std::error_code Resize(int width, int height);
    bool IsReady() const noexcept;
    NativeWindowHandle GetNativeWindow() const noexcept;

    // 幂等，包括在初始化期间或回调中调用。逻辑关闭立即使句柄失效，
    // 并撤销尚未开始的回调；正在执行的回调可以返回。closed 在清理安全完成后
    // 发送一次，但不一定等待浏览器进程退出。关闭会取消初始化，且不补发
    // initialized。异步清理需要事件循环。析构不调用用户回调。如果初始化
    // 处理消息期间公共对象被删除，实现必须保护内部状态；回调不得依赖
    // 未受保护的裸 this。
    void Close() noexcept;

private:
    friend class detail::WebViewAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// 线程级、非阻塞、非递归。迭代和软时间预算不是硬实时限制；也可能处理其他对象的事件。
// 不提供独立的 run API；已有 Windows 消息循环无需额外的事件泵。
// 调用方只能在相应 UI 线程上使用。
void ProcessEvents();

}  // namespace webview_adapter
