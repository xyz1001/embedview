#pragma once

// 平台共享库符号的导入和导出标记。
#if defined(_WIN32) || defined(__CYGWIN__)
#define EMBEDVIEW_HELPER_DLL_IMPORT __declspec(dllimport)
#define EMBEDVIEW_HELPER_DLL_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#define EMBEDVIEW_HELPER_DLL_IMPORT __attribute__((visibility("default")))
#define EMBEDVIEW_HELPER_DLL_EXPORT __attribute__((visibility("default")))
#else
#define EMBEDVIEW_HELPER_DLL_IMPORT
#define EMBEDVIEW_HELPER_DLL_EXPORT
#endif

// 静态库不需要符号标记；两个组件分别控制导入和导出。
#if defined(EMBEDVIEW_NATIVE_DLL)
#if defined(EMBEDVIEW_NATIVE_DLL_EXPORTS)
#define EMBEDVIEW_NATIVE_API EMBEDVIEW_HELPER_DLL_EXPORT
#else
#define EMBEDVIEW_NATIVE_API EMBEDVIEW_HELPER_DLL_IMPORT
#endif
#else
#define EMBEDVIEW_NATIVE_API
#endif

#if defined(EMBEDVIEW_QT_DLL)
#if defined(EMBEDVIEW_QT_DLL_EXPORTS)
#define EMBEDVIEW_QT_API EMBEDVIEW_HELPER_DLL_EXPORT
#else
#define EMBEDVIEW_QT_API EMBEDVIEW_HELPER_DLL_IMPORT
#endif
#else
#define EMBEDVIEW_QT_API
#endif
