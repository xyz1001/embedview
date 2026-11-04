#include "backend.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// WebView2 的声明依赖 Windows 和 COM 类型。
#include <windows.h>

#include <objbase.h>

#include <WebView2.h>
#include <wrl.h>
#include <wrl/client.h>
#define WEBVIEW_HEADER
#include <webview/webview.h>

#include <chrono>
#include <climits>
#include <map>
#include <utility>

namespace embedview::detail {
namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

std::error_code HrError(HRESULT hr) {
    return {static_cast<int>(hr), std::system_category()};
}
std::error_code WinError() {
    auto e = GetLastError();
    return e ? std::error_code(static_cast<int>(e), std::system_category())
             : std::make_error_code(std::errc::io_error);
}

class ScopedAutoplayArguments {
public:
    std::error_code Apply() {
        SetLastError(ERROR_SUCCESS);
        DWORD size = GetEnvironmentVariableW(kName, nullptr, 0);
        if (!size) {
            DWORD error = GetLastError();
            if (error != ERROR_SUCCESS && error != ERROR_ENVVAR_NOT_FOUND) {
                return WinError();
            }
            existed_ = error != ERROR_ENVVAR_NOT_FOUND;
        } else {
            existed_ = true;
            original_.resize(size);
            DWORD length =
                    GetEnvironmentVariableW(kName, original_.data(), size);
            if (!length || length >= size) {
                return WinError();
            }
            original_.resize(length);
        }
        if (original_.find(L"--autoplay-policy=") != std::wstring::npos) {
            return {};
        }
        auto arguments =
                original_ + L" --autoplay-policy=no-user-gesture-required";
        if (!SetEnvironmentVariableW(kName, arguments.c_str())) {
            return WinError();
        }
        changed_ = true;
        return {};
    }

    ~ScopedAutoplayArguments() {
        if (changed_) {
            SetEnvironmentVariableW(kName,
                                    existed_ ? original_.c_str() : nullptr);
        }
    }

private:
    static constexpr const wchar_t *kName =
            L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
    std::wstring original_;
    bool existed_ = false;
    bool changed_ = false;
};

class WebErrorCategory final : public std::error_category {
public:
    const char *name() const noexcept override {
        return "WebView2";
    }
    std::string message(int value) const override {
        return "WebView2 navigation error " + std::to_string(value);
    }
};
const WebErrorCategory kWebErrorCategory;

std::error_code Widen(const std::string &s, std::wstring &out) {
    if (s.find('\0') != std::string::npos || s.size() > INT_MAX) {
        return std::make_error_code(std::errc::invalid_argument);
    }
    if (s.empty()) {
        out.clear();
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                static_cast<int>(s.size()), nullptr, 0);
    if (!n) {
        return WinError();
    }
    out.resize(n);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                             static_cast<int>(s.size()), out.data(), n)) {
        return WinError();
    }
    return {};
}
std::error_code Narrow(const wchar_t *s, std::string &out) {
    if (!s) {
        return std::make_error_code(std::errc::invalid_argument);
    }
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, nullptr,
                                0, nullptr, nullptr);
    if (!n) {
        return WinError();
    }
    out.resize(n);
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1, out.data(),
                             n, nullptr, nullptr)) {
        return WinError();
    }
    out.pop_back();
    return {};
}
struct CoString {
    LPWSTR value = nullptr;
    ~CoString() {
        CoTaskMemFree(value);
    }
};

