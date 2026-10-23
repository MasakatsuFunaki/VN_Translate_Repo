"""Sakura no Toki build driver — Conan 2 + CMake + MSVC.

    python build.py                    # build everything, tests included
    python build.py --test             # build, then run all the tests
    python build.py --install          # build, then stage what the user runs
    python build.py --deploy --game-dir DIR    # build, then ship to the game
    python build.py --test --deploy --game-dir DIR    # ship only if tests pass
    python build.py --clean            # delete the build tree

Release only.  sakuranotoki.exe is a 32-bit Artemis binary behind a CrackProof
launcher, so the two halves are built for different architectures and that is
the trap this script exists to hide: `pipeline_cpp` is x64 (the tools that read
the PF8 archives and call the API), `proxy_dll` is Win32 because it is mapped
into the game process.  One CMake project cannot hold both, so the root
CMakeLists.txt drives each half as a separate sub-build with its own Conan
profile and toolchain.

One build folder for the whole game:

    build/pipeline/   pipeline_cpp, x64
    build/proxy/      proxy_dll, Win32
    build/install/    what --install stages: bin/ to run, game/ to copy

A bare run builds every binary including the test executables; `--test` also
runs them — the x64 GoogleTest suites of the pipeline and the SYSTEM tier of
the DLL.  When `--test` and `--deploy` are given together the tests gate the
deploy, so a regression is never shipped.

The hook is a `version.dll` proxy: sakuranotoki.exe imports version.dll, which
is not a KnownDLL, so a copy in the game folder wins the search order and is
mapped before the CrackProof launcher unpacks the Artemis engine.  It forwards
every export on to the system DLL.

Deploying copies `version.dll` and `translations.tsv` into the game folder.  The
destination comes from `--game-dir`, which has no default because the install
path differs per machine.  Set VN_DIST_BUILD to skip the deploy entirely,
because a distribution build stages the artifacts itself and may run where no
game is installed.

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
    """Whether this half declares that test preset.

    Not every half of every game has one, and a half without tests must be
    skipped rather than turned into a build failure.
    """
    rc = subprocess.run(["ctest", "--preset", preset, "-N"], cwd=source_dir,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return rc.returncode == 0


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


def deploy(game_dir):
    if os.environ.get("VN_DIST_BUILD"):
        print("[DIST] VN_DIST_BUILD set — skipping deploy to the game folder.")
        return

    # A missing game folder is a hard failure: without this check the copy below
    # fails on the destination and blames the game for holding the file open,
    # which sends the reader after the wrong cause.  VN_DIST_BUILD above is the
    # supported way to build where no game is installed.
    if not os.path.isdir(game_dir):
        print(f"\n[deploy] game folder not found: {game_dir}")
        print(f"Built DLL: {os.path.join(BUILD_DIR, 'proxy', CONFIG, 'version.dll')}")
        print("Correct --game-dir, or set VN_DIST_BUILD to build without deploying.")
        sys.exit(1)

    print(f"\nDeploying to {game_dir}...")

    # From the build tree, not the install tree: --deploy has to work whether
    # or not --install was asked for, and the build tree is always the fresher
    # of the two.
    # The DLL is a hard failure: the usual cause is the game holding it open.
    dll = os.path.join(BUILD_DIR, "proxy", CONFIG, "version.dll")
    try:
        shutil.copyfile(dll, os.path.join(game_dir, "version.dll"))
    except OSError as exc:
        print(f"COPY version.dll FAILED ({exc}) — is the game running? Close it and retry.")
        sys.exit(1)

    # The table is a soft failure: it is generated, so a missing one just
    # means the translation pipeline has not been run yet.
    tsv = os.path.join(ROOT, "translations.tsv")
    if os.path.exists(tsv):
        try:
            shutil.copyfile(tsv, os.path.join(game_dir, "translations.tsv"))
        except OSError as exc:
            print(f"COPY translations.tsv FAILED ({exc})")
            sys.exit(1)
    else:
        print("WARN: translations.tsv not found — run 02_translate.exe first.")

    print(f"\nDeployed to {game_dir}.")


def main():
    parser = argparse.ArgumentParser(
        description="Build the Sakura no Toki pipeline (x64) and proxy DLL (Win32).")
    parser.add_argument("--test", action="store_true",
                        help="run all the tests after building")
    parser.add_argument("--install", action="store_true",
                        help="stage the user-facing artifacts into build/install")
    parser.add_argument("--deploy", action="store_true",
                        help="copy version.dll + translations.tsv to the game folder")
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
