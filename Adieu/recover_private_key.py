#!/usr/bin/env python3
"""
Recover Adieu's per-game 16-byte SiglusEngine private key from a runtime
capture produced by the proxy DLL's KeyDumper module (key_dumper.cpp).

Workflow:
  1. With winmm.dll deployed (KeyDumperInit armed), launch SiglusEngine.exe.
     The DLL will trip on the first ~4 calls to FUN_006d1a80 and write
     siglus_key_dump.txt next to the EXE.
  2. Run this script. It reads:
        - siglus_key_dump.txt   (captured plaintext post-decrypt, with
                                 comp_size and decomp_size from [ECX])
        - Scene.pck             (the encrypted blob on disk)
        - SIGLUS_KEY_256        (the universal 256-byte XOR table)
     and prints pk[0..15].
  3. Paste the result into siglus_decoder.py:ADIEU_PRIVATE_KEY.

Key recovery math:
    encrypted_disk[i]
      ⊕ pk[i & 0xF]            (decrypt_1, per-game XOR)
      ⊕ SIGLUS_KEY_256[i & 0xFF] (decrypt_2, universal XOR)
    = plaintext_at_hook[i]

So:
    pk[i & 0xF] = encrypted_disk[i]
                ⊕ SIGLUS_KEY_256[i & 0xFF]
                ⊕ plaintext_at_hook[i]

We get hundreds of independent samples per pk byte (one per 16-byte block of
each captured chunk) — if all samples for a given byte position agree, the
recovery is rock-solid.
"""

import argparse
import os
import re
import struct
import sys
from collections import Counter
from typing import List, Optional, Tuple

# Force UTF-8 stdout so Japanese names print legibly on CP932-default
# Windows consoles. The fail-mode output is meant to be copy-pasted into
# NAME_TRANSLATIONS, so mojibake here would defeat the whole gate.
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except (AttributeError, OSError):
    pass  # older Python or stdout already replaced (e.g. captured)

