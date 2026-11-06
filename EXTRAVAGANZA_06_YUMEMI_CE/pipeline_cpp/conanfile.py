# Conan 2 recipe for the YUMEMIRYOJOKU_CE translation pipeline.
#
#   boost    - JSON, Program_options, Regex, headers
#   openssl  - TLS / crypto primitives
#   gtest    - the tests/ suites; test-only, never linked into the apps

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class YumemiPipeline(ConanFile):
    name = "yumemi_pipeline"
    version = "1.0.0"
    package_type = "application"
    description = "YUMEMIRYOJOKU_CE English translation pipeline"

    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    exports_sources = "CMakeLists.txt", "CMakePresets.json", "src/*", "tests/*"

    def requirements(self):
        self.requires("boost/1.86.0")
        self.requires("openssl/3.3.2")

    def build_requirements(self):
        if self.options.with_tests:
            self.test_requires("gtest/1.14.0")

    def layout(self):
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
