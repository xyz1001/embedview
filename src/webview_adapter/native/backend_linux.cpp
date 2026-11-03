#include "backend.h"

#ifndef WEBVIEW_HEADER
#define WEBVIEW_HEADER
#endif
#include <webview/webview.h>

#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <chrono>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace webview_adapter::detail {
namespace {

std::error_code Error(std::errc value) noexcept {
    return std::make_error_code(value);
}

class GErrorCategory final : public std::error_category {
public:
    explicit GErrorCategory(GQuark domain) : domain_(domain) {}
    const char *name() const noexcept override {
        return g_quark_to_string(domain_);
    }
    std::string message(int code) const override {
        return std::string(name()) + ": " + std::to_string(code);
    }
    std::error_condition default_error_condition(
            int code) const noexcept override {
        if ((domain_ == WEBKIT_NETWORK_ERROR &&
             code == WEBKIT_NETWORK_ERROR_CANCELLED) ||
            (domain_ == G_IO_ERROR && code == G_IO_ERROR_CANCELLED)) {
            return std::make_error_condition(std::errc::operation_canceled);
        }
        return std::error_category::default_error_condition(code);
    }

private:
    GQuark domain_;
};

std::error_code PlatformError(const GError *value) {
    if (!value) {
        return Error(std::errc::io_error);
    }
    // 类别需比任何已发出的 error_code 活得更久。
    static std::mutex mutex;
    static std::map<GQuark, std::unique_ptr<GErrorCategory>> categories;
    std::lock_guard<std::mutex> lock(mutex);
    auto &category = categories[value->domain];
    if (!category) {
        category = std::make_unique<GErrorCategory>(value->domain);
    }
    return {value->code, *category};
}

class LinuxBackend final : public Backend {
    struct FailedLoad {
        std::string url;
        std::error_code error;
    };

    class Guard {
    public:
        explicit Guard(std::shared_ptr<Backend> self) noexcept
                : self_(std::move(self)),
                  backend_(static_cast<LinuxBackend *>(self_.get())) {
            ++backend_->depth_;
        }
        ~Guard() {
            if (--backend_->depth_ == 0 && backend_->closing_ &&
                !backend_->cleaned_) {
                backend_->ScheduleCleanup();
            }
        }

    private:
        std::shared_ptr<Backend> self_;
        LinuxBackend *backend_;
    };

public:
    void BeginInitialize(Options options, BackendEvents events) override {
        auto self = shared_from_this();
        Guard guard(self);
        events_ = std::move(events);
        auto result = Initialize(options, self);
        if (closing_) {
            return;
        }
        auto callback = std::move(events_.initialized);
        if (callback) {
            callback(result);
        }
    }

    std::error_code Navigate(std::string url) override {
        Guard guard(shared_from_this());
        auto result = webview_navigate(view_, url.c_str());
        if (closing_) {
            return Error(std::errc::operation_canceled);
        }
        return WEBVIEW_FAILED(result) ? Error(std::errc::io_error)
                                      : std::error_code{};
    }

    std::error_code SetVisible(bool visible) override {
        Guard guard(shared_from_this());
        if (visible) {
            gtk_widget_show(host_);
        } else {
            gtk_widget_hide(host_);
        }
        return closing_ ? Error(std::errc::operation_canceled)
                        : std::error_code{};
    }

    std::error_code Resize(int width, int height) override {
        Guard guard(shared_from_this());
        // GTK 使用逻辑单位，调用方提供 X11 原生像素，只在此处转换一次。
        const int scale = gtk_widget_get_scale_factor(host_);
        const int divisor = scale > 0 ? scale : 1;
        const int w = width / divisor + (width % divisor != 0);
        const int h = height / divisor + (height % divisor != 0);
        gtk_window_resize(GTK_WINDOW(host_), w > 0 ? w : 1, h > 0 ? h : 1);
        return closing_ ? Error(std::errc::operation_canceled)
                        : std::error_code{};
    }

