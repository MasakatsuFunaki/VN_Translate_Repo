"""Adieu build driver — Conan 2 + CMake + MSVC.

    python build.py                    # build everything, tests included
    python build.py --test             # build, then run all the tests
    python build.py --install          # build, then stage what the user runs
    python build.py --deploy --game-dir DIR    # build, then ship to the game
    python build.py --test --deploy --game-dir DIR    # ship only if tests pass
    python build.py --clean            # delete the build tree

Release only.  The game is a 2016 32-bit SiglusEngine binary, so the two halves
are built for different architectures and that is the trap this script exists
to hide: `pipeline_cpp` is x64 (the tools that read Scene.pck and call the
API), `proxy_dll` is Win32 because its winmm.dll is loaded into the game
process.  One CMake project cannot hold both, so the root CMakeLists.txt drives
each half as a separate sub-build with its own Conan profile and toolchain.

One build folder for the whole game:

    build/pipeline/   pipeline_cpp, x64
    build/proxy/      proxy_dll, Win32
    build/install/    what --install stages: bin/ to run, game/ to copy

A bare run builds every binary including the test executables; `--test` also
runs them, both halves.  When `--test` and `--deploy` are given together the
tests gate the deploy, so a regression is never shipped.

Deploying copies `winmm.dll`, the staged `translation_table.tsv` and the
committed `menu_patch.bin` next to SiglusEngine.exe (`StartData\\GameData\\`).
`winmm.dll` applies `menu_patch.bin` on first launch after backing up original
`.g00` and `Gameexe.dat` files to immutable `.bak` copies.  The destination
comes from `--game-dir`, which has no default because the install path differs
per machine.  Set VN_DIST_BUILD to skip the deploy entirely, because a
distribution build stages the artifacts itself and may run where no game is
installed.

This script builds; it never runs paid translation executables.  Translating
calls a paid API, so that stays an explicit, separate action.
"""

import os
import sys
import shutil
import hashlib
import argparse
import subprocess

ROOT = os.path.dirname(os.path.realpath(__file__))
REPO_ROOT = os.path.dirname(ROOT)
BUILD_DIR = os.path.join(ROOT, "build")
INSTALL_DIR = os.path.join(BUILD_DIR, "install")
CONFIG = "Release"
PRESET = "windows-release"

# Keyless XOR-delta of every localized menu sprite and Gameexe.dat against the
# retail install.  winmm.dll applies it atomically on first launch.
MENU_PATCH = os.path.join(ROOT, "menu_patch.bin")

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

# The halves, for the steps CMake does not own.  ctest is per sub-build: each
# half has its own binary dir and its own test preset, and a single ctest run
# cannot span the two.  Order matters — the pipeline generates
# translation_table.tsv, which the runtime DLL reads.
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


SYNC_JSON_NAMES = (
    "translated_text.json",
    "translation_cache_anthropic.json",
    "translation_cache_gemini.json",
)


def files_identical(path_a, path_b):
    if not os.path.isfile(path_a) or not os.path.isfile(path_b):
        return False
    if os.path.getsize(path_a) != os.path.getsize(path_b):
        return False
    digest_a = hashlib.sha256()
    digest_b = hashlib.sha256()
    with open(path_a, "rb") as fa, open(path_b, "rb") as fb:
        for chunk in iter(lambda: fa.read(65536), b""):
            digest_a.update(chunk)
        for chunk in iter(lambda: fb.read(65536), b""):
            digest_b.update(chunk)
    return digest_a.digest() == digest_b.digest()


def sync_translations():
    """Bidirectionally sync script_output/*.json <-> *.json.enc."""
    out_dir = os.path.join(ROOT, "script_output")
    if not os.path.isdir(out_dir):
        return
    key_file = os.path.join(REPO_ROOT, "translations_key.txt")
    if not os.environ.get("TRANSLATIONS_KEY") and not os.path.isfile(key_file):
        return

    enc_bat = os.path.join(REPO_ROOT, "encrypt_translations.bat")
    if not os.path.isfile(enc_bat):
        enc_bat = os.path.join(ROOT, "encrypt_translations.bat")
    dec_bat = os.path.join(REPO_ROOT, "decrypt_translations.bat")
    if not os.path.isfile(dec_bat):
        dec_bat = os.path.join(ROOT, "decrypt_translations.bat")

    need_decrypt = False
    need_encrypt = False
    has_any_enc = any(
        os.path.isfile(os.path.join(out_dir, n + ".enc")) for n in SYNC_JSON_NAMES
    )
    for name in SYNC_JSON_NAMES:
        plain = os.path.join(out_dir, name)
        enc = plain + ".enc"
        has_p = os.path.isfile(plain)
        has_e = os.path.isfile(enc)
        if has_e and not has_p:
            need_decrypt = True
        elif has_p and not has_e:
            if has_any_enc:
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
    enc = doc + ".enc"
    if not os.path.isfile(doc) and os.path.isfile(enc):
        sync_translations()
    if not os.path.isfile(doc):
        return
    exe = os.path.join(BUILD_DIR, "pipeline", CONFIG, "02_build_tsv.exe")
    if not os.path.isfile(exe):
        return
    tsv = os.path.join(INSTALL_DIR, "game", "translation_table.tsv")
    os.makedirs(os.path.dirname(tsv), exist_ok=True)
    installed_exe = os.path.join(INSTALL_DIR, "bin", os.path.basename(exe))
    newest_src = max(
        os.path.getmtime(p)
        for p in (doc, enc, exe, installed_exe)
        if os.path.isfile(p)
    )
    if not (os.path.isfile(tsv) and os.path.getsize(tsv) > 0
            and os.path.getmtime(tsv) >= newest_src):
        run([exe, "--rebuild-table", "--dir", ROOT, "--out", tsv], ROOT)

    root_tsv = os.path.join(ROOT, "translation_table.tsv")
    if (not files_identical(tsv, root_tsv)
            or os.path.getmtime(root_tsv) < os.path.getmtime(tsv)):
        shutil.copy2(tsv, root_tsv)


