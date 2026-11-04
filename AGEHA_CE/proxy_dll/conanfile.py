# Conan 2 recipe for the AGEHA_CE proxy DLL (winmm.dll).

from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class AgehaProxy(ConanFile):
    name = "ageha_proxy"
    version = "1.0.0"
    package_type = "shared-library"
    description = "AGEHA ~EXTRAVAGANZA~ CE winmm.dll proxy (runtime text hook)"

    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    exports_sources = "CMakeLists.txt", "CMakePresets.json", "src/*", "tests/*"

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
