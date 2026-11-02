from conan import ConanFile
from conan.tools.files import copy
import os


class WebviewAdapterConan(ConanFile):
    name = "webview-adapter"
    package_type = "header-library"
    description = "Interface-stage skeleton for a webview adapter"
    settings = "os", "compiler", "build_type", "arch"
    options = {"with_qt": [True, False]}
    default_options = {"with_qt": False}
    exports_sources = (
        "README.md",
        "CMakeLists.txt",
        "src/CMakeLists.txt",
        "src/webview_adapter/native/webview.h",
        "src/webview_adapter/qt/webview_widget.h",
        "doc/interface_design.md",
    )

    def requirements(self):
        # Reserved for a future private backend; the draft public API does not use it.
        self.requires("webview/0.12.0")
        if self.options.with_qt:
            self.requires("qt/5.15.18")

    def package(self):
        copy(self, "README.md", src=self.source_folder, dst=os.path.join(self.package_folder, "res"))
        copy(self, "interface_design.md", src=os.path.join(self.source_folder, "doc"),
             dst=os.path.join(self.package_folder, "res", "doc"))
        copy(self, "webview.h", src=os.path.join(self.source_folder, "src", "webview_adapter", "native"),
             dst=os.path.join(self.package_folder, "include", "webview_adapter", "native"))
        if self.options.with_qt:
            copy(self, "webview_widget.h", src=os.path.join(self.source_folder, "src", "webview_adapter", "qt"),
                 dst=os.path.join(self.package_folder, "include", "webview_adapter", "qt"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "webview-adapter")
        native = self.cpp_info.components["native"]
        native.set_property("cmake_target_name", "webview_adapter::native")
        native.includedirs = ["include"]
        native.libdirs = []
        if self.options.with_qt:
            qt = self.cpp_info.components["qt"]
            qt.set_property("cmake_target_name", "webview_adapter::qt")
            qt.includedirs = ["include"]
            qt.libdirs = []
            qt.requires = ["native"]
