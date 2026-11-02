#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

// 仅为接口草案：不提供实现或运行时能力。
namespace webview_adapter {

enum class State { idle, initializing, ready, failed, closing, closed };
enum class NavigationDecision { allow, cancel };

struct Options {
    // 在新顶层文档及子框架文档的 document-start 阶段注册到主世界。
    // 不保证 Worker、已有文档或特殊 URL 的行为；不支持注册时不得静默忽略。
    std::string document_start_script;
    // 不保证 User-Agent Client Hints 相应变化。
    std::optional<std::string> user_agent;
    // 仅控制浏览器的默认上下文菜单。
    bool context_menu_enabled = true;
};

struct NavigationRequest {
    std::string url;
};

enum class LoadStatus { succeeded, failed, cancelled };

struct LoadResult {
    std::string url;
    LoadStatus status;
    std::error_code error;
};

struct Callbacks {
    std::function<void(std::error_code)> initialized;
    // 顶层引擎加载完成，不表示 HTTP 成功或协议就绪；
    // 事件与 navigate 调用不一定一一对应。
    std::function<void(const LoadResult&)> load_finished;
    // 仅处理当前顶层导航，不是网络拦截。必须快速同步返回，
    // 不得抛出异常或发生模态循环重入。
    // 默认阻止新窗口；该策略待用户审查，因为桌面 Linux 之前会将其重定向到当前页面。
    std::function<NavigationDecision(const NavigationRequest&)> navigation_requested;
    std::function<void()> closed;
};

struct Win32WindowHandle {
    std::uintptr_t value = 0;
};

struct X11WindowHandle {
    unsigned long value = 0;
};

// 借用句柄；调用方不得释放。close 开始时立即失效。
// monostate 表示没有可用句柄。
using NativeWindowHandle = std::variant<std::monostate, Win32WindowHandle, X11WindowHandle>;

// 所有操作、查询、回调和析构都必须在创建它的 UI 线程进行：Windows 上为 COM STA，
// Linux 上为 GTK 主 UI 线程。不提供隐藏的工作线程或跨线程派发；应用须自行调度。
// 构造既不初始化浏览器，也不处理消息。
class WebView {
public:
    WebView();
    ~WebView();

    WebView(const WebView&) = delete;
    WebView& operator=(const WebView&) = delete;
    WebView(WebView&&) = delete;
    WebView& operator=(WebView&&) = delete;

    // 成功返回表示已接受，而非已完成。完成可能同步、重入或异步发生；
    // 一旦接受，除非先关闭，否则恰好完成一次。就绪要求确认脚本已注册、
    // UA 和菜单已配置且句柄已就绪，而不仅是提交脚本注册请求。
    // 不支持重新初始化。通用错误使用 std::errc；平台错误保留原有类别。
    [[nodiscard]] std::error_code initialize(Options options, Callbacks callbacks);
    // 未就绪时失败。成功不表示页面加载完成。
    [[nodiscard]] std::error_code navigate(std::string url);
    [[nodiscard]] std::error_code set_visible(bool visible);
    // 尺寸使用原生客户区单位，而非 Qt 逻辑像素。使用 synchronize_bounds
    // 避免重复进行 DPI 转换。
    [[nodiscard]] std::error_code resize(int width, int height);
    [[nodiscard]] std::error_code synchronize_bounds();
    State state() const noexcept;
    NativeWindowHandle native_window() const noexcept;

    // 幂等，包括在初始化期间或回调中调用。逻辑关闭立即使句柄失效，
    // 并撤销尚未开始的回调；正在执行的回调可以返回。closed 在清理安全完成后
    // 发送一次，但不一定等待浏览器进程退出。关闭会取消初始化，且不补发
    // initialized。异步清理需要事件循环。析构不调用用户回调。如果初始化
    // 处理消息期间公共对象被删除，实现必须保护内部状态；回调不得依赖
    // 未受保护的裸 this。
    void close() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct PumpBudget {
    std::size_t max_iterations = 16;
    std::chrono::milliseconds max_duration{2};
};

// 线程级、非阻塞、非递归。迭代和软时间预算不是硬实时限制；也可能处理其他对象的事件。
// 不提供独立的 run API；已有 Windows 消息循环无需额外的事件泵。
// 调用方只能在相应 UI 线程上使用。
void process_events(PumpBudget budget = {});

} // namespace webview_adapter
