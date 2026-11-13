#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 MasakatsuFunaki
#
# Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
# Not licensed for use as training data for machine learning or generative
# AI systems; text and data mining rights are reserved.  See NOTICE.
"""Build every game this repository ships and stage them under install/.

    python build.py --list                 which games would be built, and why
    python build.py --install              build each game, stage install/<game>/
    python build.py --test --install       run each game's suite first
    python build.py --clean                delete every game's build tree
    python build.py --only shingakkou --install     one game

This is an orchestrator, not a CMake project of its own.  Each game owns its
build -- two architectures, its own presets, its own install target -- behind
`<game>/build.py`, and this script calls that and collects the results into

    install/<game>/bin            the executables you run
    install/<game>/game           drop into the game folder: proxy + table

`bin` and `game` must stay apart because the proxy is 32-bit and carries a
system DLL's name; flattened beside the x64 executables the loader would pick
it up and then refuse it.  Raw `script_output/*.json(.enc)` script dumps are
excluded from `install/<game>/` so release archives ship only the runtime
binaries and tables.

WHICH GAMES: every top-level folder holding a build.py, a CMakeLists.txt, and
a tracked script_output/translated_text.json(.enc) that git does not ignore.
A game without a tracked translation is still in progress and is skipped
until its translation lands, so the set that builds equals the set that
releases, with nothing to keep in sync by hand.
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.realpath(__file__))
INSTALL_DIR = os.path.join(ROOT, "install")

# The runtime table, staged into <game>/build/install/game/ by <game>/build.py.
# It sits beside the proxy in game/, because that pair is the drop-in patch:
# copy game/ into the game folder and it speaks English.
RUNTIME_TABLES = ("translations.tsv", "translation_table.tsv")

# Archive-repack games that patch .spt/.xtx/.fxf/.pak directly and do not use a runtime TSV.
REPACK_ONLY_GAMES = ("EXTRAVAGANZA_01_CE", "kishin_hisho")


def run(cmd, cwd, check=True):
    print(f"\n> {subprocess.list2cmdline(cmd)}   [{os.path.relpath(cwd, ROOT)}]")
    rc = subprocess.run(cmd, cwd=cwd).returncode
    if check and rc != 0:
        sys.exit(rc)
    return rc


def git_ignores(path):
    """True when git ignores `path`.  Unknown (no git, no repo) means shipped."""
    try:
        return subprocess.run(["git", "check-ignore", "-q", path], cwd=ROOT,
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
    except FileNotFoundError:
        return False


def has_translation(game):
    """True when `game` carries a non-ignored translated_text.json(.enc)."""
    for name in ("translated_text.json", "translated_text.json.enc"):
        rel = os.path.join(game, "script_output", name)
        if os.path.isfile(os.path.join(ROOT, rel)) and not git_ignores(rel):
            return True
    return False


def discover():
    """(shipped, skipped) game folder names, each sorted."""
    shipped, skipped = [], []
    for name in sorted(os.listdir(ROOT)):
        d = os.path.join(ROOT, name)
        if not os.path.isdir(d) or name.startswith("."):
            continue
        if not (os.path.isfile(os.path.join(d, "build.py"))
                and os.path.isfile(os.path.join(d, "CMakeLists.txt"))):
            continue
        if git_ignores(name) or not has_translation(name):
            skipped.append(name)
        else:
            shipped.append(name)
    return shipped, skipped


def build_game(game, do_test):
    """Run one game's own build.py.  Returns its exit code."""
    args = [sys.executable, "build.py"]
    if do_test:
        args.append("--test")
    args.append("--install")
    return run(args, os.path.join(ROOT, game), check=False)


