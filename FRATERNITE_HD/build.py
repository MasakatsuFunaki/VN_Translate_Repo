"""FRATERNITE_HD build driver — Conan 2 + CMake + MSVC.

    python build.py --test --install --deploy --game-dir DIR
    python build.py --clean

Hides the x64/Win32 split: pipeline_cpp is x64, proxy_dll is Win32.
--test gates --deploy.  Set VN_DIST_BUILD to skip the game-folder copy.
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

TABLE_NAME = "translation_table.tsv"

# ctest is per sub-build: a single ctest run cannot span both architectures.
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

    # Stale user-presets make `cmake --preset` fail after a clean.
    for _, source_dir, _ in HALVES:
        user_presets = os.path.join(source_dir, "CMakeUserPresets.json")
        if os.path.exists(user_presets):
            os.remove(user_presets)


def build_table():
    out_dir = os.path.join(ROOT, "script_output")
    doc = os.path.join(out_dir, "translated_text.json")
    cache = os.path.join(out_dir, "translation_cache_anthropic.json")
    enc = doc + ".enc"
    had_plain = os.path.isfile(doc)
    if not had_plain:
        if not os.path.isfile(enc):
            return
        run(["cmd", "/c", os.path.join(ROOT, "decrypt_translations.bat")], ROOT)
    try:
        exe = os.path.join(BUILD_DIR, "pipeline", CONFIG, "02_build_tsv.exe")
        tsv = os.path.join(INSTALL_DIR, "game", TABLE_NAME)
        run([exe, "--rebuild-table", "--dir", ROOT, "--game-dir", ROOT, "--out", tsv], ROOT)
    finally:
        if not had_plain:
            for p in (doc, cache):
                if os.path.isfile(p):
                    os.remove(p)


def build():
    clean()
    run(["cmake", "--preset", PRESET], ROOT)
    run(["cmake", "--build", "--preset", PRESET, "--config", CONFIG], ROOT)
    build_table()


def has_test_preset(source_dir, preset):
    """True when the half exposes `preset` as a TEST preset."""
    out = subprocess.run(["ctest", "--list-presets"], cwd=source_dir,
                         capture_output=True, text=True)
    return f'"{preset}"' in out.stdout


def test():
    for label, source_dir, preset in HALVES:
        print(f"\n=== {label}: tests ===")
        if not has_test_preset(source_dir, preset):
            print(f"{label}: no test preset '{preset}' -- skipping.")
            continue
        rc = run(["ctest", "--preset", preset, "-C", CONFIG, "--output-on-failure"],
                 source_dir, check=False)
        if rc != 0:
            print(f"\n{label}: TESTS FAILED (rc={rc}) -- stopping.")
            sys.exit(rc)


def stage_translations():
    """Stage unencrypted translation JSON artifacts into install/script_output/."""
    src = os.path.join(ROOT, "script_output")
    dst = os.path.join(INSTALL_DIR, "script_output")
    names = ["translated_text.json", "translation_cache_anthropic.json"]
    had_plain = {n: os.path.isfile(os.path.join(src, n)) for n in names}

    if not all(had_plain.values()):
        enc_present = any(os.path.isfile(os.path.join(src, n + ".enc")) for n in names)
        if enc_present:
            run(["cmd", "/c", os.path.join(ROOT, "decrypt_translations.bat")], ROOT)

    try:
        present = [n for n in names if os.path.isfile(os.path.join(src, n))]
        if not present:
            print("  script_output: no translation JSONs to stage")
            return

        os.makedirs(dst, exist_ok=True)
        for name in present:
            shutil.copyfile(os.path.join(src, name), os.path.join(dst, name))
            print(f"  script_output/{name}")
    finally:
        for name, existed in had_plain.items():
            if not existed:
                p = os.path.join(src, name)
                if os.path.isfile(p):
                    os.remove(p)


def install():
    print(f"\nInstalling to {INSTALL_DIR}...")
    run(["cmake", "--install", BUILD_DIR, "--config", CONFIG], ROOT)
    build_table()

    patch_src = os.path.join(ROOT, "cgsys_patch.bin")
    if os.path.isfile(patch_src):
        patch_dst = os.path.join(INSTALL_DIR, "game", "cgsys_patch.bin")
        os.makedirs(os.path.dirname(patch_dst), exist_ok=True)
        shutil.copyfile(patch_src, patch_dst)

    for folder in ("bin", "game"):
        path = os.path.join(INSTALL_DIR, folder)
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                if os.path.isfile(os.path.join(path, name)):
                    print(f"  {folder}/{name}")

    stage_translations()


def deploy(game_dir):
    if os.environ.get("VN_DIST_BUILD"):
        print("[DIST] VN_DIST_BUILD set — skipping deploy to the game folder.")
        return

    print(f"\nDeploying to {game_dir}...")

    # From the build tree (always fresher than install).
    dll = os.path.join(BUILD_DIR, "proxy", CONFIG, "winmm.dll")
    try:
        shutil.copyfile(dll, os.path.join(game_dir, "winmm.dll"))
    except OSError as exc:
        print(f"COPY winmm.dll FAILED ({exc}) — is the game running? Close it and retry.")
        sys.exit(1)

    tsv = os.path.join(INSTALL_DIR, "game", TABLE_NAME)
    if os.path.exists(tsv):
        try:
            shutil.copyfile(tsv, os.path.join(game_dir, TABLE_NAME))
        except OSError as exc:
            print(f"COPY {TABLE_NAME} FAILED ({exc})")
            sys.exit(1)
    elif not os.path.exists(os.path.join(game_dir, TABLE_NAME)):
        print(f"WARN: {TABLE_NAME} not in the game folder — run 02_build_tsv.exe first.")

    patch_src = os.path.join(ROOT, "cgsys_patch.bin")
    if os.path.isfile(patch_src):
        try:
            shutil.copyfile(patch_src, os.path.join(game_dir, "cgsys_patch.bin"))
        except OSError as exc:
            print(f"COPY cgsys_patch.bin FAILED ({exc})")
            sys.exit(1)

    cgsys = os.path.join(game_dir, "pac", "cgsys.ypf")
    cgsys_bak = cgsys + ".bak"
    if os.path.isfile(cgsys) and not os.path.isfile(cgsys_bak):
        shutil.copyfile(cgsys, cgsys_bak)

    menu_assets = os.path.join(ROOT, "menu_assets")
    if (os.path.isdir(menu_assets) or os.path.isfile(patch_src)) and os.path.isfile(cgsys):
        patcher = os.path.join(INSTALL_DIR, "bin", "03_patch_menus.exe")
        if not os.path.isfile(patcher):
            patcher = os.path.join(BUILD_DIR, "pipeline", CONFIG, "03_patch_menus.exe")
        run([patcher, "--dir", ROOT, "--game-dir", game_dir], ROOT)

    print(f"\nDeployed to {game_dir}.")


def main():
    parser = argparse.ArgumentParser(
        description="Build the FRATERNITE_HD pipeline (x64) and hook DLL (Win32).")
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

    # Fail fast before a full compile.
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