# Universal SiglusEngine 256-byte XOR key (decrypt_2 in SiglusExtract), lifted
# from xmoezzz/SiglusExtract's UnpackScene.h.  This used to be imported from
# siglus_decoder.py; that module is now pipeline_cpp/src/siglus/, so the two
# constants this one-shot tool needs are inlined here.  They must stay in sync
# with siglus_decoder.cpp (which is what the pipeline actually uses).
SIGLUS_KEY_256 = bytes([
    0x70, 0xF8, 0xA6, 0xB0, 0xA1, 0xA5, 0x28, 0x4F, 0xB5, 0x2F, 0x48, 0xFA, 0xE1, 0xE9, 0x4B, 0xDE,
    0xB7, 0x4F, 0x62, 0x95, 0x8B, 0xE0, 0x03, 0x80, 0xE7, 0xCF, 0x0F, 0x6B, 0x92, 0x01, 0xEB, 0xF8,
    0xA2, 0x88, 0xCE, 0x63, 0x04, 0x38, 0xD2, 0x6D, 0x8C, 0xD2, 0x88, 0x76, 0xA7, 0x92, 0x71, 0x8F,
    0x4E, 0xB6, 0x8D, 0x01, 0x79, 0x88, 0x83, 0x0A, 0xF9, 0xE9, 0x2C, 0xDB, 0x67, 0xDB, 0x91, 0x14,
    0xD5, 0x9A, 0x4E, 0x79, 0x17, 0x23, 0x08, 0x96, 0x0E, 0x1D, 0x15, 0xF9, 0xA5, 0xA0, 0x6F, 0x58,
    0x17, 0xC8, 0xA9, 0x46, 0xDA, 0x22, 0xFF, 0xFD, 0x87, 0x12, 0x42, 0xFB, 0xA9, 0xB8, 0x67, 0x6C,
    0x91, 0x67, 0x64, 0xF9, 0xD1, 0x1E, 0xE4, 0x50, 0x64, 0x6F, 0xF2, 0x0B, 0xDE, 0x40, 0xE7, 0x47,
    0xF1, 0x03, 0xCC, 0x2A, 0xAD, 0x7F, 0x34, 0x21, 0xA0, 0x64, 0x26, 0x98, 0x6C, 0xED, 0x69, 0xF4,
    0xB5, 0x23, 0x08, 0x6E, 0x7D, 0x92, 0xF6, 0xEB, 0x93, 0xF0, 0x7A, 0x89, 0x5E, 0xF9, 0xF8, 0x7A,
    0xAF, 0xE8, 0xA9, 0x48, 0xC2, 0xAC, 0x11, 0x6B, 0x2B, 0x33, 0xA7, 0x40, 0x0D, 0xDC, 0x7D, 0xA7,
    0x5B, 0xCF, 0xC8, 0x31, 0xD1, 0x77, 0x52, 0x8D, 0x82, 0xAC, 0x41, 0xB8, 0x73, 0xA5, 0x4F, 0x26,
    0x7C, 0x0F, 0x39, 0xDA, 0x5B, 0x37, 0x4A, 0xDE, 0xA4, 0x49, 0x0B, 0x7C, 0x17, 0xA3, 0x43, 0xAE,
    0x77, 0x06, 0x64, 0x73, 0xC0, 0x43, 0xA3, 0x18, 0x5A, 0x0F, 0x9F, 0x02, 0x4C, 0x7E, 0x8B, 0x01,
    0x9F, 0x2D, 0xAE, 0x72, 0x54, 0x13, 0xFF, 0x96, 0xAE, 0x0B, 0x34, 0x58, 0xCF, 0xE3, 0x00, 0x78,
    0xBE, 0xE3, 0xF5, 0x61, 0xE4, 0x87, 0x7C, 0xFC, 0x80, 0xAF, 0xC4, 0x8D, 0x46, 0x3A, 0x5D, 0xD0,
    0x36, 0xBC, 0xE5, 0x60, 0x77, 0x68, 0x08, 0x4F, 0xBB, 0xAB, 0xE2, 0x78, 0x07, 0xE8, 0x73, 0xBF,
])

# Historical: an earlier static cryptanalysis got pk[0..3] right but pk[4..5]
# wrong.  Kept only as the cross-check this tool prints at the end.
ADIEU_PRIVATE_KEY_PARTIAL = bytes.fromhex("a27ec9da7d1e0908")


def parse_dump(path: str) -> List[dict]:
    """Parse siglus_key_dump.txt into a list of capture records."""
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    captures = []
    # Each capture block looks like:
    #   CAPTURE #N
    #     ecx=0x... edx=0x... eip=0x...
    #     comp_size  = 0x... (N)
    #     decomp_size= 0x... (N)
    #     bytes_read = N
    #     hex:
    #       <hex> <hex> ...
    #       <hex> ...
    block_re = re.compile(
        r"CAPTURE\s+#(\d+).*?"
        r"comp_size\s*=\s*0x([0-9A-Fa-f]+).*?"
        r"decomp_size\s*=\s*0x([0-9A-Fa-f]+).*?"
        r"hex:\s*(.*?)(?=\nCAPTURE|\Z)",
        re.S,
    )
    for m in block_re.finditer(text):
        idx = int(m.group(1))
        comp = int(m.group(2), 16)
        decomp = int(m.group(3), 16)
        hex_blob = re.sub(r"[^0-9A-Fa-f]", "", m.group(4))
        plaintext = bytes.fromhex(hex_blob)
        captures.append({
            "idx": idx,
            "comp_size": comp,
            "decomp_size": decomp,
            "plaintext": plaintext,
        })
    return captures


