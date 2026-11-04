#include "webview_widget.h"
#include "../native/backend.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHideEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QThread>
#include <QTimer>
#include <QWindow>

#include <utility>

#if defined(__linux__)
#include <algorithm>
#include <vector>
#endif

namespace embedview {
namespace {

std::error_code Error(std::errc value) {
    return std::make_error_code(value);
}

#if defined(__linux__)
// 每个 UI 线程共享一个定时器；持有清理中的原生对象，而不是持有 QWidget。
struct Pump {
    QPointer<QTimer> timer;
    std::vector<std::shared_ptr<WebView>> views;
};

void RetainUntilClosed(const std::shared_ptr<WebView> &view) {
    static thread_local std::weak_ptr<Pump> current;
    auto pump = current.lock();
    if (!pump || !pump->timer) {
        pump = std::make_shared<Pump>();
        auto *timer = new QTimer(QCoreApplication::instance());
        pump->timer = timer;
        current = pump;
        QObject::connect(timer, &QTimer::timeout, timer, [pump] {
            // 原生层保证线程级非递归；次数和软时间预算同时有界。
            ProcessEvents();
            auto &views = pump->views;
            views.erase(
                    std::remove_if(views.begin(), views.end(),
                                   [](const auto &item) {
                                       return detail::WebViewAccess::IsClosed(
                                               *item);
                                   }),
                    views.end());
            if (views.empty() && pump->timer) {
                pump->timer->stop();
            }
        });
    }
    pump->views.push_back(view);
    pump->timer->start(10);
}
#endif

}  // namespace

class WebViewWidget::Impl {
public:
    // 与公共控件分离：原生初始化、Qt 事件或用户回调都可能删除控件。
    struct Shared : std::enable_shared_from_this<Shared> {
        explicit Shared(WebViewWidget *widget)
                : owner(widget), native(std::make_shared<WebView>()) {}

        // 阻止在 Qt 包装创建或其他可重入调用的中途开始原生关闭。
        // 控件析构会先解除容器的 QWidget 父子依附，退栈后再拆包装和关闭原生层。
        struct Operation {
            explicit Operation(std::shared_ptr<Shared> value)
                    : shared(std::move(value)) {
                ++shared->operations;
            }
            ~Operation() {
                if (--shared->operations == 0) {
                    shared->DrainClose();
                }
            }
            std::shared_ptr<Shared> shared;
        };

        bool Active() const {
            return owner && status != detail::State::kClosing &&
                   status != detail::State::kClosed;
        }

        void RemoveWrapper() {
            Operation operation(shared_from_this());
            auto window = foreign;
            auto widget = container;
            if (widget) {
                // 先脱离控件父子树；此调用期间保留记录，让重入的控件析构也能
                // 找到并保护容器。close_started 阻止 ParentChange
                // 中重复开始关闭。
                widget->setParent(nullptr);
            }
            // 脱离父子树后再取走记录，随后 owner 被删也不会提前删除本地包装。
            foreign.clear();
            container.clear();
            if (window) {
                // Qt 5.15 createWindowContainer 文档明确允许通过
                // QWindow::setParent 移除包装；源码借助 ChildRemoved 清除
                // container 中的窗口记录。 nullptr
                // 也会请求平台重设为顶层，不表示销毁原生窗口；Win/X11
                // 外来窗口的实际分离及所有权仍需运行验证，不能据此声明平台安全。
                window->setParent(nullptr);
            }
            delete widget.data();
            // container 正常拥有 QWindow；解除嵌入后由此处删除，QPointer
            // 防止双删。
            delete window.data();
        }

        void DrainClose() {
            if (status != detail::State::kClosing || operations != 0 ||
                close_started) {
                return;
            }

            close_started = true;
            RemoveWrapper();
            // 包装已经移除；Close 入口立即使借用句柄失效。
            auto view = native;
            view->Close();
            // 尚未 Initialize 时原生层没有登记回调，直接观察同步关闭结果。
            if (status != detail::State::kClosed &&
                detail::WebViewAccess::IsClosed(*view)) {
                Closed();
            }
        }

        void RequestClose() {
            if (status != detail::State::kClosing &&
                status != detail::State::kClosed) {
                status = detail::State::kClosing;
                // 撤销尚未开始的业务回调，但保留清理完成通知。
                callbacks.initialized = {};
                callbacks.load_finished = {};
                callbacks.navigation_requested = {};
                callbacks.close_requested = {};
            }
            DrainClose();
        }

