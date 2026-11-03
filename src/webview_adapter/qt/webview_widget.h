#pragma once

#include <webview_adapter/native/webview.h>
#include <QWidget>

#include <memory>
#include <string>
#include <system_error>

class QCloseEvent;
class QHideEvent;
class QResizeEvent;
class QShowEvent;

namespace webview_adapter {

// 构造不初始化浏览器，也不处理消息。原生接口的创建线程、
// 回调和析构规则同样适用于此；initialized 表示原生初始化与 Qt 封装均已成功。
// 不暴露可变的原生对象。Linux Qt 嵌入仅限 xcb/X11，且仍需要 GTK。共享的有界
// GLib 事件泵必须从原生初始化之前持续运行到清理安全完成；不得使用无界 pending
// 循环， 也不得通过全局环境变量静默更改平台后端。
// 容器与原生对象的所有权和销毁顺序需要在后端验证；不承诺未经验证的分离方法。
// 原生用户关闭请求先经 close_requested 转发；用户有处理器时可忽略请求，否则走
// CloseBrowser 的拆包装、关闭原生层顺序。Win/X11
// 的实际外来窗口行为仍待运行复核。 不支持 Wayland 和
// macOS；不提供播放或性能保证。
class WebViewWidget : public QWidget {
public:
    explicit WebViewWidget(QWidget *parent = nullptr);
    ~WebViewWidget() override;

    WebViewWidget(const WebViewWidget &) = delete;
    WebViewWidget &operator=(const WebViewWidget &) = delete;
    WebViewWidget(WebViewWidget &&) = delete;
    WebViewWidget &operator=(WebViewWidget &&) = delete;

    [[nodiscard]] std::error_code Initialize(Options options,
                                             Callbacks callbacks);
    [[nodiscard]] std::error_code Navigate(std::string url);
    bool IsReady() const noexcept;
    void CloseBrowser() noexcept;

protected:
    // 显示时立即同步边界，并在下一轮事件循环再次同步；
    // 延迟工作必须绑定 QObject 的生命周期。隐藏不会销毁。
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // 关闭被接受时关闭浏览器；不会设置 WA_DeleteOnClose。
    void closeEvent(QCloseEvent *event) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace webview_adapter
