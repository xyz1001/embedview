find_package(webview CONFIG REQUIRED)
if(TARGET webview::c_api)
    set(webview_adapter_webview_target webview::c_api)
elseif(TARGET webview::core_static)
    set(webview_adapter_webview_target webview::core_static)
elseif(TARGET webview::core_shared)
    set(webview_adapter_webview_target webview::core_shared)
elseif(TARGET webview::core)
    get_target_property(webview_adapter_core_type webview::core TYPE)
    get_target_property(webview_adapter_core_imported webview::core IMPORTED)
    get_target_property(webview_adapter_core_links webview::core INTERFACE_LINK_LIBRARIES)
    # Conan 配方的 core 组件有实际库时，CMakeDeps 以导入的接口目标包装它。
    if(webview_adapter_core_type STREQUAL "STATIC_LIBRARY" OR webview_adapter_core_type STREQUAL "SHARED_LIBRARY")
        set(webview_adapter_webview_target webview::core)
    elseif(
        WEBVIEW_ADAPTER_CONAN_CORE_HAS_LIBRARY
        AND webview_adapter_core_imported
        AND webview_adapter_core_links
    )
        set(webview_adapter_webview_target webview::core)
    else()
        message(FATAL_ERROR "webview::core 未确认包含已编译的 C API，拒绝仅头文件目标")
    endif()
else()
    message(FATAL_ERROR "webview 包未提供受支持的已编译 C API 目标")
endif()

add_library(webview_adapter_native STATIC webview_adapter/native/webview.cpp)
add_library(webview_adapter::native ALIAS webview_adapter_native)
set_target_properties(webview_adapter_native PROPERTIES EXPORT_NAME native)
target_compile_features(webview_adapter_native PUBLIC cxx_std_17)
target_include_directories(
    webview_adapter_native PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(webview_adapter_native PRIVATE ${webview_adapter_webview_target})

if(WIN32)
    target_sources(webview_adapter_native PRIVATE webview_adapter/native/backend_win.cpp)
    target_link_libraries(webview_adapter_native PRIVATE advapi32 ole32 shell32 shlwapi user32 version)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(gtk CONFIG REQUIRED)
    find_package(webkit2gtk CONFIG REQUIRED)
    target_sources(webview_adapter_native PRIVATE webview_adapter/native/backend_linux.cpp)
    target_link_libraries(webview_adapter_native PRIVATE gtk::gtk webkit2gtk::webkit2gtk)
endif()

install(
    TARGETS webview_adapter_native
    EXPORT webview_adapterTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(
    FILES webview_adapter/native/webview.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/webview_adapter/native"
)

if(WEBVIEW_ADAPTER_WITH_QT)
    find_package(
        Qt5
        COMPONENTS Widgets
        REQUIRED
    )
    add_library(webview_adapter_qt STATIC webview_adapter/qt/webview_widget.cpp)
    add_library(webview_adapter::qt ALIAS webview_adapter_qt)
    set_target_properties(webview_adapter_qt PROPERTIES EXPORT_NAME qt)
    target_link_libraries(webview_adapter_qt PUBLIC webview_adapter_native Qt5::Widgets)
    install(
        TARGETS webview_adapter_qt
        EXPORT webview_adapterTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
    install(FILES webview_adapter/qt/webview_widget.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/webview_adapter/qt")
endif()