def collect(game):
    """Assemble install/<game> from what the game built and its runtime artifacts.

    Two folders land there: bin/ and game/ as the game's own install target
    staged them (with the runtime TSV table or patch archive copied beside the
    proxy so game/ is a drop-in patch).  Raw script_output/*.json(.enc) script
    dumps are never copied into install/<game>/.
    """
    src = os.path.join(ROOT, game, "build", "install")
    dst = os.path.join(INSTALL_DIR, game)
    if not os.path.isdir(src):
        print(f"  {game}: nothing staged at {os.path.relpath(src, ROOT)}")
        return False
    # Replace rather than merge: an executable dropped from a game's install
    # target would otherwise keep shipping out of the previous run's tree.
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)

    src_doc = os.path.join(ROOT, game, "script_output", "translated_text.json")
    if not os.path.isfile(src_doc) or os.path.getsize(src_doc) == 0:
        print(f"  [ERROR] {game}: missing or empty translated_text.json in "
              f"{os.path.relpath(os.path.dirname(src_doc), ROOT)}")
        return False

    if os.path.exists(os.path.join(dst, "script_output")):
        print(f"  [ERROR] {game}: script_output/ leaked into staged install: "
              f"{os.path.relpath(os.path.join(dst, 'script_output'), ROOT)}")
        return False

    forbidden_names = {
        "translations_key.txt",
        "encrypt_translations.bat",
        "decrypt_translations.bat",
    }
    leaks = [
        os.path.join(b, f)
        for b, _, fs in os.walk(dst)
        for f in fs
        if f.lower().endswith((".json", ".enc")) or f in forbidden_names
    ]
    if leaks:
        print(f"  [ERROR] {game}: forbidden script/key files leaked into staged install: "
              f"{', '.join(os.path.relpath(p, ROOT) for p in leaks)}")
        return False

    # Verify that <game>/build.py staged the runtime table into game/ and that
    # it is at least as new as script_output/translated_text.json(.enc).
    if game not in REPACK_ONLY_GAMES:
        game_dir = os.path.join(dst, "game")
        tables = [os.path.join(game_dir, n) for n in RUNTIME_TABLES
                  if os.path.isfile(os.path.join(game_dir, n))]
        if not tables or os.path.getsize(tables[0]) == 0:
            print(f"  [ERROR] {game}: missing runtime TSV table in "
                  f"{os.path.relpath(game_dir, ROOT)}")
            return False
        table_mtime = os.path.getmtime(tables[0])
        for src_name in ("translated_text.json", "translated_text.json.enc"):
            src_doc = os.path.join(ROOT, game, "script_output", src_name)
            if os.path.isfile(src_doc) and table_mtime < os.path.getmtime(src_doc):
                print(f"  [ERROR] {game}: staged table {os.path.basename(tables[0])} "
                      f"is older than script_output/{src_name}")
                return False
        if game == "FRATERNITE_HD":
            patch_bin = os.path.join(game_dir, "cgsys_patch.bin")
            if not os.path.isfile(patch_bin) or os.path.getsize(patch_bin) == 0:
                print(f"  [ERROR] {game}: missing or empty cgsys_patch.bin in "
                      f"{os.path.relpath(game_dir, ROOT)}")
                return False
        elif game in (
            "shingakkou",
            "EXTRAVAGANZA_02_MUSHIGURUI_HD10",
            "EXTRAVAGANZA_03_MATSURO_CE",
            "EXTRAVAGANZA_04_AGEHA_CE",
            "EXTRAVAGANZA_05_ANZU_CE",
            "EXTRAVAGANZA_06_YUMEMI_CE",
            "EXTRAVAGANZA_07_REN_MUSHI_CE",
            "EXTRAVAGANZA_08_MIZUNA_CE",
        ):
            patch_bin = os.path.join(game_dir, "menu_patch.bin")
            if not os.path.isfile(patch_bin) or os.path.getsize(patch_bin) == 0:
                print(f"  [ERROR] {game}: missing or empty menu_patch.bin in "
                      f"{os.path.relpath(game_dir, ROOT)}")
                return False
    elif game == "EXTRAVAGANZA_01_CE":
        game_dir = os.path.join(dst, "game")
        patch_bin = os.path.join(game_dir, "extravaganza_patch.bin")
        if not os.path.isfile(patch_bin) or os.path.getsize(patch_bin) == 0:
            print(f"  [ERROR] {game}: missing or empty extravaganza_patch.bin in "
                  f"{os.path.relpath(game_dir, ROOT)}")
            return False
    elif game == "kishin_hisho":
        game_dir = os.path.join(dst, "game")
        for req_name in ("0script.pak", "alpharom_helper.dll", "launch.exe"):
            req_path = os.path.join(game_dir, req_name)
            if not os.path.isfile(req_path) or os.path.getsize(req_path) == 0:
                print(f"  [ERROR] {game}: missing or empty {req_name} in "
                      f"{os.path.relpath(game_dir, ROOT)}")
                return False
        script_dir = os.path.join(game_dir, "script")
        if not os.path.isdir(script_dir):
            print(f"  [ERROR] {game}: missing script/ directory in "
                  f"{os.path.relpath(game_dir, ROOT)}")
            return False
        script_files = [f for f in os.listdir(script_dir)
                        if os.path.isfile(os.path.join(script_dir, f))]
        nps_files = [f for f in script_files if f.lower().endswith(".nps")]
        if len(nps_files) < 119 or len(script_files) < 123:
            print(f"  [ERROR] {game}: incomplete script/ directory "
                  f"({len(nps_files)} .nps, {len(script_files)} total)")
            return False
        src_recon = os.path.join(ROOT, game, "script_output", "nps_reconstruction_map.json")
        if not os.path.isfile(src_recon) or os.path.getsize(src_recon) == 0:
            print(f"  [ERROR] {game}: missing or empty nps_reconstruction_map.json in "
                  f"{os.path.relpath(os.path.dirname(src_recon), ROOT)}")
            return False
        pak_path = os.path.join(game_dir, "0script.pak")
        pak_mtime = os.path.getmtime(pak_path)
        for src_name in (
            "translated_text.json",
            "translated_text.json.enc",
            "nps_reconstruction_map.json",
            "nps_reconstruction_map.json.enc",
        ):
            src_input = os.path.join(ROOT, game, "script_output", src_name)
            if os.path.isfile(src_input) and pak_mtime < os.path.getmtime(src_input):
                print(f"  [ERROR] {game}: staged 0script.pak is older than "
                      f"script_output/{src_name}")
                return False

    files = sum(len(f) for _, _, f in os.walk(dst))
    size = sum(os.path.getsize(os.path.join(b, f))
               for b, _, fs in os.walk(dst) for f in fs)
    print(f"  {game}: {files} file(s), {size / 1048576:.1f} MB "
          f"-> {os.path.relpath(dst, ROOT)}")
    return True


