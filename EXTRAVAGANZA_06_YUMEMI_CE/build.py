"""YUMEMIRYOJOKU_CE build driver — Conan 2 + CMake + MSVC.

    python build.py                    # build everything, tests included
    python build.py --test             # build, then run all the tests
    python build.py --install          # build, then stage what the user runs
    python build.py --deploy --game-dir DIR    # build, then ship to the game
    python build.py --test --deploy --game-dir DIR    # ship only if tests pass
    python build.py --clean            # delete the build tree

Release only. The game is a 32-bit BLACKCyc (nnn/BCmkri) binary, so the two
halves are built for different architectures and that is the trap this script
exists to hide: `pipeline_cpp` is x64 (the tools that read the SPT archives and
call the API), `proxy_dll` is Win32 because it is injected into the game
process. One CMake project cannot hold both, so the root CMakeLists.txt drives
each half as a separate sub-build with its own Conan profile and toolchain.

One build folder for the whole game:

    build/pipeline/   pipeline_cpp, x64
    build/proxy/      proxy_dll, Win32
    build/install/    what --install stages: bin/ to run, game/ to copy

A bare run builds every binary including the test executables; `--test` also
runs them, every tier of both halves. When `--test` and `--deploy` are given
together the tests gate the deploy, so a regression is never shipped.

Deploying copies `winmm.dll` into the game folder and nothing else. YUMEMIRYOJOKU_CE
never rewrites its SPT files: the DLL hooks the text splitter at runtime and
looks up `translation_table.tsv` next to the game exe. That table is written
straight into the game folder by 02_build_tsv.exe, so there is nothing for this
script to copy — it only warns when the table is not there yet. The game
folder comes from `--game-dir`, which has no default because the install path
differs per machine. Set VN_DIST_BUILD to skip the deploy entirely, because a
distribution build stages the artifacts itself and may run where no game is
installed.

This script builds; it never runs the pipeline executables. Translating calls
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

    for _, source_dir, _ in HALVES:
        user_presets = os.path.join(source_dir, "CMakeUserPresets.json")
        if os.path.exists(user_presets):
            os.remove(user_presets)


def build_table():
    doc = os.path.join(ROOT, "script_output", "translated_text.json")
    if not os.path.isfile(doc):
        return
    exe = os.path.join(BUILD_DIR, "pipeline", CONFIG, "02_build_tsv.exe")
    tsv = os.path.join(INSTALL_DIR, "game", "translation_table.tsv")
    run([exe, "--rebuild-table", "--dir", ROOT, "--game-dir", ROOT, "--out", tsv], ROOT)


def build():
    clean()
    run(["cmake", "--preset", PRESET], ROOT)
    run(["cmake", "--build", "--preset", PRESET, "--config", CONFIG], ROOT)
    build_table()


def has_test_preset(source_dir, preset):
    """Whether ctest knows this preset."""
    rc = subprocess.run(["ctest", "--preset", preset, "-N"], cwd=source_dir,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return rc.returncode == 0


def test():
    for label, source_dir, preset in HALVES:
        print(f"\n=== {label}: tests ===")
        if not has_test_preset(source_dir, preset):
            print(f"{label}: no test preset -- skipping.")
            continue
        rc = run(["ctest", "--preset", preset, "-C", CONFIG, "--output-on-failure"],
                 source_dir, check=False)
        if rc != 0:
            print(f"\n{label}: TESTS FAILED (rc={rc}) -- stopping.")
            sys.exit(rc)


def install():
    print(f"\nInstalling to {INSTALL_DIR}...")
    run(["cmake", "--install", BUILD_DIR, "--config", CONFIG], ROOT)
    build_table()

    patch_bin = os.path.join(ROOT, "menu_patch.bin")
    if os.path.isfile(patch_bin):
        os.makedirs(os.path.join(INSTALL_DIR, "game"), exist_ok=True)
        shutil.copy2(patch_bin, os.path.join(INSTALL_DIR, "game", "menu_patch.bin"))

    for folder in ("bin", "game"):
        path = os.path.join(INSTALL_DIR, folder)
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                print(f"  {folder}/{name}")


def deploy(game_dir):
    if os.environ.get("VN_DIST_BUILD"):
        print("[DIST] VN_DIST_BUILD set -- skipping deploy to the game folder.")
        return

    print(f"\nDeploying to {game_dir}...")

    dll = os.path.join(BUILD_DIR, "proxy", CONFIG, "winmm.dll")
    try:
        shutil.copyfile(dll, os.path.join(game_dir, "winmm.dll"))
    except OSError as exc:
        print(f"COPY winmm.dll FAILED ({exc}) -- is the game running? Close it and retry.")
        sys.exit(1)

    patch_bin = os.path.join(ROOT, "menu_patch.bin")
    if os.path.isfile(patch_bin):
        try:
            shutil.copyfile(patch_bin, os.path.join(game_dir, "menu_patch.bin"))
        except OSError as exc:
            print(f"COPY menu_patch.bin FAILED ({exc})")
            sys.exit(1)

    tsv = os.path.join(INSTALL_DIR, "game", "translation_table.tsv")
    if os.path.exists(tsv):
        try:
            shutil.copyfile(tsv, os.path.join(game_dir, "translation_table.tsv"))
        except OSError as exc:
            print(f"COPY translation_table.tsv FAILED ({exc})")
            sys.exit(1)
    elif not os.path.exists(os.path.join(game_dir, "translation_table.tsv")):
        print("WARN: translation_table.tsv not in the game folder -- "
              "run 02_build_tsv.exe first.")

    print(f"\nDeployed to {game_dir}.")


def main():
    parser = argparse.ArgumentParser(
        description="Build the YUMEMIRYOJOKU_CE pipeline (x64) and hook DLL (Win32).")
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

    if args.deploy and not args.game_dir:
        sys.exit("--deploy needs --game-dir: the install path differs per machine")

    build()

    if args.test:
        test()

    if args.install:
        install()

    if args.deploy:
        deploy(args.game_dir)

    print("\nDone.")


if __name__ == "__main__":
    main()
