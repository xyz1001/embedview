#pragma once

// 平台共享库符号的导入和导出标记。
#if defined(_WIN32) || defined(__CYGWIN__)
#define WEBVIEW_ADAPTER_HELPER_DLL_IMPORT __declspec(dllimport)
#define WEBVIEW_ADAPTER_HELPER_DLL_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#define WEBVIEW_ADAPTER_HELPER_DLL_IMPORT __attribute__((visibility("default")))
#define WEBVIEW_ADAPTER_HELPER_DLL_EXPORT __attribute__((visibility("default")))
#else
#define WEBVIEW_ADAPTER_HELPER_DLL_IMPORT
#define WEBVIEW_ADAPTER_HELPER_DLL_EXPORT
#endif

// 静态库不需要符号标记；两个组件分别控制导入和导出。
#if defined(WEBVIEW_ADAPTER_NATIVE_DLL)
#if defined(WEBVIEW_ADAPTER_NATIVE_DLL_EXPORTS)
#define WEBVIEW_ADAPTER_NATIVE_API WEBVIEW_ADAPTER_HELPER_DLL_EXPORT
#else
#define WEBVIEW_ADAPTER_NATIVE_API WEBVIEW_ADAPTER_HELPER_DLL_IMPORT
#endif
#else
#define WEBVIEW_ADAPTER_NATIVE_API
#endif

#if defined(WEBVIEW_ADAPTER_QT_DLL)
#if defined(WEBVIEW_ADAPTER_QT_DLL_EXPORTS)
#define WEBVIEW_ADAPTER_QT_API WEBVIEW_ADAPTER_HELPER_DLL_EXPORT
#else
#define WEBVIEW_ADAPTER_QT_API WEBVIEW_ADAPTER_HELPER_DLL_IMPORT
#endif
#else
#define WEBVIEW_ADAPTER_QT_API
#endif
