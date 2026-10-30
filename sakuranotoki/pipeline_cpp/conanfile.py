# Conan 2 recipe for the sakuranotoki translation pipeline (C++ port of the
# former Python scripts 00..04 + implementation/pfs_archive.py).
#
#   boost    - Beast/Asio (Gemini HTTPS client), JSON, Program_options,
#              Regex
#   openssl  - TLS for generativelanguage.googleapis.com AND the SHA-1 that
#              derives the PF8 archive XOR key
#   stb      - PNG decode + encode for the narrative-CG scanner (step 04)
#   gtest    - the tests/ suites; test-only, never linked into the apps
#
# The gtest dependency is gated on the same switch CMake uses for the test
# tree (-o with_tests=False skips both fetching gtest and building tests/).

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class SakuraNoTokiPipeline(ConanFile):
    name = "sakuranotoki_pipeline"
    version = "1.0.0"
    package_type = "application"
    description = "Sakura no Toki English translation pipeline"

    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    exports_sources = "CMakeLists.txt", "CMakePresets.json", "src/*", "tests/*"

    def requirements(self):
        self.requires("boost/1.86.0")
        self.requires("openssl/3.3.2")
        self.requires("stb/cci.20240531")

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