        void CloseRequested() {
            if (!Active() || handling_close_request) {
                return;
            }
            Operation operation(shared_from_this());
            handling_close_request = true;
            // 回调复制或执行抛异常时也恢复标志；operation 在此守卫之后析构，
            // 保证回调删除控件后，标志所属的共享状态仍然有效。
            struct ResetCloseRequest {
                bool &flag;
                ~ResetCloseRequest() {
                    flag = false;
                }
            } reset{handling_close_request};
            auto callback = callbacks.close_requested;
            if (callback) {
                // 有用户处理器时仅转发，允许忽略请求。用户可在其中关闭或删除控件；
                // Operation 会让关闭等到包装调用退栈后进行，且不再访问裸控件。
                callback();
            } else {
                RequestClose();
            }
        }

        void Closed() {
            // close_requested
            // 在句柄有效时进入统一关闭路径；这里只通知清理完成， 不能等到
            // closed 后才用已经失效的句柄解除嵌入。
            close_started = true;
            status = detail::State::kClosed;
            auto callback = std::move(callbacks.closed);
            callbacks = {};
            if (owner && callback) {
                callback();
            }
        }

        void SyncBounds() {
            if (!Active() || !container || status != detail::State::kReady) {
                return;
            }

            Operation operation(shared_from_this());
            const auto rect = owner->rect();
            container->setGeometry(rect);
            if (Active() && container) {
                // container 使用 Qt 逻辑坐标；原生层读取实际客户区，不再乘
                // DPI。
                (void) detail::WebViewAccess::SynchronizeBounds(*native);
            }
        }

        void Show() {
            if (!Active() || status != detail::State::kReady) {
                return;
            }
            Operation operation(shared_from_this());
            SyncBounds();
            if (!Active() || !container) {
                return;
            }
            container->show();
            if (!Active()) {
                return;
            }
            (void) native->SetVisible(true);
            if (!Active()) {
                return;
            }
            std::weak_ptr<Shared> weak = shared_from_this();
            // 上下文绑定控件；被删除后不会执行下一轮的边界同步。
            QTimer::singleShot(0, owner.data(), [weak] {
                if (auto shared = weak.lock();
                    shared && shared->Active() && shared->owner->isVisible()) {
                    shared->SyncBounds();
                }
            });
        }

        std::error_code Embed() {
            const auto handle = native->GetNativeWindow();
            WId id = 0;
#if defined(_WIN32)
            if (const auto *value = std::get_if<Win32WindowHandle>(&handle)) {
                id = static_cast<WId>(value->value);
            }
#elif defined(__linux__)
            if (const auto *value = std::get_if<X11WindowHandle>(&handle)) {
                id = static_cast<WId>(value->value);
            }
#endif
            if (!id) {
                return Error(std::errc::not_supported);
            }
            foreign = QWindow::fromWinId(id);
            if (!foreign) {
                return Error(std::errc::not_supported);
            }
            if (!Active()) {
                return Error(std::errc::operation_canceled);
            }
            // 先无父对象创建，避免创建过程重入删除 owner 后留下悬空返回值。
            // 成功后 container 接管包装 QWindow，不表示接管外来原生窗口。
            container = QWidget::createWindowContainer(foreign.data(), nullptr);
            if (!container) {
                return Error(std::errc::not_supported);
            }
            if (!Active()) {
                return Error(std::errc::operation_canceled);
            }
            container->setParent(owner.data());
            if (!Active() || !container || !foreign) {
                return Error(std::errc::operation_canceled);
            }
            return {};
        }

        void Initialized(std::error_code error) {
            if (!Active() || status != detail::State::kInitializing) {
                return;
            }
            Operation operation(shared_from_this());
            if (!error) {
                error = Embed();
            }
            if (!Active()) {
                return;
            }
            status = error ? detail::State::kFailed : detail::State::kReady;
            if (!error) {
                SyncBounds();
                if (Active() && owner->isVisible()) {
                    Show();
                } else if (Active()) {
                    (void) native->SetVisible(false);
                }
            }
            if (!Active()) {
                return;
            }
            auto callback = std::move(callbacks.initialized);
            if (callback) {
                callback(error);
            }
            if (error && Active()) {
                // 原生已就绪但 Qt 包装失败时，也按先包装、后原生的顺序清理。
                RequestClose();
            }
        }

        QPointer<WebViewWidget> owner;
        std::shared_ptr<WebView> native;
        QPointer<QWindow> foreign;
        QPointer<QWidget> container;
        Callbacks callbacks;
        detail::State status = detail::State::kIdle;
        unsigned operations = 0;
        bool close_started = false;
        bool handling_close_request = false;
    };

