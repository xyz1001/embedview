cmake_minimum_required(VERSION 3.25)
project(webview_adapter LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "webview-adapter 仅支持 Windows 和 Linux")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(WEBVIEW_ADAPTER_WITH_QT "构建 Qt 封装" OFF)

add_subdirectory(src)

set(webview_adapter_config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/webview_adapter")
set(webview_adapter_config "${CMAKE_CURRENT_BINARY_DIR}/webview_adapterConfig.cmake")
file(WRITE "${webview_adapter_config}" "include(CMakeFindDependencyMacro)\n")
file(APPEND "${webview_adapter_config}" "find_dependency(webview CONFIG)\n")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(APPEND "${webview_adapter_config}" "find_dependency(gtk CONFIG)\nfind_dependency(webkit2gtk CONFIG)\n")
endif()
if(WEBVIEW_ADAPTER_WITH_QT)
    file(APPEND "${webview_adapter_config}" "find_dependency(Qt5 COMPONENTS Widgets)\n")
endif()
file(APPEND "${webview_adapter_config}" "include(\"\${CMAKE_CURRENT_LIST_DIR}/webview_adapterTargets.cmake\")\n")

install(
    EXPORT webview_adapterTargets
    NAMESPACE webview_adapter::
    DESTINATION "${webview_adapter_config_dir}"
)
install(
    FILES "${webview_adapter_config}"
    DESTINATION "${webview_adapter_config_dir}"
)
