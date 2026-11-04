#pragma once

#include "webview.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <system_error>

namespace embedview::detail {

enum class State { kIdle, kInitializing, kReady, kFailed, kClosing, kClosed };

struct PumpBudget {
    std::size_t max_iterations = 16;
    std::chrono::milliseconds max_duration{2};
};

class WebViewAccess {
public:
    static EMBEDVIEW_NATIVE_API std::error_code SynchronizeBounds(
            WebView &webview);
    static EMBEDVIEW_NATIVE_API bool IsClosed(
            const WebView &webview) noexcept;
};

struct BackendEvents {
    std::function<void(std::error_code)> initialized;
    std::function<void(std::error_code, const std::string &url)> load_finished;
    std::function<bool(const std::string &url)> navigation_requested;
    std::function<void()> close_requested;
};

// 工厂只分配对象，不调用平台接口；共享层先持有后端，再调用 BeginInitialize。
// BeginInitialize 首行须通过 shared_from_this 保活；事件仅弱引用共享状态。
// 返回前必须完成 initialized，或因关闭而取消，返回后不得补发。
// RequestClose 立即取消事件，completed 恰好一次，须在资源安全清理后通知，
// 可以同步或延迟；关闭后除 completed 外不得再发事件。平台创建和回调必须有
// 深度保护；使用独立消息或 GLib idle 延迟清理，直到保护深度为零。
// 共享层保证操作入口已就绪且 Resize 尺寸非负，并持有后端直到清理完成。
class Backend : public std::enable_shared_from_this<Backend> {
public:
    Backend(const Backend &) = delete;
    Backend &operator=(const Backend &) = delete;
    virtual ~Backend() = default;
    virtual void BeginInitialize(Options options, BackendEvents events) = 0;
    virtual std::error_code Navigate(std::string url) = 0;
    virtual std::error_code SetMuted(bool muted) = 0;
    virtual std::error_code SetVisible(bool visible) = 0;
    virtual std::error_code Resize(int width, int height) = 0;
    virtual std::error_code SynchronizeBounds() = 0;
    virtual NativeWindowHandle GetNativeWindow() const noexcept = 0;
    virtual void RequestClose(std::function<void()> completed) noexcept = 0;

protected:
    Backend() = default;
};

std::shared_ptr<Backend> MakeBackend();
void ProcessPlatformEvents(PumpBudget budget);

}  // namespace embedview::detail