def list_pck_chunks(pck_path: str) -> List[Tuple[int, int]]:
    """Return [(file_offset, chunk_size_on_disk)] for every scene chunk."""
    data = open(pck_path, "rb").read()
    sec = []
    for i in range(11):
        sec.append(struct.unpack_from("<II", data, 4 + i * 8))
    scene_idx_off, scene_count = sec[8]
    scene_data_off, _          = sec[9]
    chunks = []
    for i in range(scene_count):
        rel, sz = struct.unpack_from("<II", data, scene_idx_off + i * 8)
        chunks.append((scene_data_off + rel, sz))
    return chunks, data


def recover_keystream(enc: bytes, plain: bytes) -> bytes:
    """For each byte index, pk_keystream[i] = enc[i] ⊕ universal[i&0xFF] ⊕ plain[i]."""
    n = min(len(enc), len(plain))
    return bytes(enc[i] ^ SIGLUS_KEY_256[i & 0xFF] ^ plain[i]
                 for i in range(n))


def fold_to_pk(keystream: bytes, period: int = 16) -> Tuple[bytes, List[int]]:
    """Fold the keystream onto pk[0..period-1] by majority vote.

    Returns (pk, agreement_per_position) where agreement is the count of
    matching samples for the winning value at each position. Anything < N
    samples means inconsistency (= we matched the wrong chunk).
    """
    pk = bytearray(period)
    agreement = []
    for k in range(period):
        slot = keystream[k::period]
        if not slot:
            pk[k] = 0
            agreement.append(0)
            continue
        c = Counter(slot)
        winner, count = c.most_common(1)[0]
        pk[k] = winner
        agreement.append(count)
    return bytes(pk), agreement


def try_match(capture: dict, chunks: List[Tuple[int, int]],
              pck: bytes) -> Optional[Tuple[int, bytes, List[int]]]:
    """Find a chunk on disk whose size matches capture.comp_size, then
    recover pk and check it's consistent (every byte position dominated by
    one value across many samples)."""
    target = capture["comp_size"]
    plain  = capture["plaintext"]
    candidates = [(off, sz) for (off, sz) in chunks if sz == target]
    if not candidates:
        return None
    # Try every candidate and keep the one with cleanest agreement.
    best = None
    for (off, sz) in candidates:
        enc = pck[off:off + min(sz, len(plain))]
        ks = recover_keystream(enc, plain)
        pk, agree = fold_to_pk(ks, 16)
        # Score: total agreement / total samples
        total_samples = sum(min(len(ks) // 16 + (1 if (len(ks) % 16) > k else 0),
                                len(ks)) for k in range(16))
        # Simpler score: sum(agree) - if all bytes agree across all samples,
        # this == len(ks).
        score = sum(agree)
        if best is None or score > best[2]:
            best = (off, pk, score, agree, len(ks))
    return best


def fmt_hex(b: bytes) -> str:
    return " ".join(f"{x:02X}" for x in b)


def resolve_paths() -> Tuple[str, str]:
    parser = argparse.ArgumentParser(
        description="Recover Adieu's 16-byte SiglusEngine private key from runtime captures."
    )
    parser.add_argument(
        "--dump",
        help="Path to siglus_key_dump.txt (defaults to <VN_GAME_DIR>/siglus_key_dump.txt)",
    )
    parser.add_argument(
        "--pck",
        help="Path to Scene.pck (defaults to <VN_GAME_DIR>/Scene.pck)",
    )
    parser.add_argument(
        "--game-dir",
        default=os.environ.get("VN_GAME_DIR", ""),
        help="GameData folder containing siglus_key_dump.txt and Scene.pck (or set VN_GAME_DIR)",
    )
    args = parser.parse_args()

    dump_path = args.dump or (
        os.path.join(args.game_dir, "siglus_key_dump.txt") if args.game_dir else ""
    )
    pck_path = args.pck or (
        os.path.join(args.game_dir, "Scene.pck") if args.game_dir else ""
    )
    if not dump_path or not pck_path:
        parser.error("Provide --dump and --pck, or pass --game-dir / set VN_GAME_DIR.")
    return dump_path, pck_path


