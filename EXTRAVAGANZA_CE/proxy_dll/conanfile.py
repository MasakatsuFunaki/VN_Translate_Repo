# Conan 2 recipe for the EXTRAVAGANZA_CE proxy DLL (winmm.dll).
#
# The DLL itself has no external dependencies: it builds against plain Win32
# and the MSVC runtime. gtest is here for the test pyramid only, gated on the
# same switch CMake uses for the test tree (-o with_tests=False skips both
# fetching gtest and building tests/).
#
# Everything is Win32: the profile in profiles/x86 pins arch=x86, because the
# DLL is injected into mushiEx_CE.exe, a 32-bit game process.
#
# Replaces the former conanfile.txt, which could not express the layout the
# game's single build tree needs.

from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class ExtravaganzaCeProxy(ConanFile):
    name = "extravaganza_ce_proxy"
    version = "1.0.0"
    package_type = "shared-library"
    description = "EXTRAVAGANZA CE winmm.dll proxy (runtime font patch)"

    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    exports_sources = "CMakeLists.txt", "CMakePresets.json", "src/*", "tests/*"

    def build_requirements(self):
        # test_requires keeps gtest off the DLL's link line entirely.
        if self.options.with_tests:
            self.test_requires("gtest/1.14.0")

    def layout(self):
        # build_folder="." because the output folder IS the binary directory:
        # `conan install . --output-folder=<game>/build/proxy` then puts the
        # toolchain in <game>/build/proxy/generators, which is exactly where
        # CMakePresets.json looks for it. The default ("build") would bury it
        # one level deeper and the preset would not find it.
        cmake_layout(self, build_folder=".")

    def generate(self):
        CMakeDeps(self).generate()
        tc = CMakeToolchain(self)
        tc.cache_variables["ENABLE_TESTS"] = bool(self.options.with_tests)
        tc.generate()
