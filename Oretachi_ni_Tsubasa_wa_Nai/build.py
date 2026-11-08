"""Oretachi ni Tsubasa wa Nai Anniversary Edition (Oretachi_ni_Tsubasa_wa_Nai) build driver — Conan 2 + CMake + MSVC.

    python build.py                    # build everything, tests included
    python build.py --test             # build, then run all the tests
    python build.py --install          # build, then stage what the user runs
    python build.py --deploy --game-dir DIR    # build, then ship to the game
    python build.py --test --deploy --game-dir DIR    # ship only if tests pass
    python build.py --clean            # delete the build tree

Release only.  ORE_TUBA.exe is a 32-bit PE, so the two halves are built for
different architectures and that is the trap this script exists to hide:
`pipeline_cpp` is x64 (the tools that read the LPK1/SOB archives and call the
API), `proxy_dll` is Win32 because the DLL is loaded into the 32-bit game
process.  One CMake project cannot hold both, so the root CMakeLists.txt drives
each half as a separate sub-build with its own Conan profile and toolchain.

One build folder for the whole game:

    build/pipeline/   pipeline_cpp, x64
    build/proxy/      proxy_dll, Win32
    build/install/    what --install stages: bin/ to run, game/ to copy

A bare run builds every binary including the test executables; `--test` also
runs them — the pipeline's UT suites and all three tiers (UT/CT/SYSTEM) of the
DLL.  When `--test` and `--deploy` are given together the tests gate the
deploy, so a regression is never shipped.

Deploying copies `winmm.dll` and `translation_table.tsv` into the game folder
named by `--game-dir`, which has no default because the install path differs
per machine.  Set VN_DIST_BUILD to skip the deploy, because a distribution
build stages the artifacts itself and may run where no game is installed.

This script builds; it never runs the pipeline executables.  Translating calls
a paid API, so that stays an explicit, separate action.
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
PRESET = "windows-release"

HALVES = [
    ("pipeline_cpp (x64)", os.path.join(ROOT, "pipeline_cpp"),
     "conan-windows-x64-release"),
    ("proxy_dll (Win32)", os.path.join(ROOT, "proxy_dll"),
     "conan-windows-x86-release"),
]


def active_halves():
    return [
        h for h in HALVES
        if os.path.exists(os.path.join(h[1], "CMakeLists.txt"))
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


SOFTDENCHI_OUTER_SIZE = 3004760
DMM_OUTER_WRAPPER_SIZE = 1381376
INNER_PE_OFFSET = 0x031BC8
INNER_PE_SIZE = 0x10D000


def unwrap_inner_pe_if_needed(game_dir):
    if not game_dir or not os.path.isdir(game_dir):
        return False
    exe_path = os.path.join(game_dir, "ORE_TUBA.exe")
    if not os.path.isfile(exe_path):
        return False
    file_size = os.path.getsize(exe_path)
    if file_size == INNER_PE_SIZE:
        return False
    if file_size not in (SOFTDENCHI_OUTER_SIZE, DMM_OUTER_WRAPPER_SIZE):
        return False
    if file_size < INNER_PE_OFFSET + INNER_PE_SIZE:
        return False
    with open(exe_path, "rb") as f:
        outer_bytes = f.read()
    inner_slice = outer_bytes[INNER_PE_OFFSET:INNER_PE_OFFSET + INNER_PE_SIZE]
    if len(inner_slice) != INNER_PE_SIZE or inner_slice[:2] != b"MZ":
        return False
    backup_path = os.path.join(game_dir, "ORE_TUBA_orig.exe")
    if not os.path.isfile(backup_path):
        with open(backup_path, "wb") as f:
            f.write(outer_bytes)
        print(f"Backed up outer wrapper to {backup_path}")
    with open(exe_path, "wb") as f:
        f.write(inner_slice)
    print(f"Unwrapped inner Lucifen PE ({INNER_PE_SIZE:,} bytes) to {exe_path}")
    return True


def build_table(game_dir=None):
    translated_json = os.path.join(ROOT, "script_output", "translated_text.json")
    if not os.path.isfile(translated_json):
        return
    build_tsv_exe = os.path.join(BUILD_DIR, "pipeline", CONFIG, "03_build_tsv.exe")
    if not os.path.isfile(build_tsv_exe):
        return
    out_tsv = os.path.join(INSTALL_DIR, "game", "translation_table.tsv")
    os.makedirs(os.path.dirname(out_tsv), exist_ok=True)
    cmd = [build_tsv_exe, "--rebuild-table", "--out", out_tsv]
    if game_dir:
        cmd.extend(["--game-dir", str(game_dir)])
    run(cmd, ROOT)


def build(game_dir=None):
    clean()
    run(["cmake", "--preset", PRESET], ROOT)
    run(["cmake", "--build", "--preset", PRESET, "--config", CONFIG], ROOT)
    build_table(game_dir)


def has_test_preset(source_dir, preset):
    listed = subprocess.run(
        ["ctest", "--list-presets"],
        cwd=source_dir,
        capture_output=True,
        text=True,
    )
    return listed.returncode == 0 and f'"{preset}"' in listed.stdout


def test():
    for label, source_dir, preset in active_halves():
        if not has_test_preset(source_dir, preset):
            print(f"\n=== {label}: no test preset -- skipping ===")
            continue
        print(f"\n=== {label}: tests ===")
        rc = run(
            ["ctest", "--preset", preset, "-C", CONFIG, "--output-on-failure"],
            source_dir,
            check=False,
        )
        if rc != 0:
            print(f"\n{label}: TESTS FAILED (rc={rc}) -- stopping.")
            sys.exit(rc)


def install(game_dir=None):
    print(f"\nInstalling to {INSTALL_DIR}...")
    run(["cmake", "--install", BUILD_DIR, "--config", CONFIG], ROOT)
    build_table(game_dir)

    for folder in ("bin", "game"):
        path = os.path.join(INSTALL_DIR, folder)
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                print(f"  {folder}/{name}")


def deploy(game_dir):
    if os.environ.get("VN_DIST_BUILD"):
        print("[DIST] VN_DIST_BUILD set — skipping deploy to the game folder.")
        return

    dll = os.path.join(BUILD_DIR, "proxy", CONFIG, "winmm.dll")

    if not os.path.isdir(game_dir):
        print(f"\n[deploy] game folder not found: {game_dir}")
        print(f"Built DLL: {dll}")
        print("Correct --game-dir, or set VN_DIST_BUILD to build without deploying.")
        sys.exit(1)

    build_table(game_dir)
    unwrap_inner_pe_if_needed(game_dir)

    print(f"\nDeploying to {game_dir}...")

    if os.path.isfile(dll):
        try:
            shutil.copyfile(dll, os.path.join(game_dir, "winmm.dll"))
        except OSError as exc:
            print(f"COPY winmm.dll FAILED ({exc}) — is the game running? Close it and retry.")
            sys.exit(1)

    staged_tsv = os.path.join(INSTALL_DIR, "game", "translation_table.tsv")
    game_tsv = os.path.join(game_dir, "translation_table.tsv")
    if os.path.isfile(staged_tsv):
        try:
            shutil.copyfile(staged_tsv, game_tsv)
        except OSError as exc:
            print(f"COPY translation_table.tsv FAILED ({exc})")
            sys.exit(1)
    elif not os.path.exists(game_tsv):
        print("WARN: translation_table.tsv not in build/install/game or game folder — "
              "run 02_translate.exe / 03_build_tsv.exe first.")

    print(f"\nDeployed to {game_dir}.")


def main():
    parser = argparse.ArgumentParser(
        description="Build the Oretachi_ni_Tsubasa_wa_Nai pipeline (x64) and hook DLL (Win32).")
    parser.add_argument("--test", action="store_true",
                        help="run all the tests after building")
    parser.add_argument("--install", action="store_true",
                        help="stage the user-facing artifacts into build/install")
    parser.add_argument("--deploy", action="store_true",
                        help="copy winmm.dll + translation_table.tsv to the game folder")
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

    build(args.game_dir)

    if args.test:
        test()

    if args.install:
        install(args.game_dir)

    if args.deploy:
        deploy(args.game_dir)

    print("\nDone.")


if __name__ == "__main__":
    main()
