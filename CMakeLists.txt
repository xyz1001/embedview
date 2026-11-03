cmake_minimum_required(VERSION 3.25)
project(embedview LANGUAGES CXX)

include(GNUInstallDirs)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "embedview 仅支持 Windows 和 Linux")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(EMBEDVIEW_WITH_QT "构建 Qt 封装" OFF)
# 兼容旧变量名
if(DEFINED WEBVIEW_ADAPTER_WITH_QT)
    set(EMBEDVIEW_WITH_QT ${WEBVIEW_ADAPTER_WITH_QT})
endif()

add_subdirectory(src)

set(embedview_config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/embedview")
set(embedview_config "${CMAKE_CURRENT_BINARY_DIR}/embedviewConfig.cmake")
file(WRITE "${embedview_config}" "include(CMakeFindDependencyMacro)\n")
file(APPEND "${embedview_config}" "find_dependency(webview CONFIG)\n")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(APPEND "${embedview_config}" "find_dependency(gtk CONFIG)\nfind_dependency(webkit2gtk CONFIG)\n")
endif()
if(EMBEDVIEW_WITH_QT)
    file(APPEND "${embedview_config}" "find_dependency(Qt5 COMPONENTS Widgets)\n")
endif()
file(APPEND "${embedview_config}" "include(\"\${CMAKE_CURRENT_LIST_DIR}/embedviewTargets.cmake\")\n")

install(
    EXPORT embedviewTargets
    NAMESPACE embedview::
    DESTINATION "${embedview_config_dir}"
)
install(
    FILES "${embedview_config}"
    DESTINATION "${embedview_config_dir}"
)
