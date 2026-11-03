"""FAVORITE (夜、灯す。 / Yoru, Tomosu) build driver — Conan 2 + CMake + MSVC.

    python build.py                    # build everything, tests included
    python build.py --test             # build, then run all the tests
    python build.py --install          # build, then stage what the user runs
    python build.py --deploy --game-dir DIR    # build, then ship to the game
    python build.py --test --deploy --game-dir DIR    # ship only if tests pass
    python build.py --clean            # delete the build tree

Release only.  Sakura.exe is a 32-bit PE, so the two halves are built for
different architectures and that is the trap this script exists to hide:
`pipeline_cpp` is x64 (the tools that walk Sakura.hcb and call the API),
`proxy_dll` is Win32 because the DLL is loaded into the game process.  One
CMake project cannot hold both, so the root CMakeLists.txt drives each half as
a separate sub-build with its own Conan profile and toolchain.

One build folder for the whole game:

    build/pipeline/   pipeline_cpp, x64
    build/proxy/      proxy_dll, Win32
    build/install/    what --install stages: bin/ to run, game/ to copy

A bare run builds every binary including the test executables; `--test` also
runs them — the pipeline's UT suites and all three tiers (UT/CT/SYSTEM) of the
DLL.  When `--test` and `--deploy` are given together the tests gate the
deploy, so a regression is never shipped.

Deploying copies `winmm.dll` into the game folder named by `--game-dir`, which
has no default because the install path differs per machine.  That is the only
file copied: unlike the other games in this repo, the translation step writes
`translation_table.tsv` straight into the game folder, so the table is never
staged in the project tree and there is nothing to copy for it.  The deploy
still checks that the table is there and warns if it is not.  Set VN_DIST_BUILD
to skip the deploy, because a distribution build stages the artifacts itself
and may run where no game is installed.

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
CONFIG = "Release"
PRESET = "windows-release"

# The halves, for the steps CMake does not own.  ctest is per sub-build: each
# half has its own binary dir and its own test preset, and a single ctest run
# cannot span the two.  Order matters — the pipeline generates
# translation_table.tsv, which the DLL loads at runtime.
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


def build_table():
    doc = os.path.join(ROOT, "script_output", "translated_text.json")
    if not os.path.isfile(doc):
        return
    exe = os.path.join(BUILD_DIR, "pipeline", CONFIG, "02_translate.exe")
    tsv = os.path.join(INSTALL_DIR, "game", "translation_table.tsv")
    run([exe, "--rebuild-table", "--dir", ROOT, "--game-dir", ROOT, "--out", tsv], ROOT)


def build():
    clean()
    # The root project configures and builds both halves: it runs Conan for
    # each, configures each through its own preset, and builds each in its
    # own binary dir under build/.
    run(["cmake", "--preset", PRESET], ROOT)
    run(["cmake", "--build", "--preset", PRESET, "--config", CONFIG], ROOT)
    build_table()


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
    build_table()

    for folder in ("bin", "game"):
        path = os.path.join(INSTALL_DIR, folder)
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                print(f"  {folder}/{name}")


def deploy(game_dir):
    if os.environ.get("VN_DIST_BUILD"):
        print("[DIST] VN_DIST_BUILD set — skipping deploy to the game folder.")
        return

    # From the build tree, not the install tree: --deploy has to work whether
    # or not --install was asked for, and the build tree is always the fresher
    # of the two.
    dll = os.path.join(BUILD_DIR, "proxy", CONFIG, "winmm.dll")

    # A missing folder is a hard failure: without this check the copy below
    # fails on the destination and blames the game for holding the file open,
    # which sends the reader after the wrong cause.
    if not os.path.isdir(game_dir):
        print(f"\n[deploy] game folder not found: {game_dir}")
        print(f"Built DLL: {dll}")
        print("Correct --game-dir, or set VN_DIST_BUILD to build without deploying.")
        sys.exit(1)

    print(f"\nDeploying to {game_dir}...")

    # The DLL is a hard failure: the usual cause is the game holding it open.
    try:
        shutil.copyfile(dll, os.path.join(game_dir, "winmm.dll"))
    except OSError as exc:
        print(f"COPY winmm.dll FAILED ({exc}) — is the game running? Close it and retry.")
        sys.exit(1)

    tsv = os.path.join(INSTALL_DIR, "game", "translation_table.tsv")
    if os.path.exists(tsv):
        try:
            shutil.copyfile(tsv, os.path.join(game_dir, "translation_table.tsv"))
        except OSError as exc:
            print(f"COPY translation_table.tsv FAILED ({exc})")
            sys.exit(1)
    elif not os.path.exists(os.path.join(game_dir, "translation_table.tsv")):
        print("WARN: translation_table.tsv not in the game folder — "
              "run 02_translate.exe first.")

    print(f"\nDeployed to {game_dir}.")


def main():
    parser = argparse.ArgumentParser(
        description="Build the FAVORITE pipeline (x64) and hook DLL (Win32).")
    parser.add_argument("--test", action="store_true",
                        help="run all the tests after building")
    parser.add_argument("--install", action="store_true",
                        help="stage the user-facing artifacts into build/install")
    parser.add_argument("--deploy", action="store_true",
                        help="copy winmm.dll to the game folder")
    parser.add_argument("--game-dir",
                        help="game folder to deploy into; required with --deploy")
    parser.add_argument("--clean", action="store_true",
                        help="delete the build tree and stop")
    args = parser.parse_args()

    if args.clean:
        clean()
        return

    # Checked before the build, so a missing --game-dir fails in a second
    # rather than after a full compile.
    if args.deploy and not args.game_dir:
        sys.exit("--deploy needs --game-dir: the install path differs per machine")

    build()

    # Tests run before any staging or deploy, so --test gates both.
    if args.test:
        test()

    if args.install:
        install()

    if args.deploy:
        deploy(args.game_dir)

    print("\nDone.")


if __name__ == "__main__":
    main()
