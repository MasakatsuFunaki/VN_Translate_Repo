"""Mushigurui HD10 build driver — Conan 2 + CMake + MSVC.

    python build.py                    # build everything, tests included
    python build.py --test             # build, then run all the tests
    python build.py --install          # build, then stage what the user runs
    python build.py --deploy           # build, then ship the artifacts to the game
    python build.py --test --deploy    # build, test, and ship only if the tests pass
    python build.py --clean            # delete the build tree

Release only.  mushigurui_HD10.exe is a 32-bit BLACKCyc (nnn/BCmkri) binary,
so the two halves are built for different architectures and that is the trap
this script exists to hide: `pipeline_cpp` is x64 (the SPT/GPK tools and the
Anthropic client), `proxy_dll` is Win32 because winmm.dll is loaded into the
game process — its CMakeLists hard-fails on a 64-bit configure.  One CMake
project cannot hold both, so the root CMakeLists.txt drives each half as a
separate sub-build with its own Conan profile and toolchain.

One build folder for the whole game:

    build/pipeline/   pipeline_cpp, x64
    build/proxy/      proxy_dll, Win32
    build/install/    what --install stages: bin/ to run, game/ to copy

A bare run builds every binary including the test executables; `--test` also
runs them: the pipeline's `ut_*` suites and the DLL's UT/CT/SYSTEM tiers.
When `--test` and `--deploy` are given together the tests gate the deploy, so
a regression is never shipped.

Deploying copies `winmm.dll` into C:\\Games\\mushigurui_HD10.  That is the
whole payload for this game: `translation_table.tsv` (and the translated XTX
config files) are written straight into the game folder by the pipeline
executables themselves, so there is nothing at the repo root to copy — the
deploy only warns when the table is not there yet.  Set VN_DIST_BUILD to skip
the copy, because a distribution build stages the artifacts itself and may run
where no game is installed.

This script builds; it never runs the pipeline executables.  Translating calls
a paid API, so that stays an explicit, separate action.
"""

import os
import sys
import shutil
import argparse
import subprocess

ROOT = os.path.dirname(os.path.realpath(__file__))
BUILD_DIR = os.path.join(ROOT, "build")
INSTALL_DIR = os.path.join(BUILD_DIR, "install")
GAME_DIR = r"C:\Games\mushigurui_HD10"
CONFIG = "Release"
PRESET = "windows-release"

# The halves, for the steps CMake does not own.  ctest is per sub-build:
# each half has its own binary dir and its own test preset, and a single
# ctest run cannot span the two.  Order matters — the pipeline generates
# translation_table.tsv, which the DLL reads at runtime.
HALVES = [
    ("pipeline_cpp (x64)", os.path.join(ROOT, "pipeline_cpp"),
     "conan-windows-x64-release"),
    ("proxy_dll (Win32)", os.path.join(ROOT, "proxy_dll"),
     "conan-windows-x86-release"),
]


def run(cmd, cwd, check=True):
    print(f"\n> {subprocess.list2cmdline(cmd)}   [{os.path.relpath(cwd, ROOT)}]")
    rc = subprocess.run(cmd, cwd=cwd).returncode
    if check and rc != 0:
        sys.exit(rc)
    return rc


def clean():
    if os.path.isdir(BUILD_DIR):
        print(f"Removing {BUILD_DIR}")
        shutil.rmtree(BUILD_DIR)
    else:
        print("Nothing to clean.")

    # Conan rewrites these on every install and they point into the tree we
    # just deleted; a stale one makes `cmake --preset` fail on a file that no
    # longer exists, which reads like a broken preset rather than a stale one.
    for _, source_dir, _ in HALVES:
        user_presets = os.path.join(source_dir, "CMakeUserPresets.json")
        if os.path.exists(user_presets):
            os.remove(user_presets)


def build():
    # The root project configures and builds both halves: it runs Conan for
    # each, configures each through its own preset, and builds each in its
    # own binary dir under build/.
    run(["cmake", "--preset", PRESET], ROOT)
    run(["cmake", "--build", "--preset", PRESET, "--config", CONFIG], ROOT)


def has_test_preset(source_dir, preset):
    """True when the half declares a ctest preset by that name.

    A half without one has nothing for ctest to run, and asking anyway is a
    hard error rather than an empty run.
    """
    listing = subprocess.run(["ctest", "--list-presets"], cwd=source_dir,
                             capture_output=True, text=True)
    return listing.returncode == 0 and f'"{preset}"' in listing.stdout


def test():
    for label, source_dir, preset in HALVES:
        print(f"\n=== {label}: tests ===")
        if not has_test_preset(source_dir, preset):
            print(f"{label}: no test preset '{preset}' — skipping.")
            continue
        rc = run(["ctest", "--preset", preset, "-C", CONFIG, "--output-on-failure"],
                 source_dir, check=False)
        if rc != 0:
            print(f"\n{label}: TESTS FAILED (rc={rc}) — stopping.")
            sys.exit(rc)


def install():
    print(f"\nInstalling to {INSTALL_DIR}...")
    run(["cmake", "--install", BUILD_DIR, "--config", CONFIG], ROOT)

    for folder in ("bin", "game"):
        path = os.path.join(INSTALL_DIR, folder)
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                print(f"  {folder}/{name}")


def deploy():
    if os.environ.get("VN_DIST_BUILD"):
        print("[DIST] VN_DIST_BUILD set — skipping deploy to the game folder.")
        return

    print(f"\nDeploying to {GAME_DIR}...")

    # From the build tree, not the install tree: --deploy has to work whether
    # or not --install was asked for, and the build tree is always the fresher
    # of the two.
    # The DLL is a hard failure: the usual cause is the game holding it open.
    dll = os.path.join(BUILD_DIR, "proxy", CONFIG, "winmm.dll")
    try:
        shutil.copyfile(dll, os.path.join(GAME_DIR, "winmm.dll"))
    except OSError as exc:
        print(f"COPY winmm.dll FAILED ({exc}) — is the game running? Close it and retry.")
        sys.exit(1)

    # The table is a soft failure: 02_translate.exe writes it into the game
    # folder directly, so a missing one just means the translation pipeline
    # has not been run yet.
    if not os.path.exists(os.path.join(GAME_DIR, "translation_table.tsv")):
        print("WARN: translation_table.tsv not in the game folder — "
              "run 02_translate.exe first.")

    print(f"\nDeployed to {GAME_DIR}.")


def main():
    parser = argparse.ArgumentParser(
        description="Build the Mushigurui HD10 pipeline (x64) and proxy DLL (Win32).")
    parser.add_argument("--test", action="store_true",
                        help="run all the tests after building")
    parser.add_argument("--install", action="store_true",
                        help="stage the user-facing artifacts into build/install")
    parser.add_argument("--deploy", action="store_true",
                        help="copy winmm.dll to the game folder")
    parser.add_argument("--clean", action="store_true",
                        help="delete the build tree and stop")
    args = parser.parse_args()

    if args.clean:
        clean()
        return

    build()

    # Tests run before any staging or deploy, so --test gates both.
    if args.test:
        test()

    if args.install:
        install()

    if args.deploy:
        deploy()

    print("\nDone.")


if __name__ == "__main__":
    main()