class WinBackend final : public Backend {
    static constexpr UINT kCleanupMessage = WM_APP + 0x4b1;
    struct Cookie {
        std::weak_ptr<WinBackend> weak;
    };
    struct Guard {
        std::shared_ptr<WinBackend> self;
        explicit Guard(std::shared_ptr<WinBackend> p) : self(std::move(p)) {
            ++self->depth_;
        }
        ~Guard() {
            if (--self->depth_ == 0 && self->closing_) {
                self->ScheduleCleanup();
            }
        }
    };
    std::shared_ptr<WinBackend> Hold() {
        return std::static_pointer_cast<WinBackend>(shared_from_this());
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp,
                                       LPARAM lp) noexcept {
        try {
            auto *cookie = reinterpret_cast<Cookie *>(
                    GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (msg == WM_NCCREATE) {
                cookie = static_cast<Cookie *>(
                        reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                  reinterpret_cast<LONG_PTR>(cookie));
            }
            auto self = cookie ? cookie->weak.lock() : nullptr;
            if (!self) {
                return DefWindowProcW(hwnd, msg, wp, lp);
            }
            if (hwnd == self->dispatcher_ &&
                (msg == kCleanupMessage || (msg == WM_TIMER && wp == 1))) {
                if (msg == kCleanupMessage) {
                    self->queued_ = false;
                }
                if (self->closing_ && self->depth_ == 0) {
                    self->Cleanup();
                }
                return 0;
            }
            Guard guard(self);
            if (hwnd == self->host_) {
                if (msg == WM_CLOSE) {
                    self->NotifyClose();
                    return 0;
                }
                if (msg == WM_SIZE && !self->closing_ && self->controller_) {
                    (void) self->SyncBounds();
                }
                if (msg == WM_NCDESTROY) {
                    self->host_ = nullptr;
                }
            }
            if (msg == WM_NCDESTROY) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        } catch (...) {
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    static std::error_code RegisterClass() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WebviewAdapterNativeHost_1";
        if (!RegisterClassW(&wc) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return WinError();
        }
        return {};
    }
    void ScheduleCleanup() noexcept {
        if (!closing_ || cleaned_ || queued_ || depth_) {
            return;
        }
        if (dispatcher_ && PostMessageW(dispatcher_, kCleanupMessage, 0, 0)) {
            queued_ = true;
            return;
        }
        // 提前启动的常驻定时器兜底，消息投递失败时不在原生回调栈中销毁。
        // 窗口创建之前没有浏览器或窗口回调栈，可以直接完成清理。
        if (!dispatcher_ && !host_ && !view_) {
            Cleanup();
        }
    }
    void Cleanup() noexcept {
        if (cleaned_) {
            return;
        }
        cleaned_ = true;
        if (dispatcher_) {
            KillTimer(dispatcher_, 1);
        }
        if (core_) {
            if (nav_registered_) {
                core_->remove_NavigationStarting(nav_token_);
            }
            if (complete_registered_) {
                core_->remove_NavigationCompleted(complete_token_);
            }
            if (new_registered_) {
                core_->remove_NewWindowRequested(new_token_);
            }
            if (close_registered_) {
                core_->remove_WindowCloseRequested(close_token_);
            }
            if (!script_id_.empty()) {
                core_->RemoveScriptToExecuteOnDocumentCreated(
                        script_id_.c_str());
            }
        }
        // 创建期间重入关闭时，浏览器已返回但初始化尚未取得控制器。
        // 原生句柄为借用引用，ComPtr 赋值时补充引用以便平衡释放。
        if (view_ && !controller_) {
            auto *borrowed = static_cast<ICoreWebView2Controller *>(
                    webview_get_native_handle(
                            view_,
                            WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
            if (borrowed) {
                controller_ = borrowed;
            }
        }
        if (controller_) {
            controller_->Close();
        }
        core_.Reset();
        controller_.Reset();
        if (view_) {
            webview_destroy(view_);
            view_ = nullptr;
        }
        if (host_ && IsWindow(host_)) {
            DestroyWindow(host_);
        }
        host_ = nullptr;
        if (dispatcher_ && IsWindow(dispatcher_)) {
            DestroyWindow(dispatcher_);
        }
        dispatcher_ = nullptr;
        if (com_initialized_) {
            CoUninitialize();
            com_initialized_ = false;
        }
        auto completed = std::move(completed_);
        if (completed) {
            completed();
        }
    }
    void FinishInit(std::error_code error) {
        if (closing_ || init_finished_) {
            return;
        }
        init_finished_ = true;
        auto callback = std::move(events_.initialized);
        if (callback) {
            callback(error);
        }
    }
    void NotifyClose() {
        if (closing_) {
            return;
        }
        auto callback = events_.close_requested;
        if (callback) {
            try {
                callback();
            } catch (...) {
            }
        }
    }
    std::error_code SyncBounds() {
        if (!host_ || !child_ || !controller_) {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        RECT r{};
        if (!GetClientRect(host_, &r)) {
            return WinError();
        }
        if (!SetWindowPos(child_, nullptr, 0, 0, r.right, r.bottom,
                          SWP_NOZORDER | SWP_NOACTIVATE)) {
            return WinError();
        }
        RECT bounds{0, 0, r.right, r.bottom};
        HRESULT hr = controller_->put_Bounds(bounds);
        return FAILED(hr) ? HrError(hr) : std::error_code{};
    }
    std::error_code InstallEvents() {
        std::weak_ptr<WinBackend> weak = Hold();
        auto nav = Callback<ICoreWebView2NavigationStartingEventHandler>(
                [weak](ICoreWebView2 *,
                       ICoreWebView2NavigationStartingEventArgs *args)
                        -> HRESULT {
                    try {
                        auto self = weak.lock();
                        if (!self || !args) {
                            return S_OK;
                        }
                        Guard guard(self);
                        if (self->closing_) {
                            args->put_Cancel(TRUE);
                            return S_OK;
                        }
                        CoString uri;
                        UINT64 id = 0;
                        HRESULT hr = args->get_Uri(&uri.value);
                        std::string url;
                        if (FAILED(hr) || Narrow(uri.value, url)) {
                            args->put_Cancel(TRUE);
                            return S_OK;
                        }
                        if (SUCCEEDED(args->get_NavigationId(&id))) {
                            self->urls_[id] = url;
                        }
                        auto callback = self->events_.navigation_requested;
                        if (callback && !callback(url)) {
                            args->put_Cancel(TRUE);
                        }
                        if (self->closing_) {
                            args->put_Cancel(TRUE);
                        }
                    } catch (...) {
                        if (args) {
                            args->put_Cancel(TRUE);
                        }
                    }
                    return S_OK;
                });
        if (!nav) {
            return HrError(E_OUTOFMEMORY);
        }
        HRESULT hr = core_->add_NavigationStarting(nav.Get(), &nav_token_);
        if (FAILED(hr)) {
            return HrError(hr);
        }
        nav_registered_ = true;
        if (closing_) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        auto complete = Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [weak](ICoreWebView2 *,
                       ICoreWebView2NavigationCompletedEventArgs *args)
                        -> HRESULT {
                    try {
                        auto self = weak.lock();
                        if (!self || !args) {
                            return S_OK;
                        }
                        Guard guard(self);
                        if (self->closing_) {
                            return S_OK;
                        }
                        UINT64 id = 0;
                        BOOL success = FALSE;
                        HRESULT hr = args->get_NavigationId(&id);
                        std::string url;
                        if (SUCCEEDED(hr)) {
                            auto it = self->urls_.find(id);
                            if (it != self->urls_.end()) {
                                url = std::move(it->second);
                                self->urls_.erase(it);
                            }
                        }
                        if (SUCCEEDED(hr)) {
                            hr = args->get_IsSuccess(&success);
                        }
                        std::error_code error;
                        if (FAILED(hr)) {
                            error = HrError(hr);
                        } else if (!success) {
                            COREWEBVIEW2_WEB_ERROR_STATUS status{};
                            hr = args->get_WebErrorStatus(&status);
                            if (FAILED(hr)) {
                                error = HrError(hr);
                            } else if (
                                    status ==
                                    COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                                error = std::make_error_code(
                                        std::errc::operation_canceled);
                            } else {
                                error = status ? std::error_code(
                                                         static_cast<int>(
                                                                 status),
                                                         kWebErrorCategory)
                                               : std::make_error_code(
                                                         std::errc::io_error);
                            }
                        }
                        auto callback = self->events_.load_finished;
                        if (callback) {
                            callback(error, url);
                        }
                    } catch (...) {
                    }
                    return S_OK;
                });
        if (!complete) {
            return HrError(E_OUTOFMEMORY);
        }
        hr = core_->add_NavigationCompleted(complete.Get(), &complete_token_);
        if (FAILED(hr)) {
            return HrError(hr);
        }
        complete_registered_ = true;
        if (closing_) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        auto new_window = Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [weak](ICoreWebView2 *,
                       ICoreWebView2NewWindowRequestedEventArgs *args)
                        -> HRESULT {
                    try {
                        auto self = weak.lock();
                        if (self) {
                            Guard guard(self);
                            if (args) {
                                return args->put_Handled(TRUE);
                            }
                        } else if (args) {
                            return args->put_Handled(TRUE);
                        }
                    } catch (...) {
                    }
                    return S_OK;
                });
        if (!new_window) {
            return HrError(E_OUTOFMEMORY);
        }
        hr = core_->add_NewWindowRequested(new_window.Get(), &new_token_);
        if (FAILED(hr)) {
            return HrError(hr);
        }
        new_registered_ = true;
        if (closing_) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        auto close = Callback<ICoreWebView2WindowCloseRequestedEventHandler>(
                [weak](ICoreWebView2 *, IUnknown *) -> HRESULT {
                    try {
                        auto self = weak.lock();
                        if (self) {
                            Guard guard(self);
                            self->NotifyClose();
                        }
                    } catch (...) {
                    }
                    return S_OK;
                });
        if (!close) {
            return HrError(E_OUTOFMEMORY);
        }
        hr = core_->add_WindowCloseRequested(close.Get(), &close_token_);
        if (FAILED(hr)) {
            return HrError(hr);
        }
        close_registered_ = true;
        return {};
    }

public:
    void BeginInitialize(Options options, BackendEvents events) override {
        auto self = Hold();
        Guard guard(self);
        events_ = std::move(events);
        cookie_.weak = self;
        try {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(hr)) {
                FinishInit(HrError(hr));
                return;
            }
            com_initialized_ = true;
            if (auto ec = RegisterClass()) {
                FinishInit(ec);
                return;
            }
            dispatcher_ = CreateWindowExW(0, L"WebviewAdapterNativeHost_1", L"",
                                          0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                          GetModuleHandleW(nullptr), &cookie_);
            if (!dispatcher_) {
                FinishInit(WinError());
                return;
            }
            // 在创建浏览器之前建立无需再次分配的关闭兜底机制。
            if (!SetTimer(dispatcher_, 1, 250, nullptr)) {
                auto ec = WinError();
                DestroyWindow(dispatcher_);
                dispatcher_ = nullptr;
                CoUninitialize();
                com_initialized_ = false;
                FinishInit(ec);
                return;
            }
            if (closing_) {
                return;
            }
            host_ = CreateWindowExW(0, L"WebviewAdapterNativeHost_1", L"",
                                    WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), &cookie_);
            if (!host_) {
                FinishInit(WinError());
                return;
            }
            {
                // WebView2 创建环境时读取进程变量；仅在调用期间临时追加。
                ScopedAutoplayArguments arguments;
                if (options.allow_autoplay) {
                    if (auto ec = arguments.Apply()) {
                        FinishInit(ec);
                        return;
                    }
                }
                view_ = webview_create(0, host_);
            }
            if (closing_) {
                return;
            }
            if (!view_) {
                FinishInit(std::make_error_code(std::errc::io_error));
                return;
            }
            if (!host_ || !IsWindow(host_)) {
                FinishInit(std::make_error_code(std::errc::operation_canceled));
                return;
            }
            child_ = static_cast<HWND>(webview_get_native_handle(
                    view_, WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET));
            auto *borrowed = static_cast<ICoreWebView2Controller *>(
                    webview_get_native_handle(
                            view_,
                            WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
            if (!child_ || !borrowed) {
                FinishInit(std::make_error_code(std::errc::not_supported));
                return;
            }
            controller_ = borrowed;
            hr = controller_->get_CoreWebView2(&core_);
            if (FAILED(hr) || !core_) {
                FinishInit(FAILED(hr) ? HrError(hr) : HrError(E_FAIL));
                return;
            }
            ComPtr<ICoreWebView2Settings> settings;
            hr = core_->get_Settings(&settings);
            if (FAILED(hr) || !settings) {
                FinishInit(FAILED(hr) ? HrError(hr) : HrError(E_FAIL));
                return;
            }
            hr = settings->put_AreDefaultContextMenusEnabled(
                    options.context_menu_enabled ? TRUE : FALSE);
            if (FAILED(hr)) {
                FinishInit(HrError(hr));
                return;
            }
            if (options.user_agent) {
                std::wstring ua;
                if (auto ec = Widen(*options.user_agent, ua)) {
                    FinishInit(ec);
                    return;
                }
                ComPtr<ICoreWebView2Settings2> settings2;
                hr = settings.As(&settings2);
                if (FAILED(hr)) {
                    FinishInit(HrError(hr));
                    return;
                }
                hr = settings2->put_UserAgent(ua.c_str());
                if (FAILED(hr)) {
                    FinishInit(HrError(hr));
                    return;
                }
            }
            if (options.background_color) {
                ComPtr<ICoreWebView2Controller2> controller2;
                hr = controller_.As(&controller2);
                if (FAILED(hr)) {
                    FinishInit(HrError(hr));
                    return;
                }
                const auto rgb = *options.background_color;
                COREWEBVIEW2_COLOR color{255, static_cast<BYTE>(rgb >> 16),
                                         static_cast<BYTE>(rgb >> 8),
                                         static_cast<BYTE>(rgb)};
                hr = controller2->put_DefaultBackgroundColor(color);
                if (FAILED(hr)) {
                    FinishInit(HrError(hr));
                    return;
                }
            }
            if (auto ec = InstallEvents()) {
                FinishInit(ec);
                return;
            }
            if (closing_) {
                return;
            }
            if (auto ec = SyncBounds()) {
                FinishInit(ec);
                return;
            }
            if (options.document_start_script.empty()) {
                FinishInit({});
                return;
            }
            std::wstring script;
            if (auto ec = Widen(options.document_start_script, script)) {
                FinishInit(ec);
                return;
            }
            std::weak_ptr<WinBackend> weak = self;
            auto handler = Callback<
                    ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                    [weak](HRESULT result, LPCWSTR id) -> HRESULT {
                        auto self = weak.lock();
                        if (!self) {
                            return S_OK;
                        }
                        Guard guard(self);
                        try {
                            if (SUCCEEDED(result) && id && *id &&
                                !self->cleaned_) {
                                self->script_id_ = id;
                            }
                            if (!self->closing_ && !self->init_finished_ &&
                                !self->script_completed_) {
                                self->script_result_ =
                                        SUCCEEDED(result) && (!id || !*id)
                                                ? E_FAIL
                                                : result;
                                self->script_completed_ = true;
                            }
                        } catch (...) {
                            if (!self->closing_ && !self->init_finished_ &&
                                !self->script_completed_) {
                                self->script_result_ = E_OUTOFMEMORY;
                                self->script_completed_ = true;
                            }
                        }
                        // GetMessageW 处理发送消息时可能直接调用此处理器。
                        if (self->script_completed_ && self->dispatcher_) {
                            PostMessageW(self->dispatcher_, WM_NULL, 0, 0);
                        }
                        return S_OK;
                    });
            if (!handler) {
                FinishInit(HrError(E_OUTOFMEMORY));
                return;
            }
            hr = core_->AddScriptToExecuteOnDocumentCreated(script.c_str(),
                                                            handler.Get());
            if (FAILED(hr)) {
                FinishInit(HrError(hr));
                return;
            }
            while (!script_completed_ && !closing_) {
                MSG msg{};
                const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
                if (result == 0) {
                    PostQuitMessage(static_cast<int>(msg.wParam));
                    FinishInit(std::make_error_code(
                            std::errc::operation_canceled));
                    return;
                }
                if (result == -1) {
                    auto error = WinError();
                    FinishInit(error);
                    return;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!closing_) {
                FinishInit(FAILED(script_result_) ? HrError(script_result_)
                                                  : std::error_code{});
            }
        } catch (...) {
            FinishInit(std::make_error_code(std::errc::not_enough_memory));
        }
    }
    std::error_code Navigate(std::string url) override {
        Guard guard(Hold());
        std::wstring wide;
        if (auto ec = Widen(url, wide)) {
            return ec;
        }
        HRESULT hr = core_->Navigate(wide.c_str());
        return FAILED(hr) ? HrError(hr) : std::error_code{};
    }
    std::error_code SetMuted(bool muted) override {
        Guard guard(Hold());
        ComPtr<ICoreWebView2_8> core;
        HRESULT hr = core_.As(&core);
        if (FAILED(hr)) {
            return HrError(hr);
        }
        hr = core->put_IsMuted(muted ? TRUE : FALSE);
        return FAILED(hr) ? HrError(hr) : std::error_code{};
    }
    std::error_code SetVisible(bool visible) override {
        Guard guard(Hold());
        HRESULT hr = controller_->put_IsVisible(visible ? TRUE : FALSE);
        if (FAILED(hr)) {
            return HrError(hr);
        }
        if (closing_) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        ShowWindow(host_, visible ? SW_SHOWNA : SW_HIDE);
        if (closing_) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        ShowWindow(child_, visible ? SW_SHOWNA : SW_HIDE);
        return {};
    }
    std::error_code Resize(int width, int height) override {
        Guard guard(Hold());
        if (!SetWindowPos(host_, nullptr, 0, 0, width, height,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
            return WinError();
        }
        if (closing_) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        return SyncBounds();
    }
    std::error_code SynchronizeBounds() override {
        Guard guard(Hold());
        return SyncBounds();
    }
    NativeWindowHandle GetNativeWindow() const noexcept override {
        return !closing_ && host_
                       ? NativeWindowHandle{Win32WindowHandle{
                                 reinterpret_cast<std::uintptr_t>(host_)}}
                       : NativeWindowHandle{};
    }
    void RequestClose(std::function<void()> completed) noexcept override {
        auto self = Hold();
        Guard guard(self);
        if (closing_) {
            return;
        }
        closing_ = true;
        events_ = {};
        completed_ = std::move(completed);
        // GetMessageW 可能在处理跨线程发送消息时重入关闭，唤醒等待。
        if (dispatcher_ && depth_ > 1) {
            PostMessageW(dispatcher_, WM_NULL, 0, 0);
        }
    }

private:
    Cookie cookie_;
    HWND host_ = nullptr, dispatcher_ = nullptr, child_ = nullptr;
    webview_t view_ = nullptr;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> core_;
    EventRegistrationToken nav_token_{}, complete_token_{}, new_token_{},
            close_token_{};
    bool nav_registered_ = false, complete_registered_ = false,
         new_registered_ = false, close_registered_ = false;
    bool com_initialized_ = false, init_finished_ = false, closing_ = false,
         cleaned_ = false, queued_ = false;
    bool script_completed_ = false;
    HRESULT script_result_ = E_FAIL;
    int depth_ = 0;
    BackendEvents events_;
    std::function<void()> completed_;
    std::wstring script_id_;
    std::map<UINT64, std::string> urls_;
};
}  // namespace

std::shared_ptr<Backend> MakeBackend() {
    return std::make_shared<WinBackend>();
}
void ProcessPlatformEvents(PumpBudget budget) {
    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < budget.max_iterations; ++i) {
        if (std::chrono::steady_clock::now() - start >= budget.max_duration) {
            break;
        }
        MSG msg{};
        if (!PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE)) {
            break;
        }
        if (msg.message == WM_QUIT) {
            break;
        }
        if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            break;
        }
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
}  // namespace embedview::detail
#endif
