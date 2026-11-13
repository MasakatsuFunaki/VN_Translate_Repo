"""kishin_hisho build driver — Conan 2 + CMake + MSVC.

    python build.py                    # build everything, tests included
    python build.py --test             # build, then run all the tests
    python build.py --install          # build, then stage what the user runs
    python build.py --deploy --game-dir DIR    # build, then ship to the game
    python build.py --test --deploy --game-dir DIR    # ship only if tests pass
    python build.py --clean            # delete the build tree

Release only. This game is not a translation pipeline: 斬魔大聖デモンベイン
(Nitroplus, 2006) ships with the AlphaROM copy protection, whose runtime API
resolver crashes on Windows 11, and the deliverable is a launcher plus an
injected DLL that repair the stub in flight.

The two halves are built for different architectures and that is the trap this
script exists to hide: `pipeline_cpp` is x64 (the patcher driver `01_patch` and
the script archive rebuilder `02_build_pak`, on Boost.ProgramOptions + capstone +
zlib), `helper_dll` is Win32
because both of its binaries live in the 32-bit game process — `launch.exe`
starts it suspended and `alpharom_helper.dll` is injected into it. Its
CMakeLists refuses outright to configure 64-bit. One CMake project cannot hold
both, so the root CMakeLists.txt drives each half as a separate sub-build with
its own Conan profile and toolchain.

One build folder for the whole game:

    build/pipeline/   pipeline_cpp, x64
    build/helper/     helper_dll, Win32
    build/install/    what --install stages: bin/ to run, game/ to copy

A bare run builds every binary including the test executables; `--test` also
runs them. Only `pipeline_cpp` has a test suite (its CMakePresets.json declares
a test preset); `helper_dll` has none and is skipped rather than failed. When
`--test` and `--deploy` are given together the tests gate the deploy, so a
regression is never shipped.

Deploying copies `alpharom_helper.dll` and `launch.exe` plus rebuilt
`0script.pak` / `script/*.nps` into the game folder named by `--game-dir`, which
has no default because the install path differs per machine. A NitroPlus install
sits under Program Files (x86) and therefore needs an elevated shell.
`01_patch.exe` performs the same deployment for pre-built or staged distribution
artifacts (`<project>/game` or `build/install/game`).

Set VN_DIST_BUILD to skip the deploy, because a distribution build stages the
artifacts itself and may run where no game is installed.

This script builds; it never runs the pipeline executables.
"""

import os
import sys
import shutil
import argparse
import subprocess

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.realpath(__file__))
BUILD_DIR = os.path.join(ROOT, "build")
INSTALL_DIR = os.path.join(BUILD_DIR, "install")
CONFIG = "Release"
PRESET = "windows-release"

# The halves, for the steps CMake does not own.  ctest is per sub-build: each
# half has its own binary dir, and a single ctest run cannot span the two.
# `helper_dll` declares no test preset — it is two hand-written Win32 binaries
# with no suite — so --test steps over it instead of failing on a missing
# preset.
HALVES = [
    ("pipeline_cpp (x64)", os.path.join(ROOT, "pipeline_cpp"),
     "conan-windows-x64-release", True),
    ("helper_dll (Win32)", os.path.join(ROOT, "helper_dll"),
     "conan-windows-x86-release", False),
]

# What the deploy ships, in the order it is copied. Both come out of the Win32
# half's build tree.
ARTIFACTS = ["alpharom_helper.dll", "launch.exe"]


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
    for _, source_dir, _, _ in HALVES:
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


def build():
    clean()
    # The root project configures and builds both halves: it runs Conan for
    # each, configures each through its own preset, and builds each in its
    # own binary dir under build/.
    run(["cmake", "--preset", PRESET], ROOT)
    run(["cmake", "--build", "--preset", PRESET, "--config", CONFIG], ROOT)


def test():
    for label, source_dir, preset, has_tests in HALVES:
        if not has_tests:
            print(f"\n=== {label}: no test suite — skipping ===")
            continue

        print(f"\n=== {label}: tests ===")
        rc = run(["ctest", "--preset", preset, "-C", CONFIG, "--output-on-failure"],
                 source_dir, check=False)
        if rc != 0:
            print(f"\n{label}: TESTS FAILED (rc={rc}) — stopping.")
            sys.exit(rc)


def _stage_or_deploy_pak(target_game_dir):
    script_out = os.path.join(ROOT, "script_output")
    translated_json = os.path.join(script_out, "translated_text.json")
    recon_json = os.path.join(script_out, "nps_reconstruction_map.json")
    build_pak_exe = os.path.join(BUILD_DIR, "pipeline", CONFIG, "02_build_pak.exe")
    for required in (build_pak_exe, translated_json, recon_json):
        if not os.path.isfile(required):
            print(f"MISSING REQUIRED PAK INPUT ({required}) — cannot build 0script.pak.")
            sys.exit(1)
    run([build_pak_exe, "--dir", ROOT, "--game-dir", target_game_dir, "--verify"], ROOT)


def install():
    print(f"\nInstalling to {INSTALL_DIR}...")
    run(["cmake", "--install", BUILD_DIR, "--config", CONFIG], ROOT)
    _stage_or_deploy_pak(os.path.join(INSTALL_DIR, "game"))

    for folder in ("bin", "game"):
        path = os.path.join(INSTALL_DIR, folder)
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                print(f"  {folder}/{name}")


def deploy(game_dir):
    if os.environ.get("VN_DIST_BUILD"):
        print("[DIST] VN_DIST_BUILD set — skipping deploy to the game folder.")
        return

    print(f"\nDeploying to {game_dir}...")

    if not os.path.isdir(game_dir):
        print(f"GAME FOLDER NOT FOUND ({game_dir}) — nothing to deploy to.")
        sys.exit(1)

    # From the build tree, not the install tree: --deploy has to work whether
    # or not --install was asked for, and the build tree is always the fresher
    # of the two.
    #
    # Every copy is a hard failure: both files are build outputs, so a missing
    # one means the build did not run. A denied or busy destination is the
    # usual runtime cause — the game holding the DLL open, or Program Files
    # refusing a non-elevated write.
    source_dir = os.path.join(BUILD_DIR, "helper", CONFIG)
    for name in ARTIFACTS:
        try:
            shutil.copyfile(os.path.join(source_dir, name),
                            os.path.join(game_dir, name))
        except OSError as exc:
            print(f"COPY {name} FAILED ({exc}) — is the game running? Close it "
                  f"and retry. A game folder under Program Files also needs an "
                  f"elevated shell.")
            sys.exit(1)
        print(f"  {name}")

    _stage_or_deploy_pak(game_dir)

    print(f"\nDeployed to {game_dir}. Run launch.exe there, not KishinHisho.exe.")


def main():
    parser = argparse.ArgumentParser(
        description="Build the kishin_hisho tooling (x64) and AlphaROM helper (Win32).")
    parser.add_argument("--test", action="store_true",
                        help="run all the tests after building")
    parser.add_argument("--install", action="store_true",
                        help="stage the user-facing artifacts into build/install")
    parser.add_argument("--deploy", action="store_true",
                        help="copy alpharom_helper.dll + launch.exe to the game folder")
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