    std::error_code SynchronizeBounds() override {
        Guard guard(shared_from_this());
        // GTK 容器负责将浏览器分配到宿主客户区，不再次转换尺寸。
        gtk_widget_queue_resize(GTK_WIDGET(browser_));
        return closing_ ? Error(std::errc::operation_canceled)
                        : std::error_code{};
    }

    NativeWindowHandle GetNativeWindow() const noexcept override {
        if (!xid_) {
            return std::monostate{};
        }
        return X11WindowHandle{xid_};
    }

    void RequestClose(std::function<void()> completed) noexcept override {
        Guard guard(shared_from_this());
        if (closing_) {
            return;
        }
        closing_ = true;
        xid_ = 0;
        events_ = {};
        completed_ = std::move(completed);
    }

private:
    std::error_code Initialize(const Options &options,
                               const std::shared_ptr<Backend> &self) {
        if (!gtk_init_check(nullptr, nullptr)) {
            return Error(std::errc::not_supported);
        }
        if (closing_) {
            return Error(std::errc::operation_canceled);
        }
        auto *display = gdk_display_get_default();
        if (!display || !GDK_IS_X11_DISPLAY(display)) {
            return Error(std::errc::not_supported);
        }

        cookie_ = self;
        host_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        if (!host_) {
            return Error(std::errc::not_enough_memory);
        }
        g_object_ref_sink(host_);
        if (closing_) {
            return Error(std::errc::operation_canceled);
        }
        delete_handler_ = g_signal_connect(host_, "delete-event",
                                           G_CALLBACK(OnDelete), &cookie_);
        if (!delete_handler_) {
            return Error(std::errc::io_error);
        }

        // 上游 0.12.0 在特定 NVIDIA/X11 环境可能设置 DMA-BUF 环境变量。
        view_ = webview_create(0, host_);
        if (closing_) {
            return Error(std::errc::operation_canceled);
        }
        if (!view_) {
            return Error(std::errc::io_error);
        }
        if (webview_get_native_handle(
                    view_, WEBVIEW_NATIVE_HANDLE_KIND_UI_WINDOW) != host_) {
            return Error(std::errc::io_error);
        }
        browser_ = static_cast<WebKitWebView *>(webview_get_native_handle(
                view_, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
        if (!browser_) {
            return Error(std::errc::not_supported);
        }
        manager_ = webkit_web_view_get_user_content_manager(browser_);
        auto *settings = webkit_web_view_get_settings(browser_);
        if (!manager_ || !settings) {
            return Error(std::errc::not_supported);
        }
        if (options.user_agent) {
            webkit_settings_set_user_agent(settings,
                                           options.user_agent->c_str());
        }
        if (closing_) {
            return Error(std::errc::operation_canceled);
        }
        if (!options.document_start_script.empty()) {
            script_ = webkit_user_script_new(
                    options.document_start_script.c_str(),
                    WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr,
                    nullptr);
            if (!script_) {
                return Error(std::errc::not_enough_memory);
            }
            webkit_user_content_manager_add_script(manager_, script_);
        }
        if (closing_) {
            return Error(std::errc::operation_canceled);
        }
        policy_handler_ = g_signal_connect(browser_, "decide-policy",
                                           G_CALLBACK(OnPolicy), &cookie_);
        changed_handler_ = g_signal_connect(
                browser_, "load-changed", G_CALLBACK(OnLoadChanged), &cookie_);
        failed_handler_ = g_signal_connect(browser_, "load-failed",
                                           G_CALLBACK(OnLoadFailed), &cookie_);
        close_handler_ = g_signal_connect(browser_, "close",
                                          G_CALLBACK(OnClose), &cookie_);
        if (!options.context_menu_enabled) {
            menu_handler_ = g_signal_connect(browser_, "context-menu",
                                             G_CALLBACK(OnMenu), &cookie_);
        }
        if (!policy_handler_ || !changed_handler_ || !failed_handler_ ||
            !close_handler_ ||
            (!options.context_menu_enabled && !menu_handler_)) {
            return Error(std::errc::io_error);
        }
        if (closing_) {
            return Error(std::errc::operation_canceled);
        }
        gtk_widget_realize(host_);
        if (closing_) {
            return Error(std::errc::operation_canceled);
        }
        auto *window = gtk_widget_get_window(host_);
        if (!window || !GDK_IS_X11_DISPLAY(gdk_window_get_display(window)) ||
            !gdk_window_ensure_native(window)) {
            return Error(std::errc::not_supported);
        }
        xid_ = gdk_x11_window_get_xid(window);
        if (!xid_) {
            return Error(std::errc::not_supported);
        }
        return {};
    }

    static std::shared_ptr<Backend> Lock(gpointer data) noexcept {
        return static_cast<std::weak_ptr<Backend> *>(data)->lock();
    }

    static gboolean OnDelete(GtkWidget *, GdkEvent *, gpointer data) noexcept {
        try {
            auto self = Lock(data);
            if (!self) {
                return TRUE;
            }
            Guard guard(self);
            auto *backend = static_cast<LinuxBackend *>(self.get());
            if (!backend->closing_) {
                auto callback = backend->events_.close_requested;
                if (callback) {
                    callback();
                }
            }
        } catch (...) {
        }
        return TRUE;
    }

    static void OnClose(WebKitWebView *view, gpointer data) noexcept {
        g_signal_stop_emission_by_name(view, "close");
        try {
            auto self = Lock(data);
            if (!self) {
                return;
            }
            Guard guard(self);
            auto *backend = static_cast<LinuxBackend *>(self.get());
            if (!backend->closing_) {
                auto callback = backend->events_.close_requested;
                if (callback) {
                    callback();
                }
            }
        } catch (...) {
        }
    }

    static gboolean OnMenu(WebKitWebView *, WebKitContextMenu *, GdkEvent *,
                           WebKitHitTestResult *, gpointer) noexcept {
        return TRUE;
    }

    static gboolean OnPolicy(WebKitWebView *view,
                             WebKitPolicyDecision *decision,
                             WebKitPolicyDecisionType type,
                             gpointer data) noexcept {
        if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
            type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
            return FALSE;
        }
        try {
            auto self = Lock(data);
            if (!self) {
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            Guard guard(self);
            auto *backend = static_cast<LinuxBackend *>(self.get());
            if (backend->closing_) {
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            auto *action =
                    webkit_navigation_policy_decision_get_navigation_action(
                            WEBKIT_NAVIGATION_POLICY_DECISION(decision));
            auto *request =
                    action ? webkit_navigation_action_get_request(action)
                           : nullptr;
            const char *uri =
                    request ? webkit_uri_request_get_uri(request) : nullptr;
            if (!uri) {
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }

            auto callback = backend->events_.navigation_requested;
            bool allowed = callback ? callback(uri) : true;
            if (backend->closing_ || !allowed) {
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }

            if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
                // 在忽略 decision 前使用借用的原始 request，保留请求内容。
                webkit_web_view_load_request(view, request);
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            return FALSE;
        } catch (...) {
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
    }

    static void OnLoadChanged(WebKitWebView *view, WebKitLoadEvent event,
                              gpointer data) noexcept {
        try {
            auto self = Lock(data);
            if (!self) {
                return;
            }
            Guard guard(self);
            auto *backend = static_cast<LinuxBackend *>(self.get());
            if (backend->closing_) {
                return;
            }
            if (event == WEBKIT_LOAD_STARTED) {
                backend->load_failed_ = false;
                backend->failed_result_.reset();
            } else if (event == WEBKIT_LOAD_FINISHED) {
                auto callback = backend->events_.load_finished;
                const char *uri = webkit_web_view_get_uri(view);
                auto result =
                        backend->failed_result_
                                ? std::move(*backend->failed_result_)
                                : FailedLoad{
                                          uri ? uri : "",
                                          backend->load_failed_
                                                  ? Error(std::errc::io_error)
                                                  : std::error_code{}};
                backend->load_failed_ = false;
                backend->failed_result_.reset();
                if (callback) {
                    callback(result.error, result.url);
                }
            }
        } catch (...) {
        }
    }

    static gboolean OnLoadFailed(WebKitWebView *, WebKitLoadEvent,
                                 const gchar *uri, GError *failure,
                                 gpointer data) noexcept {
        try {
            auto self = Lock(data);
            if (!self) {
                return TRUE;
            }
            Guard guard(self);
            auto *backend = static_cast<LinuxBackend *>(self.get());
            if (backend->closing_) {
                return TRUE;
            }
            backend->load_failed_ = true;
            backend->failed_result_ =
                    FailedLoad{uri ? uri : "", PlatformError(failure)};
        } catch (...) {
        }
        return TRUE;
    }

    void ScheduleCleanup() noexcept {
        if (cleanup_scheduled_ || cleaned_) {
            return;
        }
        // 共享层持有后端直到完成通知；idle 入口再局部保活。
        cleanup_scheduled_ = true;
        if (!g_idle_add_full(
                    G_PRIORITY_DEFAULT_IDLE,
                    [](gpointer data) noexcept -> gboolean {
                        auto *backend = static_cast<LinuxBackend *>(data);
                        auto self = backend->shared_from_this();
                        backend->cleanup_scheduled_ = false;
                        if (backend->depth_ == 0) {
                            backend->Cleanup();
                        }
                        // 若发生嵌套事件循环，最后一个 Guard 退出时再安排清理。
                        return G_SOURCE_REMOVE;
                    },
                    this, nullptr)) {
            std::terminate();
        }
    }

    void Cleanup() noexcept {
        if (cleaned_) {
            return;
        }
        cleaned_ = true;
        auto disconnect = [](gpointer object, gulong &id) {
            if (object && id) {
                g_signal_handler_disconnect(object, id);
            }
            id = 0;
        };
        disconnect(host_, delete_handler_);
        disconnect(browser_, policy_handler_);
        disconnect(browser_, changed_handler_);
        disconnect(browser_, failed_handler_);
        disconnect(browser_, close_handler_);
        disconnect(browser_, menu_handler_);
        if (script_) {
            if (manager_) {
                webkit_user_content_manager_remove_script(manager_, script_);
            }
            webkit_user_script_unref(script_);
            script_ = nullptr;
        }
        manager_ = nullptr;
        browser_ = nullptr;
        if (view_) {
            webview_destroy(view_);
            view_ = nullptr;
        }
        if (host_) {
            gtk_widget_destroy(host_);
            g_object_unref(host_);
            host_ = nullptr;
        }
        cookie_.reset();
        auto completed = std::move(completed_);
        if (completed) {
            completed();
        }
    }

    BackendEvents events_;
    std::function<void()> completed_;
    std::weak_ptr<Backend> cookie_;
    GtkWidget *host_ = nullptr;
    webview_t view_ = nullptr;
    WebKitWebView *browser_ = nullptr;
    WebKitUserContentManager *manager_ = nullptr;
    WebKitUserScript *script_ = nullptr;
    unsigned long xid_ = 0;
    gulong delete_handler_ = 0, policy_handler_ = 0, changed_handler_ = 0;
    gulong failed_handler_ = 0, close_handler_ = 0, menu_handler_ = 0;
    std::size_t depth_ = 0;
    bool closing_ = false, cleaned_ = false, cleanup_scheduled_ = false;
    bool load_failed_ = false;
    std::optional<FailedLoad> failed_result_;
};

}  // namespace

std::shared_ptr<Backend> MakeBackend() {
    return std::make_shared<LinuxBackend>();
}

void ProcessPlatformEvents(PumpBudget budget) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < budget.max_iterations; ++i) {
        if (std::chrono::steady_clock::now() - start >= budget.max_duration) {
            break;
        }
        if (!g_main_context_iteration(nullptr, FALSE)) {
            break;
        }
    }
}

}  // namespace webview_adapter::detail
