"""CROSS-CHANNEL build driver — Conan 2 + CMake + MSVC.

    python build.py                    # build everything, tests included
    python build.py --test             # build, then run all the tests
    python build.py --install          # build, then stage what the user runs
    python build.py --deploy           # build, then ship the artifacts to the game
    python build.py --test --deploy    # build, test, and ship only if the tests pass
    python build.py --clean            # delete the build tree

Release only.  cc.exe is a 32-bit WillPlus/AdvHD binary, so the two halves are
built for different architectures and that is the trap this script exists to
hide: `pipeline_cpp` is x64 (the tools that read sn.bin and call the API),
`proxy_dll` is Win32 because it is injected into the game process.  One CMake
project cannot hold both, so the root CMakeLists.txt drives each half as a
separate sub-build with its own Conan profile and toolchain.

One build folder for the whole game:

    build/pipeline/   pipeline_cpp, x64
    build/proxy/      proxy_dll, Win32
    build/install/    what --install stages: bin/ to run, game/ to copy

A bare run builds every binary including the test executables; `--test` also
runs them — the x64 GoogleTest suite of the pipeline and the UT/CT/SYSTEM
tiers of the DLL.  When `--test` and `--deploy` are given together the tests
gate the deploy, so a regression is never shipped.

Deploying copies `xinput1_3.dll` (the XInput proxy that sits next to cc.exe)
and `translations.tsv` into the game folder; set VN_DIST_BUILD to skip that,
because a distribution build stages the artifacts itself and may run where no
game is installed.  Nothing else ships: 03_find_narrative_cg re-renders
narrative CGs into script_output/narrative_patched/, but CPK repacking is not
implemented, so those BMPs are inspection-only and never reach the game.

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
GAME_DIR = r"C:\Games\CROSS_CHANNEL"
CONFIG = "Release"
PRESET = "windows-release"

# Japanese paths and console output survive the trip only in UTF-8, which is
# what the batch wrappers get from `chcp 65001`.
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except (AttributeError, ValueError):
    pass

# The halves, for the steps CMake does not own.  ctest is per sub-build: each
# half has its own binary dir and its own test preset, and a single ctest run
# cannot span the two.  Order matters — the pipeline generates
# translations.tsv, which the deploy step ships.
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
    """True when the half declares this test preset.

    Not every half in this repo has one, and ctest exits non-zero on a preset
    it cannot find — which would read as a test failure rather than as a half
    that simply has no tests.
    """
    listed = subprocess.run(["ctest", "--list-presets"], cwd=source_dir,
                            capture_output=True, text=True)
    return listed.returncode == 0 and f'"{preset}"' in listed.stdout


def test():
    for label, source_dir, preset in HALVES:
        if not has_test_preset(source_dir, preset):
            print(f"\n=== {label}: no test preset — skipping ===")
            continue
        print(f"\n=== {label}: tests ===")
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
    dll = os.path.join(BUILD_DIR, "proxy", CONFIG, "xinput1_3.dll")
    try:
        shutil.copyfile(dll, os.path.join(GAME_DIR, "xinput1_3.dll"))
    except OSError as exc:
        print(f"COPY xinput1_3.dll FAILED ({exc}) — is the game running? Close it and retry.")
        sys.exit(1)

    # The table is a soft failure: it is generated, so a missing one just
    # means the translation pipeline has not been run yet.
    tsv = os.path.join(ROOT, "translations.tsv")
    if os.path.exists(tsv):
        try:
            shutil.copyfile(tsv, os.path.join(GAME_DIR, "translations.tsv"))
        except OSError as exc:
            print(f"COPY translations.tsv FAILED ({exc})")
            sys.exit(1)
    else:
        print("WARN: translations.tsv not found — run 02_translate.exe first.")

    print(f"\nDeployed to {GAME_DIR}.")


def main():
    parser = argparse.ArgumentParser(
        description="Build the CROSS-CHANNEL pipeline (x64) and proxy DLL (Win32).")
    parser.add_argument("--test", action="store_true",
                        help="run all the tests after building")
    parser.add_argument("--install", action="store_true",
                        help="stage the user-facing artifacts into build/install")
    parser.add_argument("--deploy", action="store_true",
                        help="copy xinput1_3.dll + translations.tsv to the game folder")
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