def build():
    # The root project configures and builds both halves: it runs Conan for
    # each, configures each through its own preset, and builds each in its own
    # binary dir under build/.
    run(["cmake", "--preset", PRESET], ROOT)
    run(["cmake", "--build", "--preset", PRESET, "--config", CONFIG], ROOT)
    build_table()


def has_test_preset(source_dir, preset):
    """Whether the half offers `preset` as a TEST preset.

    Not every half has one, and `ctest --preset` on a half that does not is a
    hard error about a missing preset rather than a run with no tests.  Asking
    ctest itself keeps the answer true even when the presets change.
    """
    out = subprocess.run(["ctest", "--list-presets"], cwd=source_dir,
                         capture_output=True, text=True)
    return out.returncode == 0 and f'"{preset}"' in out.stdout


def test():
    for label, source_dir, preset in HALVES:
        print(f"\n=== {label}: tests ===")
        if not has_test_preset(source_dir, preset):
            print(f"{label}: no test preset — skipping.")
            continue
        rc = run(["ctest", "--preset", preset, "-C", CONFIG, "--output-on-failure"],
                 source_dir, check=False)
        if rc != 0:
            print(f"\n{label}: TESTS FAILED (rc={rc}) — stopping.")
            sys.exit(rc)


def stage_menu_patch():
    """Copy the committed menu_patch.bin beside winmm.dll in install/game."""
    if not os.path.isfile(MENU_PATCH) or os.path.getsize(MENU_PATCH) == 0:
        sys.exit(
            f"missing or empty {MENU_PATCH} -- run "
            f"04_patch_menus.exe --game-dir <install> first"
        )
    dst = os.path.join(INSTALL_DIR, "game", os.path.basename(MENU_PATCH))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(MENU_PATCH, dst)


def install():
    print(f"\nInstalling to {INSTALL_DIR}...")
    run(["cmake", "--install", BUILD_DIR, "--config", CONFIG], ROOT)
    build_table()
    stage_menu_patch()

    for folder in ("bin", "game"):
        path = os.path.join(INSTALL_DIR, folder)
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                print(f"  {folder}/{name}")


def deploy(game_dir):
    if os.environ.get("VN_DIST_BUILD"):
        print("[DIST] VN_DIST_BUILD set — skipping deploy to the game folder.")
        return

    # A missing game folder is a hard failure: --deploy that ships nothing and
    # still reports success is worse than a clear error.  VN_DIST_BUILD above
    # is the supported way to build where no game is installed.
    if not os.path.isdir(game_dir):
        print(f"\n[deploy] game folder not found: {game_dir}")
        print(f"Built DLL: {os.path.join(BUILD_DIR, 'proxy', CONFIG, 'winmm.dll')}")
        print("Correct --game-dir, or set VN_DIST_BUILD to build without deploying.")
        sys.exit(1)

    print(f"\nDeploying to {game_dir}...")

    # Stage fresh artifacts into build/install/game first so --deploy works
    # identically whether or not --install was passed on the command line.
    run(["cmake", "--install", BUILD_DIR, "--config", CONFIG], ROOT)
    build_table()
    stage_menu_patch()

    staged_game_dir = os.path.join(INSTALL_DIR, "game")

    dll = os.path.join(staged_game_dir, "winmm.dll")
    try:
        shutil.copy2(dll, os.path.join(game_dir, "winmm.dll"))
    except OSError as exc:
        print(f"COPY winmm.dll FAILED ({exc}) — is the game running? Close it and retry.")
        sys.exit(1)

    tsv = os.path.join(staged_game_dir, "translation_table.tsv")
    if not os.path.exists(tsv):
        tsv = os.path.join(ROOT, "translation_table.tsv")
    if os.path.exists(tsv):
        try:
            shutil.copy2(tsv, os.path.join(game_dir, "translation_table.tsv"))
        except OSError as exc:
            print(f"COPY translation_table.tsv FAILED ({exc})")
            sys.exit(1)
    elif not os.path.exists(os.path.join(game_dir, "translation_table.tsv")):
        print("WARN: translation_table.tsv not found — run 02_translate.exe or 02_build_tsv.exe first.")

    src_patch = os.path.join(staged_game_dir, "menu_patch.bin")
    dst_patch = os.path.join(game_dir, "menu_patch.bin")
    try:
        shutil.copy2(src_patch, dst_patch)
    except OSError as exc:
        print(f"COPY menu_patch.bin FAILED ({exc})")
        sys.exit(1)

    print(f"\nDeployed to {game_dir}.")


def main():
    parser = argparse.ArgumentParser(
        description="Build the Adieu pipeline (x64) and hook DLL (Win32).")
    parser.add_argument("--test", action="store_true",
                        help="run all the tests after building")
    parser.add_argument("--install", action="store_true",
                        help="stage the user-facing artifacts into build/install")
    parser.add_argument("--deploy", action="store_true",
                        help="copy winmm.dll, translation_table.tsv and "
                             "menu_patch.bin to the game folder")
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

    sync_translations()
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
