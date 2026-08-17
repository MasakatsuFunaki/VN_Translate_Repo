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


def build():
    run(["cmake", "--preset", PRESET], ROOT)
    run(["cmake", "--build", "--preset", PRESET, "--config", CONFIG], ROOT)


def has_test_preset(source_dir, preset):
    """True when the half exposes `preset` as a TEST preset."""
    out = subprocess.run(["ctest", "--list-presets"], cwd=source_dir,
                         capture_output=True, text=True)
    return f'"{preset}"' in out.stdout


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


def stage_translations():
    """Stage .enc artifacts and the decrypt batch into install/."""
    src = os.path.join(ROOT, "script_output")
    dst = os.path.join(INSTALL_DIR, "script_output")
    names = ["translated_text.json.enc", "translation_cache_anthropic.json.enc"]

    present = [n for n in names if os.path.isfile(os.path.join(src, n))]
    if not present:
        print("  script_output: no .enc to stage — run "
              "encrypt_translations.bat --encrypt first")
        return

    os.makedirs(dst, exist_ok=True)
    for name in present:
        shutil.copyfile(os.path.join(src, name), os.path.join(dst, name))
        print(f"  script_output/{name}")

    for name in ("decrypt_translations.bat", "translations_key.txt"):
        path = os.path.join(ROOT, name)
        if os.path.isfile(path):
            shutil.copyfile(path, os.path.join(INSTALL_DIR, name))
            print(f"  {name}")
        else:
            print(f"  MISSING {name} at {path} — the install cannot be decrypted")


def install():
    print(f"\nInstalling to {INSTALL_DIR}...")
    run(["cmake", "--install", BUILD_DIR, "--config", CONFIG], ROOT)

    for folder in ("bin", "game"):
        path = os.path.join(INSTALL_DIR, folder)
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
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

    # Soft failure: 02_translate writes the table; missing just means it hasn't run.
    if not os.path.exists(os.path.join(game_dir, TABLE_NAME)):
        print(f"WARN: {TABLE_NAME} not in the game folder — run 02_translate.exe first.")

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
