add_library(webview_adapter_native INTERFACE)
add_library(webview_adapter::native ALIAS webview_adapter_native)
set_target_properties(webview_adapter_native PROPERTIES EXPORT_NAME native)
target_compile_features(webview_adapter_native INTERFACE cxx_std_17)
target_include_directories(
    webview_adapter_native INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                                     $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
install(TARGETS webview_adapter_native EXPORT webview_adapterTargets)
install(FILES webview_adapter/native/webview.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/webview_adapter/native")

if(WEBVIEW_ADAPTER_WITH_QT)
    find_package(Qt5 COMPONENTS Widgets REQUIRED)
    add_library(webview_adapter_qt INTERFACE)
    add_library(webview_adapter::qt ALIAS webview_adapter_qt)
    set_target_properties(webview_adapter_qt PROPERTIES EXPORT_NAME qt)
    target_link_libraries(webview_adapter_qt INTERFACE webview_adapter_native Qt5::Widgets)
    install(TARGETS webview_adapter_qt EXPORT webview_adapterTargets)
    install(FILES webview_adapter/qt/webview_widget.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/webview_adapter/qt")
endif()
