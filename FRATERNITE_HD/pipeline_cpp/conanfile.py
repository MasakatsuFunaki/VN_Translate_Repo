# Conan 2 recipe for the FRATERNITE_HD translation pipeline.
# boost (Beast/JSON/PO), openssl (TLS), zlib (YPF), gtest (test-only).

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class FraternitePipeline(ConanFile):
    name = "fraternite_pipeline"
    version = "1.0.0"
    package_type = "application"
    description = "Fraternite HD Remaster English translation pipeline"

    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    exports_sources = "CMakeLists.txt", "CMakePresets.json", "src/*", "tests/*"

    def requirements(self):
        self.requires("boost/1.86.0")
        self.requires("openssl/3.3.2")
        self.requires("zlib/1.3.1")

    def build_requirements(self):
        if self.options.with_tests:
            self.test_requires("gtest/1.14.0")

    def layout(self):
        # "." so the toolchain lands where CMakePresets.json expects it.
        cmake_layout(self, build_folder=".")

    def generate(self):
        CMakeDeps(self).generate()
        tc = CMakeToolchain(self)
        tc.cache_variables["ENABLE_TESTS"] = bool(self.options.with_tests)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if self.options.with_tests:
            cmake.test()
