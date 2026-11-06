#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 MasakatsuFunaki
#
# Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
# Not licensed for use as training data for machine learning or generative
# AI systems; text and data mining rights are reserved. See NOTICE.

"""Build WelcomeToAnzU_CE (x64 pipeline + 32-bit winmm proxy DLL) into ./build/.

Layout produced under WelcomeToAnzU_CE/build/:
    pipeline/       x64 CMake build tree + Conan generators
    proxy/          x86 CMake build tree + Conan generators
    root/           top-level superbuild (drives both via `cmake --preset default`)
    install/
      bin/          00_run_all.exe .. 04_patch_menus.exe + runtime DLLs
      game/         winmm.dll

Usage:
    python build.py                     # conan + configure + build both halves
    python build.py --test              # also run all unit tests via ctest
    python build.py --install           # stage into build/install/{bin,game}
    python build.py --deploy            # stage + copy winmm.dll to the game dir
    python build.py --clean             # wipe ./build and CMakeUserPresets.json
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"
PIPELINE = ROOT / "pipeline_cpp"
PROXY = ROOT / "proxy_dll"
STAGE = BUILD / "install"
MENU_PATCH_BIN = ROOT / "menu_patch.bin"

GAME_DEFAULT = Path(os.environ["VN_GAME_DIR"]) if os.environ.get("VN_GAME_DIR") else None


def find_vcvarsall() -> Path:
    vswhere = Path(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        "Microsoft Visual Studio", "Installer", "vswhere.exe",
    )
    if vswhere.exists():
        out = subprocess.check_output(
            [str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
            text=True,
        ).strip()
        if out:
            cand = Path(out, "VC", "Auxiliary", "Build", "vcvarsall.bat")
            if cand.exists():
                return cand

    for base in (r"C:\Program Files", r"C:\Program Files (x86)"):
        vs_root = Path(base, "Microsoft Visual Studio")
        if vs_root.exists():
            hits = sorted(vs_root.glob("*/*/VC/Auxiliary/Build/vcvarsall.bat"), reverse=True)
            if hits:
                return hits[0]

    sys.exit("ERROR: vcvarsall.bat not found (is Visual Studio with C++ installed?)")


def capture_msvc_env(vcvarsall: Path, arch: str) -> dict[str, str]:
    """Run vcvarsall.bat <arch> once and return the resulting environment."""
    cmd = f'"{vcvarsall}" {arch} >nul && set'
    out = subprocess.check_output(cmd, shell=True, text=True)
    env: dict[str, str] = {}
    for line in out.splitlines():
        if "=" in line and not line.startswith("="):
            k, _, v = line.partition("=")
            env[k] = v
    return env


def run(cmd: list[str], *, cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
    print(f"\n==> [{cwd.relative_to(ROOT) if cwd != ROOT else '.'}] {' '.join(cmd)}", flush=True)
    rc = subprocess.call(cmd, cwd=cwd, env=env)
    if rc != 0:
        sys.exit(rc)


def clean() -> None:
    if BUILD.exists():
        print(f"Removing {BUILD}")
        shutil.rmtree(BUILD)
    for sub in (PIPELINE, PROXY):
        up = sub / "CMakeUserPresets.json"
        if up.exists():
            up.unlink()


def sanitize_cmake_backslashes(build_dir: Path) -> None:
    """Escape raw Windows backslashes inside quotes in Conan-generated .cmake files."""
    gen_dir = build_dir / "generators"
    if not gen_dir.exists():
        return
    win_path_re = re.compile(r'"([A-Za-z]:\\[^"]*)"')
    for cmake_file in gen_dir.glob("*.cmake"):
        txt = cmake_file.read_text(encoding="utf-8")
        fixed = win_path_re.sub(lambda m: '"' + m.group(1).replace("\\", "/") + '"', txt)
        if fixed != txt:
            cmake_file.write_text(fixed, encoding="utf-8")


def build_half(
    name: str,
    src_dir: Path,
    preset: str,
    vcvars_arch: str,
    conan_args: list[str],
    vcvarsall: Path,
) -> dict[str, str]:
    print(f"\n================ {name} ({vcvars_arch}) ================", flush=True)
    env = capture_msvc_env(vcvarsall, vcvars_arch)
    sub_build = BUILD / ("pipeline" if "x64" in preset else "proxy")
    run(
        [
            "conan",
            "install",
            ".",
            f"--output-folder={sub_build}",
            "--build=missing",
            "-c",
            "tools.cmake.cmaketoolchain:generator=Ninja Multi-Config",
            *conan_args,
        ],
        cwd=src_dir,
        env=env,
    )
    sanitize_cmake_backslashes(sub_build)
    run(["cmake", "--preset", preset], cwd=src_dir, env=env)
    run(["cmake", "--build", "--preset", preset], cwd=src_dir, env=env)
    return env


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clean", action="store_true", help="delete ./build before building")
    ap.add_argument("--test", action="store_true", help="run unit tests for both subprojects")
    ap.add_argument("--install", action="store_true", help="stage artifacts into build/install/")
    ap.add_argument("--deploy", action="store_true", help="stage + copy winmm.dll to the game directory")
    ap.add_argument("--game-dir", type=Path, default=GAME_DEFAULT, help="game folder for --deploy")
    args = ap.parse_args()

    if args.clean:
        clean()

    vcvarsall = find_vcvarsall()
    print(f"Using Visual Studio environment: {vcvarsall}")

    env_x64 = build_half(
        "pipeline_cpp",
        PIPELINE,
        "conan-windows-x64-release",
        "x64",
        [],
        vcvarsall,
    )
    env_x86 = build_half(
        "proxy_dll",
        PROXY,
        "conan-windows-x86-release",
        "x86",
        [f"-pr:h={PROXY / 'profiles' / 'x86'}", "-pr:b=default"],
        vcvarsall,
    )

    run(["cmake", "--preset", "default"], cwd=ROOT, env=env_x64)

    if args.test:
        run(["ctest", "--preset", "conan-windows-x64-release"], cwd=PIPELINE, env=env_x64)
        run(["ctest", "--preset", "conan-windows-x86-release"], cwd=PROXY, env=env_x86)

    if args.install or args.deploy:
        run(["cmake", "--install", str(BUILD / "root")], cwd=ROOT, env=env_x64)
        doc = ROOT / "script_output" / "translated_text.json"
        if doc.is_file():
            exe = STAGE / "bin" / "02_build_tsv.exe"
            game_stage = STAGE / "game"
            game_stage.mkdir(parents=True, exist_ok=True)
            staged_tsv = game_stage / "translation_table.tsv"
            staged_translations_tsv = game_stage / "translations.tsv"
            root_tsv = ROOT / "translation_table.tsv"
            root_translations_tsv = ROOT / "translations.tsv"
            run([str(exe), "--rebuild-table", "--dir", str(ROOT), "--game-dir", str(ROOT), "--out", str(staged_tsv)], cwd=ROOT, env=env_x64)
            shutil.copy2(staged_tsv, staged_translations_tsv)
            shutil.copy2(staged_tsv, root_tsv)
            shutil.copy2(staged_tsv, root_translations_tsv)
            print(f"Staged translation table -> {staged_tsv}, {staged_translations_tsv}, {root_tsv}, and {root_translations_tsv}")
        if MENU_PATCH_BIN.is_file():
            game_stage = STAGE / "game"
            game_stage.mkdir(parents=True, exist_ok=True)
            shutil.copy2(MENU_PATCH_BIN, game_stage / "menu_patch.bin")
            print(f"Staged {MENU_PATCH_BIN} -> {game_stage / 'menu_patch.bin'}")
        print(f"\nStaged pipeline tools -> {STAGE / 'bin'}")
        print(f"Staged proxy DLL      -> {STAGE / 'game' / 'winmm.dll'}")

    if args.deploy:
        if os.environ.get("VN_DIST_BUILD"):
            print("\n[DIST] VN_DIST_BUILD set -- skipping deployment")
            return 0
        if not args.game_dir:
            sys.exit("--deploy needs --game-dir: the install path differs per machine")
        dll = STAGE / "game" / "winmm.dll"
        if not args.game_dir.exists():
            sys.exit(f"ERROR: game directory not found: {args.game_dir}")
        dst = args.game_dir / "winmm.dll"
        shutil.copy2(dll, dst)
        print(f"\nDeployed {dll} -> {dst}")
        staged_tsv = STAGE / "game" / "translation_table.tsv"
        if staged_tsv.is_file():
            dst_tsv = args.game_dir / "translation_table.tsv"
            dst_translations_tsv = args.game_dir / "translations.tsv"
            shutil.copy2(staged_tsv, dst_tsv)
            shutil.copy2(staged_tsv, dst_translations_tsv)
            print(f"Deployed {staged_tsv} -> {dst_tsv} and {dst_translations_tsv}")
        staged_patch = STAGE / "game" / "menu_patch.bin"
        src_patch = staged_patch if staged_patch.is_file() else MENU_PATCH_BIN
        if src_patch.is_file():
            dst_patch = args.game_dir / "menu_patch.bin"
            shutil.copy2(src_patch, dst_patch)
            print(f"Deployed {src_patch} -> {dst_patch}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
