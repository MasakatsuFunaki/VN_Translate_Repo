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
    install/<game>/script_output  the extraction and its translation

`bin` and `game` must stay apart because the proxy is 32-bit and carries a
system DLL's name; flattened beside the x64 executables the loader would pick
it up and then refuse it.

WHICH GAMES: every top-level folder holding a build.py and a CMakeLists.txt
that git does not ignore.  .gitignore is the manifest -- a game listed there is
neither tracked nor shipped, so the set that builds equals the set that
releases, with nothing to keep in sync by hand.
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.realpath(__file__))
INSTALL_DIR = os.path.join(ROOT, "install")

# The runtime table, wherever a game keeps it.  It goes beside the proxy in
# game/, because that pair is the drop-in patch: copy game/ into the game
# folder and it speaks English.
RUNTIME_TABLES = ("translations.tsv", "translation_table.tsv")


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
        (skipped if git_ignores(name) else shipped).append(name)
    return shipped, skipped


def build_game(game, do_test):
    """Run one game's own build.py.  Returns its exit code."""
    args = [sys.executable, "build.py"]
    if do_test:
        args.append("--test")
    args.append("--install")
    return run(args, os.path.join(ROOT, game), check=False)


def collect(game):
    """Assemble install/<game> from what the game built and what it translated.

    Three things land there: bin/ and game/ as the game's own install target
    staged them, the translation output under script_output/, and the runtime
    table copied beside the proxy so game/ is a drop-in patch.  A release is
    only useful with the translation in it -- without the table the download
    is a toolchain the user has to spend their own API budget to feed.
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

    # Top-level files only.  script_output also holds the CG working folders
    # (narrative_extracted alone is over a gigabyte of bitmaps carved out of
    # the archives) -- regenerable, enormous, and no use to anyone downloading
    # a patch.  The documents at the top are the translation itself.
    text_src = os.path.join(ROOT, game, "script_output")
    if os.path.isdir(text_src):
        text_dst = os.path.join(dst, "script_output")
        os.makedirs(text_dst, exist_ok=True)
        for name in sorted(os.listdir(text_src)):
            f = os.path.join(text_src, name)
            if os.path.isfile(f):
                shutil.copy2(f, os.path.join(text_dst, name))
    else:
        print(f"  {game}: no script_output -- shipping binaries only")

    # The table lives at the game's root (it is what --deploy copies), so it
    # is not part of the install target and has to be picked up by name.
    for name in RUNTIME_TABLES:
        table = os.path.join(ROOT, game, name)
        if os.path.isfile(table):
            game_dir = os.path.join(dst, "game")
            os.makedirs(game_dir, exist_ok=True)
            shutil.copy2(table, os.path.join(game_dir, name))

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
        unknown = [g for g in args.only if g not in shipped]
        if unknown:
            print(f"[ERROR] not a shipped game: {', '.join(unknown)}")
            print(f"        shipped: {', '.join(shipped) or '(none)'}")
            return 2
        shipped = [g for g in shipped if g in args.only]

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

    results = []
    for game in shipped:
        print(f"\n{'=' * 70}\n== {game}\n{'=' * 70}")
        rc = build_game(game, args.test)
        staged = collect(game) if rc == 0 and args.install else False
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
