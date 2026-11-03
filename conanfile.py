from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class WebviewAdapterConan(ConanFile):
    name = "webview-adapter"
    version = "0.1.2"
    package_type = "static-library"
    description = "C++ webview adapter"
    settings = "os", "compiler", "build_type", "arch"
    options = {"with_qt": [True, False]}
    default_options = {"with_qt": False}
    exports_sources = "CMakeLists.txt", "src/*"

    def validate(self):
        if self.settings.os not in ("Windows", "Linux"):
            raise ConanInvalidConfiguration("webview-adapter 仅支持 Windows 和 Linux")

    def requirements(self):
        self.requires("webview/0.12.0")
        if self.settings.os == "Linux":
            self.requires("gtk/system")
            self.requires("webkit2gtk/system")
        if self.options.with_qt:
            self.requires("qt/5.15.18")

    def layout(self):
        cmake_layout(self)

    def _webview_component(self):
        components = self.dependencies["webview"].cpp_info.components
        for name in ("c_api", "core_static", "core_shared", "core"):
            if name in components:
                component = components[name]
                if (
                    not component.libs
                    or component.get_property("cmake_target_name") != f"webview::{name}"
                ):
                    raise ConanInvalidConfiguration(
                        f"webview::{name} 未提供受支持的已编译库"
                    )
                return name
        raise ConanInvalidConfiguration("webview 包未提供受支持的已编译 C API 组件")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["BUILD_SHARED_LIBS"] = False
        tc.variables["WEBVIEW_ADAPTER_WITH_QT"] = self.options.with_qt
        # 仅在 Conan 组件确实声明了库时，允许 CMakeDeps 的 core 接口包装目标。
        tc.variables["WEBVIEW_ADAPTER_CONAN_CORE_HAS_LIBRARY"] = (
            self._webview_component() == "core"
        )
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        CMake(self).install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "webview-adapter")
        native = self.cpp_info.components["native"]
        native.set_property("cmake_target_name", "webview_adapter::native")
        native.libs = ["webview_adapter_native"]
        # 与生成阶段检查的实际组件保持一致，传递静态库所需的链接依赖。
        native.requires = [f"webview::{self._webview_component()}"]
        if self.settings.os == "Windows":
            native.system_libs = [
                "advapi32",
                "ole32",
                "shell32",
                "shlwapi",
                "user32",
                "version",
            ]
        elif self.settings.os == "Linux":
            native.requires.extend(["gtk::gtk", "webkit2gtk::webkit2gtk"])
        if self.options.with_qt:
            qt = self.cpp_info.components["qt"]
            qt.set_property("cmake_target_name", "webview_adapter::qt")
            qt.libs = ["webview_adapter_qt"]
            qt.requires = ["native", "qt::qtWidgets"]