def clean(games):
    for game in games:
        d = os.path.join(ROOT, game, "build")
        if os.path.isdir(d):
            print(f"Removing {os.path.relpath(d, ROOT)}")
            shutil.rmtree(d)
    if os.path.isdir(INSTALL_DIR):
        print(f"Removing {os.path.relpath(INSTALL_DIR, ROOT)}")
        shutil.rmtree(INSTALL_DIR)


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--install", action="store_true",
                   help="build each game and stage it under install/<game>")
    p.add_argument("--test", action="store_true",
                   help="run each game's suite before staging it")
    p.add_argument("--clean", action="store_true",
                   help="delete every game's build tree and the install tree")
    p.add_argument("--only", metavar="GAME", action="append",
                   help="restrict to this game; repeatable")
    p.add_argument("--keep-going", action="store_true",
                   help="carry on after a game fails instead of stopping")
    p.add_argument("--list", action="store_true",
                   help="print which games would be built, and stop")
    args = p.parse_args()

    shipped, skipped = discover()
    if args.only:
        known = set(shipped) | set(skipped)
        unknown = [g for g in args.only if g not in known]
        if unknown:
            print(f"[ERROR] not a shipped game: {', '.join(unknown)}")
            print(f"        shipped: {', '.join(shipped) or '(none)'}")
            return 2
        shipped = [g for g in sorted(known) if g in args.only]

    print(f"Shipped ({len(shipped)}): {', '.join(shipped) or '(none)'}")
    print(f"Ignored ({len(skipped)}): {', '.join(skipped) or '(none)'}")
    if args.list:
        return 0
    if not (args.install or args.test or args.clean):
        p.print_help()
        return 0
    if not shipped:
        print("[ERROR] no game to build -- every candidate is git-ignored")
        return 2

    if args.clean:
        clean(shipped)
        if not (args.install or args.test):
            return 0
    elif args.install and not args.only and os.path.isdir(INSTALL_DIR):
        print(f"Removing {os.path.relpath(INSTALL_DIR, ROOT)}")
        shutil.rmtree(INSTALL_DIR)

    results = []
    for game in shipped:
        print(f"\n{'=' * 70}\n== {game}\n{'=' * 70}")
        rc = build_game(game, args.test)
        staged = collect(game) if rc == 0 and args.install else False
        if args.install and rc == 0 and not staged:
            rc = 1
        results.append((game, rc, staged))
        if rc != 0 and not args.keep_going:
            print(f"\n{game} failed (rc={rc}) -- stopping.  "
                  f"Use --keep-going to build the rest anyway.")
            break

    print(f"\n{'=' * 70}\n== Summary\n{'=' * 70}")
    for game, rc, staged in results:
        state = "ok" if rc == 0 else f"FAILED rc={rc}"
        print(f"  {game:<28} {state}{'  (staged)' if staged else ''}")
    missing = [g for g in shipped if g not in {r[0] for r in results}]
    for game in missing:
        print(f"  {game:<28} not attempted")

    failed = [g for g, rc, _ in results if rc != 0]
    if failed:
        print(f"\n{len(failed)} game(s) failed: {', '.join(failed)}")
        return 1
    if args.install:
        print(f"\nStaged under {os.path.relpath(INSTALL_DIR, ROOT)}: "
              f"{', '.join(g for g, _, s in results if s)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
