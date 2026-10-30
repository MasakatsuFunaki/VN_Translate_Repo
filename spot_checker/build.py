"""spot_checker build driver -- Conan 2 + CMake + MSVC (x64).

    python build.py                  # build everything, tests included
    python build.py --test           # build, then run all the tests
    python build.py --install        # build, then stage 01_spot_check.exe into build/install/bin
    python build.py --clean          # delete the build tree
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.realpath(__file__))
BUILD_DIR = os.path.join(ROOT, "build")
INSTALL_DIR = os.path.join(BUILD_DIR, "install")
CONFIG = "Release"
PRESET = "conan-windows-x64-release"


def run(cmd, cwd, check=True):
    print(f"\n> {subprocess.list2cmdline(cmd)}   [{os.path.relpath(cwd, ROOT)}]")
    rc = subprocess.run(cmd, cwd=cwd).returncode
    if check and rc != 0:
        sys.exit(rc)
    return rc


def run_with_vcvars(name, cmd_args, cwd):
    vcvars = os.path.join(BUILD_DIR, "generators", "conanvcvars.bat")
    wrap_dir = os.path.join(BUILD_DIR, "_wrappers")
    os.makedirs(wrap_dir, exist_ok=True)
    bat_path = os.path.join(wrap_dir, f"{name}.bat")
    with open(bat_path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("@echo off\n")
        f.write(f'call "{vcvars}" || exit /b 1\n')
        f.write(f'cd /d "{cwd}" || exit /b 1\n')
        f.write(f"{subprocess.list2cmdline(cmd_args)}\n")
    return run([bat_path], cwd)


def clean():
    if os.path.isdir(BUILD_DIR):
        print(f"Removing {BUILD_DIR}")
        shutil.rmtree(BUILD_DIR)
    else:
        print("Nothing to clean.")

    user_presets = os.path.join(ROOT, "CMakeUserPresets.json")
    if os.path.exists(user_presets):
        os.remove(user_presets)


def build():
    run([
        "conan", "install", ROOT,
        f"--output-folder={BUILD_DIR}",
        "--build=missing",
        "-pr=default",
        f"-s=build_type={CONFIG}",
        "-c", "tools.cmake.cmaketoolchain:generator=Ninja Multi-Config",
    ], ROOT)
    run_with_vcvars("configure", ["cmake", "--preset", PRESET], ROOT)
    run_with_vcvars("build", ["cmake", "--build", "--preset", PRESET, "--config", CONFIG], ROOT)


def test():
    print("\n=== spot_checker (x64): tests ===")
    rc = run(["ctest", "--preset", PRESET, "-C", CONFIG, "--output-on-failure"],
             ROOT, check=False)
    if rc != 0:
        print(f"\nspot_checker: TESTS FAILED (rc={rc}) -- stopping.")
        sys.exit(rc)


def install():
    print(f"\nInstalling to {INSTALL_DIR}...")
    run(["cmake", "--install", BUILD_DIR, "--config", CONFIG, "--prefix", INSTALL_DIR], ROOT)
    bin_dir = os.path.join(INSTALL_DIR, "bin")
    if os.path.isdir(bin_dir):
        for name in sorted(os.listdir(bin_dir)):
            print(f"  bin/{name}")


def main():
    parser = argparse.ArgumentParser(
        description="Build the standalone spot_checker tool (x64).")
    parser.add_argument("--test", action="store_true",
                        help="run all the unit tests after building")
    parser.add_argument("--install", action="store_true",
                        help="stage 01_spot_check.exe into build/install/bin")
    parser.add_argument("--clean", action="store_true",
                        help="delete the build tree and stop")
    args = parser.parse_args()

    if args.clean:
        clean()
        return

    build()

    if args.test:
        test()

    if args.install:
        install()

    print("\nDone.")


if __name__ == "__main__":
    main()
