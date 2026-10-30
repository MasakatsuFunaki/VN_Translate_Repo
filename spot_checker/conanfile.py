# Conan 2 recipe for the VN_TRANSLATE standalone spot_checker subproject.
#
#   boost    - Beast/Asio (Gemini HTTPS client), JSON, Program_options, Regex
#   openssl  - TLS for generativelanguage.googleapis.com
#   gtest    - the tests/ suites; test-only, never linked into the apps
#
# Matches the exact versions in CROSS_CHANNEL/pipeline_cpp/conanfile.py so
# Conan resolves from the local cache (~/.conan2) without network downloads.

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class SpotChecker(ConanFile):
    name = "spot_checker"
    version = "1.0.0"
    package_type = "application"
    description = "Reusable sliding-window translation spot-checker powered by gemini-4-argon"

    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    exports_sources = "CMakeLists.txt", "CMakePresets.json", "src/*", "tests/*"

    def requirements(self):
        self.requires("boost/1.86.0")
        self.requires("openssl/3.3.2")

    def build_requirements(self):
        # test_requires keeps gtest off the app's link line entirely.
        if self.options.with_tests:
            self.test_requires("gtest/1.14.0")

    def layout(self):
        # build_folder="." because the output folder IS the binary directory:
        # `conan install . --output-folder=spot_checker/build` puts the
        # toolchain in spot_checker/build/generators, where CMakePresets.json
        # looks for it.
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