    explicit Impl(WebViewWidget *owner)
            : shared(std::make_shared<Shared>(owner)) {}
    std::shared_ptr<Shared> shared;
};

WebViewWidget::WebViewWidget(QWidget *parent)
        : QWidget(parent), impl_(std::make_unique<Impl>(this)) {}

WebViewWidget::~WebViewWidget() {
    auto shared = impl_->shared;
    shared->owner.clear();
    shared->callbacks = {};
    if (shared->operations != 0 && shared->container) {
        // 当前 Qt/native 调用可能仍在栈上。先解除 QWidget 的 QObject 父子依附，
        // 防止接下来的 QWidget 基类析构提前删除 container 及其 foreign 包装。
        // shared 由 Operation 独立持有；退栈后才在原生句柄有效时统一拆包装。
        shared->container->setParent(nullptr);
    }
    shared->RequestClose();
}

std::error_code WebViewWidget::Initialize(Options options,
                                          Callbacks callbacks) {
    auto shared = impl_->shared;
    if (shared->status != detail::State::kIdle) {
        return Error(std::errc::operation_not_permitted);
    }
    if (!QCoreApplication::instance() || QThread::currentThread() != thread() ||
        thread() != QCoreApplication::instance()->thread()) {
        return Error(std::errc::operation_not_permitted);
    }
#if defined(__linux__)
    if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
        return Error(std::errc::not_supported);
    }
#elif !defined(_WIN32)
    return Error(std::errc::not_supported);
#endif

    shared->status = detail::State::kInitializing;
    shared->callbacks = std::move(callbacks);
#if defined(__linux__)
    // 在 Initialize 前启动共享泵；即使控件在其中被删，原生清理仍有事件循环。
    RetainUntilClosed(shared->native);
#endif
    // 不跨原生同步初始化持有 Operation：重入关闭必须能立即通知原生层。
    std::weak_ptr<Impl::Shared> weak = shared;
    Callbacks native_callbacks;
    native_callbacks.initialized = [weak](std::error_code error) {
        if (auto state = weak.lock()) {
            state->Initialized(error);
        }
    };
    native_callbacks.load_finished = [weak](std::error_code error,
                                            const std::string &url) {
        if (auto state = weak.lock(); state && state->Active() &&
                                      state->status == detail::State::kReady) {
            auto callback = state->callbacks.load_finished;
            if (callback) {
                callback(error, url);
            }
        }
    };
    native_callbacks.navigation_requested = [weak](const std::string &url) {
        if (auto state = weak.lock(); state && state->Active() &&
                                      state->status == detail::State::kReady) {
            auto callback = state->callbacks.navigation_requested;
            const bool allow = callback ? callback(url) : true;
            return state->Active() ? allow : false;
        }
        return false;
    };
    native_callbacks.closed = [weak] {
        if (auto state = weak.lock()) {
            state->Closed();
        }
    };
    native_callbacks.close_requested = [weak] {
        if (auto state = weak.lock()) {
            state->CloseRequested();
        }
    };
    const auto error = shared->native->Initialize(std::move(options),
                                                  std::move(native_callbacks));
    // 此后绝不再访问 this/impl_：上面的调用可能同步回调并删除控件。
    if (error && shared->Active()) {
        shared->callbacks.initialized = {};
        shared->RequestClose();
    }
    return error;
}

std::error_code WebViewWidget::Navigate(std::string url) {
    auto shared = impl_->shared;
    if (!shared->Active() || shared->status != detail::State::kReady) {
        return Error(std::errc::operation_not_permitted);
    }
    Impl::Shared::Operation operation(shared);
    return shared->native->Navigate(std::move(url));
}

std::error_code WebViewWidget::SetMuted(bool muted) {
    auto shared = impl_->shared;
    if (!shared->Active() || shared->status != detail::State::kReady) {
        return Error(std::errc::operation_not_permitted);
    }
    Impl::Shared::Operation operation(shared);
    return shared->native->SetMuted(muted);
}

bool WebViewWidget::IsReady() const noexcept {
    return impl_->shared->status == detail::State::kReady;
}

void WebViewWidget::CloseBrowser() noexcept {
    auto shared = impl_->shared;
    shared->RequestClose();
}

void WebViewWidget::showEvent(QShowEvent *event) {
    auto shared = impl_->shared;
    QWidget::showEvent(event);
    shared->Show();
}

void WebViewWidget::hideEvent(QHideEvent *event) {
    auto shared = impl_->shared;
    QWidget::hideEvent(event);
    if (shared->Active() && shared->status == detail::State::kReady) {
        Impl::Shared::Operation operation(shared);
        (void) shared->native->SetVisible(false);
    }
}

void WebViewWidget::resizeEvent(QResizeEvent *event) {
    auto shared = impl_->shared;
    QWidget::resizeEvent(event);
    shared->SyncBounds();
}

void WebViewWidget::closeEvent(QCloseEvent *event) {
    auto shared = impl_->shared;
    QWidget::closeEvent(event);
    if (shared->owner && event->isAccepted()) {
        shared->RequestClose();
    }
}

}  // namespace embedview