def main():
    dump_path, pck_path = resolve_paths()

    if not os.path.exists(dump_path):
        print(f"ERROR: dump file not found: {dump_path}")
        print("Have you launched the game with the new winmm.dll deployed?")
        sys.exit(1)
    if not os.path.exists(pck_path):
        print(f"ERROR: Scene.pck not found: {pck_path}")
        sys.exit(1)

    captures = parse_dump(dump_path)
    if not captures:
        print(f"ERROR: no captures parsed from {dump_path}")
        print("Open the file and check it has CAPTURE #N blocks.")
        sys.exit(1)
    print(f"Parsed {len(captures)} captures from {dump_path}")

    chunks, pck = list_pck_chunks(pck_path)
    print(f"Scene.pck has {len(chunks)} chunks")

    # Recover pk from each capture independently. Any inconsistency between
    # captures means we matched a wrong chunk somewhere.
    print()
    print(f"{'cap':>4}  {'comp_size':>12}  {'matched_chunk_off':>20}  "
          f"{'min_agree':>11}  pk[0..15]")
    print("-" * 100)

    all_pks = []
    for cap in captures:
        result = try_match(cap, chunks, pck)
        if not result:
            print(f"{cap['idx']:>4}  0x{cap['comp_size']:08X}  "
                  f"{'<no chunk match>':>20}")
            continue
        off, pk, score, agree, ks_len = result
        min_agree = min(agree)
        max_agree = max(agree)
        # min_agree should equal ks_len // 16 (or +/-1) if pk is consistent.
        expected = ks_len // 16
        consistent = min_agree >= expected  # all samples agreed
        flag = "OK" if consistent else "??"
        print(f"{cap['idx']:>4}  0x{cap['comp_size']:08X}  "
              f"0x{off:>14X}  {min_agree:>4}/{max_agree:<4} {flag}  "
              f"{fmt_hex(pk)}")
        all_pks.append(pk)

    if not all_pks:
        print("\nERROR: no pk recovered. Either captures didn't match any")
        print("chunk on disk, or the dump file is malformed.")
        sys.exit(2)

    # Cross-check: every capture should yield the same pk.
    print()
    if len(set(all_pks)) == 1:
        pk = all_pks[0]
        print(f"All {len(all_pks)} captures agree.")
    else:
        print(f"WARNING: {len(set(all_pks))} different pks across captures!")
        for i, p in enumerate(all_pks):
            print(f"  cap {i}: {fmt_hex(p)}")
        # Fall back to majority byte-by-byte.
        pk = bytes(Counter(p[k] for p in all_pks).most_common(1)[0][0]
                   for k in range(16))
        print(f"  majority: {fmt_hex(pk)}")

    print()
    print("=" * 60)
    print(f"RECOVERED ADIEU_PRIVATE_KEY (16 bytes):")
    print(f"  hex: {pk.hex()}")
    print(f"  pretty: {fmt_hex(pk)}")
    print("=" * 60)

    # Verify against the partial key we already had from cryptanalysis.
    if pk[:len(ADIEU_PRIVATE_KEY_PARTIAL)] == ADIEU_PRIVATE_KEY_PARTIAL:
        print(f"\nCROSS-CHECK PASSED: pk[0..{len(ADIEU_PRIVATE_KEY_PARTIAL)-1}] "
              f"matches the previously-derived partial key.")
    else:
        print(f"\nCROSS-CHECK FAILED!")
        print(f"  static-derived  pk[0..7] = "
              f"{fmt_hex(ADIEU_PRIVATE_KEY_PARTIAL)}")
        print(f"  runtime-derived pk[0..7] = "
              f"{fmt_hex(pk[:len(ADIEU_PRIVATE_KEY_PARTIAL)])}")
        print(f"  Investigate before trusting this key.")

    print()
    print("Paste into siglus_decoder.py:")
    print(f'    ADIEU_PRIVATE_KEY = bytes.fromhex("{pk.hex()}")')


if __name__ == "__main__":
    main()
