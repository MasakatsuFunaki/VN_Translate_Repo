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


SYNC_JSON_NAMES = (
    "translated_text.json",
    "translation_cache_anthropic.json",
    "translation_cache_gemini.json",
    "nps_reconstruction_map.json",
    "chart_translation_cache.json",
)


def sync_translations():
    """Bidirectionally sync script_output/*.json <-> *.json.enc."""
    out_dir = os.path.join(ROOT, "script_output")
    if not os.path.isdir(out_dir):
        return
    repo_root = os.path.dirname(ROOT)
    enc_bat = os.path.join(repo_root, "encrypt_translations.bat")
    if not os.path.isfile(enc_bat):
        enc_bat = os.path.join(ROOT, "encrypt_translations.bat")
    dec_bat = os.path.join(repo_root, "decrypt_translations.bat")
    if not os.path.isfile(dec_bat):
        dec_bat = os.path.join(ROOT, "decrypt_translations.bat")
    need_decrypt = False
    need_encrypt = False
    for name in SYNC_JSON_NAMES:
        plain = os.path.join(out_dir, name)
        enc = plain + ".enc"
        has_p = os.path.isfile(plain)
        has_e = os.path.isfile(enc)
        if has_e and not has_p:
            need_decrypt = True
        elif has_p and not has_e:
            # Only auto-encrypt if this game already tracks at least one .enc file
            if any(os.path.isfile(os.path.join(out_dir, n + ".enc")) for n in SYNC_JSON_NAMES):
                need_encrypt = True
        elif has_p and has_e:
            p_mtime = os.path.getmtime(plain)
            e_mtime = os.path.getmtime(enc)
            if e_mtime > p_mtime + 2.0:
                need_decrypt = True
            elif p_mtime > e_mtime + 2.0:
                need_encrypt = True
    if need_decrypt and os.path.isfile(dec_bat):
        run(["cmd", "/c", dec_bat, "--dir", ROOT], ROOT)
    if need_encrypt and os.path.isfile(enc_bat):
        run(["cmd", "/c", enc_bat, "--encrypt", "--dir", ROOT], ROOT)


def build_table():
    doc = os.path.join(ROOT, "script_output", "translated_text.json")
    if not os.path.isfile(doc):
        sync_translations()
    if not os.path.isfile(doc):
        return
    exe = os.path.join(BUILD_DIR, "pipeline", CONFIG, "02_build_tsv.exe")
    tsv = os.path.join(INSTALL_DIR, "game", TABLE_NAME)
    os.makedirs(os.path.dirname(tsv), exist_ok=True)
    if (os.path.isfile(tsv) and os.path.getsize(tsv) > 0
            and os.path.getmtime(tsv) >= os.path.getmtime(doc)
            and os.path.getmtime(tsv) >= os.path.getmtime(exe)):
        return
    run([exe, "--rebuild-table", "--dir", ROOT, "--game-dir", ROOT, "--out", tsv], ROOT)


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

    if os.path.isfile(patch_src) and os.path.isfile(cgsys):
        patcher = os.path.join(INSTALL_DIR, "bin", "03_patch_menus.exe")
        if not os.path.isfile(patcher):
            patcher = os.path.join(BUILD_DIR, "pipeline", CONFIG, "03_patch_menus.exe")
        run([patcher, "--game-dir", game_dir], ROOT)

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

    sync_translations()
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
