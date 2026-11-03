# Conan 2 recipe for the kishin_hisho tooling (C++ port of 01_patch.py,
# helper_dll/build.py and the six analysis/ RE scripts).
#
# This game is NOT a translation pipeline, so there is no anthropic client and
# no Boost.JSON/Regex/Beast here.
#
#   boost     - Program_options only
#   capstone  - x86-32 disassembly, replacing the Python `capstone` binding
#   gtest     - the tests/ suites; test-only, never linked into the apps
#
# PE parsing replaces the `pefile` package and is hand-rolled in src/pe/ --
# pefile's get_memory_mapped_image() layout is reproduced exactly because the
# tools index it by RVA.

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class KishinHishoTools(ConanFile):
    name = "kishin_hisho_tools"
    version = "1.0.0"
    package_type = "application"
    description = "Kishin Hishou Demonbane AlphaROM patcher + RE tooling"

    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    exports_sources = "CMakeLists.txt", "CMakePresets.json", "src/*", "tests/*"

    def requirements(self):
        self.requires("boost/1.86.0")
        self.requires("capstone/5.0")
        self.requires("zlib/1.3.1")

    def build_requirements(self):
        # test_requires keeps gtest off the apps' link line entirely.
        if self.options.with_tests:
            self.test_requires("gtest/1.14.0")

    def layout(self):
        # build_folder="." because the output folder IS the binary directory:
        # `conan install . --output-folder=<game>/build/pipeline` then puts the
        # toolchain in <game>/build/pipeline/generators, which is exactly where
        # CMakePresets.json looks for it. The default ("build") would bury it
        # one level deeper and the preset would not find it.
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
