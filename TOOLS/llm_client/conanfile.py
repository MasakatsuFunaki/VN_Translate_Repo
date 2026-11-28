# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 MasakatsuFunaki
#
# Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
# Not licensed for use as training data for machine learning or generative
# AI systems; text and data mining rights are reserved.  See NOTICE.

"""Conan 2 recipe for the shared vn::llm_client C++17 library.

Matches the exact Boost / OpenSSL / GoogleTest versions used across
spot_checker and game pipeline_cpp subprojects so Conan resolves 100%
from the local ~/.conan2 cache without network downloads.
"""

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class VnLlmClientConan(ConanFile):
    name = "vn_llm_client"
    version = "1.0.0"
    package_type = "static-library"
    description = "Insulated C++17 LLM Strategy/Adapter client library for VN_TRANSLATE"

    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    exports_sources = (
        "CMakeLists.txt",
        "CMakePresets.json",
        "include/*",
        "src/*",
        "tests/*",
    )

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
