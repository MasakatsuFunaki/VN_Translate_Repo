# Conan 2 recipe for the Shingakkou translation pipeline (C++ port of the
# former Python scripts 00..03).
#
#   boost    - Beast/Asio (Anthropic HTTPS client), JSON, Program_options,
#              Regex
#   openssl  - TLS for api.anthropic.com.  No crypto beyond that: the archive
#              cipher is an 8-line repeating XOR and ShsCompression is
#              hand-rolled, so there is no zlib / Blowfish dependency here.
#   freetype - TTF measurement/rasterisation for the narrative-CG renderer
#   stb      - image decode/encode for the narrative-CG renderer
#   gtest    - the tests/ suites; test-only, never linked into the apps
#
# The gtest dependency is gated on the same switch CMake uses for the test
# tree (-o with_tests=False skips both fetching gtest and building tests/).

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class ShingakkouPipeline(ConanFile):
    name = "shingakkou_pipeline"
    version = "1.0.0"
    package_type = "application"
    description = "Shingakkou ~Noli me tangere~ English translation pipeline"

    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    exports_sources = "CMakeLists.txt", "CMakePresets.json", "src/*", "tests/*"

    def requirements(self):
        self.requires("boost/1.86.0")
        self.requires("openssl/3.3.2")
        # Narrative-CG step only: FreeType replaces PIL ImageFont (measurement
        # + rasterisation) and stb replaces PIL Image (BMP/PNG codecs).
        self.requires("freetype/2.13.2")
        self.requires("stb/cci.20240531")

    def build_requirements(self):
        # test_requires keeps gtest off the apps' link line entirely.
        if self.options.with_tests:
            self.test_requires("gtest/1.14.0")

    def layout(self):
        # build_folder="." because the output folder IS the binary directory:
        # `conan install . --output-folder=<game>/build/pipeline` then puts the
        # toolchain in <game>/build/pipeline/generators, which is exactly where
        # CMakePresets.json looks for it.  The default ("build") would bury it
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
