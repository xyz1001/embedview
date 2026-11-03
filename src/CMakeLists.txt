find_package(webview CONFIG REQUIRED)
if(TARGET webview::c_api)
    set(embedview_webview_target webview::c_api)
elseif(TARGET webview::core_static)
    set(embedview_webview_target webview::core_static)
elseif(TARGET webview::core_shared)
    set(embedview_webview_target webview::core_shared)
elseif(TARGET webview::core)
    get_target_property(embedview_core_type webview::core TYPE)
    get_target_property(embedview_core_imported webview::core IMPORTED)
    get_target_property(embedview_core_links webview::core INTERFACE_LINK_LIBRARIES)
    # Conan 配方的 core 组件有实际库时，CMakeDeps 以导入的接口目标包装它。
    if(embedview_core_type STREQUAL "STATIC_LIBRARY" OR embedview_core_type STREQUAL "SHARED_LIBRARY")
        set(embedview_webview_target webview::core)
    elseif(
        EMBEDVIEW_CONAN_CORE_HAS_LIBRARY
        AND embedview_core_imported
        AND embedview_core_links
    )
        set(embedview_webview_target webview::core)
    else()
        message(FATAL_ERROR "webview::core 未确认包含已编译的 C API，拒绝仅头文件目标")
    endif()
else()
    message(FATAL_ERROR "webview 包未提供受支持的已编译 C API 目标")
endif()

add_library(embedview_native embedview/native/webview.cpp)
add_library(embedview::native ALIAS embedview_native)
set_target_properties(embedview_native PROPERTIES EXPORT_NAME native)
if(BUILD_SHARED_LIBS)
    set_target_properties(embedview_native PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_compile_definitions(
        embedview_native
        PUBLIC EMBEDVIEW_NATIVE_DLL
        PRIVATE EMBEDVIEW_NATIVE_DLL_EXPORTS
    )
endif()
target_compile_features(embedview_native PUBLIC cxx_std_17)
target_include_directories(
    embedview_native PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(embedview_native PRIVATE ${embedview_webview_target})

if(WIN32)
    target_sources(embedview_native PRIVATE embedview/native/backend_win.cpp)
    target_link_libraries(embedview_native PRIVATE advapi32 ole32 shell32 shlwapi user32 version)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(gtk CONFIG REQUIRED)
    find_package(webkit2gtk CONFIG REQUIRED)
    target_sources(embedview_native PRIVATE embedview/native/backend_linux.cpp)
    target_link_libraries(embedview_native PRIVATE gtk::gtk webkit2gtk::webkit2gtk)
endif()

install(
    TARGETS embedview_native
    EXPORT embedviewTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
    FILES embedview/export.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/embedview"
)
install(
    FILES embedview/native/webview.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/embedview/native"
)

if(EMBEDVIEW_WITH_QT)
    find_package(
        Qt5
        COMPONENTS Widgets
        REQUIRED
    )
    add_library(embedview_qt embedview/qt/webview_widget.cpp)
    add_library(embedview::qt ALIAS embedview_qt)
    set_target_properties(embedview_qt PROPERTIES EXPORT_NAME qt)
    if(BUILD_SHARED_LIBS)
        set_target_properties(embedview_qt PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
        target_compile_definitions(
            embedview_qt
            PUBLIC EMBEDVIEW_QT_DLL
            PRIVATE EMBEDVIEW_QT_DLL_EXPORTS
        )
    endif()
    target_link_libraries(embedview_qt PUBLIC embedview_native Qt5::Widgets)
    install(
        TARGETS embedview_qt
        EXPORT embedviewTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
    install(FILES embedview/qt/webview_widget.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/embedview/qt")
endif()
