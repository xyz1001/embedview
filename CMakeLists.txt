cmake_minimum_required(VERSION 3.25)
project(webview_adapter LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WEBVIEW_ADAPTER_WITH_QT "Expose the placeholder Qt interface target" OFF)

add_subdirectory(src)

set(webview_adapter_config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/webview_adapter")
set(webview_adapter_config "${CMAKE_CURRENT_BINARY_DIR}/webview_adapterConfig.cmake")
file(WRITE "${webview_adapter_config}" "include(CMakeFindDependencyMacro)\n")
if(WEBVIEW_ADAPTER_WITH_QT)
    file(APPEND "${webview_adapter_config}" "find_dependency(Qt5 COMPONENTS Widgets)\n")
endif()
file(APPEND "${webview_adapter_config}" "include(\"\${CMAKE_CURRENT_LIST_DIR}/webview_adapterTargets.cmake\")\n")

install(
    EXPORT webview_adapterTargets
    NAMESPACE webview_adapter::
    DESTINATION "${webview_adapter_config_dir}"
)
install(FILES "${webview_adapter_config}" DESTINATION "${webview_adapter_config_dir}")
