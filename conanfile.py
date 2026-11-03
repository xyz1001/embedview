import os

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy


class EmbedviewConan(ConanFile):
    name = "embedview"
    version = "0.1.3"
    package_type = "library"
    description = "C++ webview adapter library for window embedding"
    license = "MIT"
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False], "with_qt": [True, False]}
    default_options = {"shared": False, "fPIC": True, "with_qt": False}
    exports_sources = "CMakeLists.txt", "LICENSE", "src/*"

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def validate(self):
        if self.settings.os not in ("Windows", "Linux"):
            raise ConanInvalidConfiguration("embedview 仅支持 Windows 和 Linux")

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
        tc.variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        tc.variables["EMBEDVIEW_WITH_QT"] = self.options.with_qt
        # 仅在 Conan 组件确实声明了库时，允许 CMakeDeps 的 core 接口包装目标。
        tc.variables["EMBEDVIEW_CONAN_CORE_HAS_LIBRARY"] = (
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
        copy(self, "LICENSE", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "embedview")
        native = self.cpp_info.components["native"]
        native.set_property("cmake_target_name", "embedview::native")
        native.libs = ["embedview_native"]
        if self.options.shared:
            native.defines = ["EMBEDVIEW_NATIVE_DLL"]
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
            qt.set_property("cmake_target_name", "embedview::qt")
            qt.libs = ["embedview_qt"]
            if self.options.shared:
                qt.defines = ["EMBEDVIEW_QT_DLL"]
            qt.requires = ["native", "qt::qtWidgets"]
