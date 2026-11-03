from conan import ConanFile
from conan.tools.cmake import cmake_layout


class AlpharomHelperConan(ConanFile):
    # helper_dll currently has zero third-party dependencies: both targets
    # (alpharom_helper.dll + launch.exe) link only against the Windows system
    # libraries kernel32/user32. Conan is used here purely for consistent
    # tooling (CMakeToolchain + CMakeDeps + cmake_layout) so the standard
    # 3-step flow works and external libraries can be added later without
    # restructuring the build.
    #
    # Everything is Win32: the profile in profiles/x86 pins arch=x86, because
    # both binaries live in the 32-bit game process.
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"

    def layout(self):
        # build_folder="." because the output folder IS the binary directory:
        # `conan install . --output-folder=<game>/build/helper` then puts the
        # toolchain in <game>/build/helper/generators, which is exactly where
        # CMakePresets.json looks for it. The default ("build") would bury it
        # one level deeper and the preset would not find it.
        cmake_layout(self, build_folder=".")
