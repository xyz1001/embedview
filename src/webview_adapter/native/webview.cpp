#include "backend.h"

#include <exception>
#include <thread>
#include <utility>

namespace webview {
using detail::State;

namespace {

struct SharedState {
    const std::thread::id thread = std::this_thread::get_id();
    State state = State::kIdle;
    bool owner_alive = true;
    bool cleanup_done = false;
    std::size_t active = 0;
    Callbacks callbacks;
    std::function<void()> closed;
    std::function<void()> cleanup_completed;
    std::shared_ptr<detail::Backend> backend;
    std::shared_ptr<SharedState> cleanup_keep_alive;
};

bool OnThread(const SharedState &state) noexcept {
    return state.thread == std::this_thread::get_id();
}

std::error_code Error(std::errc value) noexcept {
    return std::make_error_code(value);
}

void FinishClose(const std::shared_ptr<SharedState> &state) noexcept {
    if (state->state != State::kClosing || !state->cleanup_done ||
        state->active != 0) {
        return;
    }

    auto keep_alive = state;
    keep_alive->state = State::kClosed;
    keep_alive->backend.reset();
    keep_alive->cleanup_keep_alive.reset();

    auto callback = std::move(keep_alive->closed);
    if (keep_alive->owner_alive && callback) {
        try {
            callback();
        } catch (...) {
            // 用户回调不得抛出异常；不允许异常越过库边界。
        }
    }
}

class Activity {
public:
    explicit Activity(std::shared_ptr<SharedState> state) noexcept
            : state_(std::move(state)) {
        ++state_->active;
    }
    ~Activity() {
        --state_->active;
        FinishClose(state_);
    }
    Activity(const Activity &) = delete;
    Activity &operator=(const Activity &) = delete;

private:
    std::shared_ptr<SharedState> state_;
};

void StartClose(const std::shared_ptr<SharedState> &state) noexcept {
    if (state->state == State::kClosing || state->state == State::kClosed) {
        return;
    }

    state->state = State::kClosing;
    state->closed = std::move(state->callbacks.closed);
    state->callbacks = {};
    auto backend = state->backend;
    if (!backend) {
        state->cleanup_done = true;
        FinishClose(state);
        return;
    }

    // 通知在初始化接受前已分配；关闭过程只移动它并建立临时自持有。
    state->cleanup_keep_alive = state;
    backend->RequestClose(std::move(state->cleanup_completed));
}

detail::BackendEvents MakeEvents(const std::shared_ptr<SharedState> &state) {
    std::weak_ptr<SharedState> weak = state;
    detail::BackendEvents events;
    events.initialized = [weak](std::error_code result) noexcept {
        auto state = weak.lock();
        if (!state || !OnThread(*state) || !state->owner_alive ||
            state->state != State::kInitializing) {
            return;
        }
        Activity activity(state);
        state->state = result ? State::kFailed : State::kReady;
        try {
            auto callback = state->callbacks.initialized;
            if (callback) {
                callback(result);
            }
        } catch (...) {
        }
    };
    events.load_finished = [weak](std::error_code error,
                                  const std::string &url) noexcept {
        auto state = weak.lock();
        if (!state || !OnThread(*state) || !state->owner_alive ||
            state->state != State::kReady) {
            return;
        }
        Activity activity(state);
        try {
            auto callback = state->callbacks.load_finished;
            if (callback) {
                callback(error, url);
            }
        } catch (...) {
        }
    };
    events.navigation_requested = [weak](const std::string &url) noexcept {
        auto state = weak.lock();
        if (!state || !OnThread(*state) || !state->owner_alive ||
            state->state != State::kReady) {
            return false;
        }
        Activity activity(state);
        bool decision = true;
        try {
            auto callback = state->callbacks.navigation_requested;
            if (callback) {
                decision = callback(url);
            }
        } catch (...) {
            decision = false;
        }
        if (!state->owner_alive || state->state != State::kReady) {
            return false;
        }
        return decision;
    };
    events.close_requested = [weak]() noexcept {
        auto state = weak.lock();
        if (!state || !OnThread(*state) || !state->owner_alive ||
            state->state == State::kClosing || state->state == State::kClosed) {
            return;
        }
        Activity activity(state);
        try {
            auto callback = state->callbacks.close_requested;
            if (callback) {
                callback();
            } else {
                StartClose(state);
            }
        } catch (...) {
        }
    };
    return events;
}

template <typename F>
std::error_code ReadyCall(const std::shared_ptr<SharedState> &state, F &&call) {
    if (!OnThread(*state)) {
        return Error(std::errc::operation_not_permitted);
    }
    Activity activity(state);
    if (state->state != State::kReady) {
        return Error(std::errc::operation_not_permitted);
    }

    auto backend = state->backend;
    try {
        return call(*backend);
    } catch (...) {
        return Error(std::errc::io_error);
    }
}

}  // namespace

class WebView::Impl {
public:
    std::shared_ptr<SharedState> state = std::make_shared<SharedState>();
};

WebView::WebView() : impl_(std::make_unique<Impl>()) {}

WebView::~WebView() {
    auto state = impl_->state;
    // 错线程销毁无法安全释放 UI 资源，也不能异步借用已销毁的公共对象。
    if (!OnThread(*state)) {
        std::terminate();
    }
    Activity activity(state);
    state->owner_alive = false;
    state->closed = {};
    state->callbacks.closed = {};
    StartClose(state);
}

std::error_code WebView::Initialize(Options options, Callbacks callbacks) {
    auto state = impl_->state;
    if (!OnThread(*state)) {
        return Error(std::errc::operation_not_permitted);
    }
    Activity activity(state);
    if (state->state != State::kIdle) {
        return Error(std::errc::operation_not_permitted);
    }
    std::shared_ptr<detail::Backend> backend;
    detail::BackendEvents events;
    std::function<void()> cleanup_completed;
    try {
        std::weak_ptr<SharedState> weak = state;
        cleanup_completed = [weak]() noexcept {
            auto state = weak.lock();
            if (!state) {
                return;
            }
            if (!OnThread(*state)) {
                std::terminate();
            }
            state->cleanup_done = true;
            FinishClose(state);
        };
        events = MakeEvents(state);
        backend = detail::MakeBackend();
        if (!backend) {
            return Error(std::errc::not_enough_memory);
        }
    } catch (...) {
        return Error(std::errc::not_enough_memory);
    }
    state->cleanup_completed = std::move(cleanup_completed);
    state->backend = backend;
    state->callbacks = std::move(callbacks);
    state->state = State::kInitializing;
    try {
        backend->BeginInitialize(std::move(options), events);
    } catch (...) {
        events.initialized(Error(std::errc::io_error));
    }
    return {};
}

std::error_code WebView::Navigate(std::string url) {
    auto state = impl_->state;
    if (!OnThread(*state)) {
        return Error(std::errc::operation_not_permitted);
    }
    if (url.empty()) {
        return Error(std::errc::invalid_argument);
    }
    return ReadyCall(state, [&url](detail::Backend &backend) {
        return backend.Navigate(std::move(url));
    });
}

std::error_code WebView::SetVisible(bool visible) {
    auto state = impl_->state;
    return ReadyCall(state, [visible](detail::Backend &backend) {
        return backend.SetVisible(visible);
    });
}

std::error_code WebView::Resize(int width, int height) {
    auto state = impl_->state;
    if (!OnThread(*state)) {
        return Error(std::errc::operation_not_permitted);
    }
    if (width < 0 || height < 0) {
        return Error(std::errc::invalid_argument);
    }
    return ReadyCall(state, [width, height](detail::Backend &backend) {
        return backend.Resize(width, height);
    });
}

std::error_code detail::WebViewAccess::SynchronizeBounds(WebView &webview) {
    auto state = webview.impl_->state;
    return ReadyCall(state, [](detail::Backend &backend) {
        return backend.SynchronizeBounds();
    });
}

bool detail::WebViewAccess::IsClosed(const WebView &webview) noexcept {
    auto state = webview.impl_->state;
    return OnThread(*state) && state->state == State::kClosed;
}

bool WebView::IsReady() const noexcept {
    auto state = impl_->state;
    return OnThread(*state) && state->state == State::kReady;
}

NativeWindowHandle WebView::GetNativeWindow() const noexcept {
    auto state = impl_->state;
    if (!OnThread(*state)) {
        return std::monostate{};
    }
    if (state->state != State::kReady) {
        return std::monostate{};
    }
    return state->backend->GetNativeWindow();
}

void WebView::Close() noexcept {
    auto state = impl_->state;
    if (!OnThread(*state)) {
        return;
    }
    Activity activity(state);
    StartClose(state);
}

void ProcessEvents() {
    static thread_local bool active = false;
    if (active) {
        return;
    }
    struct Guard {
        bool &active;
        explicit Guard(bool &value) : active(value) {
            active = true;
        }
        ~Guard() {
            active = false;
        }
    } guard(active);
    try {
        detail::ProcessPlatformEvents(detail::PumpBudget{});
    } catch (...) {
        // 平台异常不得跨越事件泵边界。
    }
}

}  // namespace webview
