#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 MasakatsuFunaki
#
# Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
# Not licensed for use as training data for machine learning or generative
# AI systems; text and data mining rights are reserved.  See NOTICE.

"""Standalone build and test driver for tools/llm_client (x64).

    python build.py                # configure and build library + test binaries
    python build.py --test         # build and run UT, CT, and live SYSTEM suites
    python build.py --clean        # remove build/ and CMakeUserPresets.json
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.realpath(__file__))
BUILD_DIR = os.path.join(ROOT, "build")
CONFIG = "Release"
PRESET = "conan-windows-x64-release"


def run(cmd, cwd, check=True):
    rel = os.path.relpath(cwd, ROOT)
    print(f"\n> {subprocess.list2cmdline(cmd)}   [{rel}]")
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
    tiers = [
        ("UT", False),
        ("CT", False),
        ("SYSTEM", True),
    ]
    for label, verbose in tiers:
        print(f"\n=== vn::llm_client (x64): {label} suite ===")
        cmd = ["ctest", "--preset", PRESET, "-C", CONFIG, "--output-on-failure", "-L", label]
        if verbose:
            cmd.append("-V")
        rc = run(cmd, ROOT, check=False)
        if rc != 0:
            print(f"\nvn::llm_client: {label} TESTS FAILED (rc={rc}) -- stopping.")
            sys.exit(rc)


def main():
    parser = argparse.ArgumentParser(
        description="Build and test the shared vn::llm_client C++17 library (x64).")
    parser.add_argument("--test", action="store_true",
                        help="run UT, CT, and live SYSTEM test suites")
    parser.add_argument("--clean", action="store_true",
                        help="delete the build tree and stop")
    args = parser.parse_args()

    if args.clean:
        clean()
        if not args.test:
            return

    build()

    if args.test:
        test()

    print("\nDone.")


if __name__ == "__main__":
    main()
