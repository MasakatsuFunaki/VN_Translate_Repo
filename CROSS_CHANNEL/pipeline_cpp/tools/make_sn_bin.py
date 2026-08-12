#!/usr/bin/env python3
"""Build a synthetic sn.bin for step-1 parity checking.

CROSS_CHANNEL is not installed on the build machine, so 01_extract cannot be
verified against the real archive.  This generates a deliberately adversarial
decompressed payload -- every branch of the walker, classifier and the three
CLAUDE.md extractor traps -- and LZSS-encodes it two ways:

  1. all-literal   (flag 0xFF + 8 raw bytes per group)
  2. with real back-references over the multi-megabyte filler run, which
     exercises the ring buffer, its self-extending copy and the 4096 wrap

Both encodings must decode to the identical payload; the script asserts that
using 01_extract.py's own lzss_decompress, so a bug in the generator cannot be
mistaken for a bug in the port.

  python make_sn_bin.py <out_dir>

writes <out_dir>/full_literal/data/sn.bin, full_ref/, small_literal/,
small_ref/ -- "full" has entries past GAME_START_OFFSET (rotation fires),
"small" has none (start_idx == 0, list untouched).
"""
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
# 01_extract.py is retired from the working tree; read it back out of git at
# the migration baseline, the same way the other games' step-1 runners do.
BASELINE_REV = 'fd504ae'
EXTRACT_PY_IN_GIT = 'CROSS_CHANNEL/01_extract.py'

GAME_START_OFFSET = 0x1CDBD3


def _load_extract():
    """Import the retired 01_extract.py -- only for its lzss_decompress, which
    cross-checks that both encodings decode back to the same payload."""
    r = subprocess.run(['git', '-C', REPO, 'show', f'{BASELINE_REV}:{EXTRACT_PY_IN_GIT}'],
                       capture_output=True)
    if r.returncode != 0:
        raise SystemExit(f'cannot read {EXTRACT_PY_IN_GIT} at {BASELINE_REV}:\n'
                         f'{r.stderr.decode("utf-8", "replace")}')
    path = os.path.join(tempfile.mkdtemp(prefix='crc_extract_'), '01_extract.py')
    with open(path, 'wb') as f:
        f.write(r.stdout)
    spec = importlib.util.spec_from_file_location('crc_extract', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# --------------------------------------------------------------------------
# payload
# --------------------------------------------------------------------------

def cp(s):
    return s.encode('cp932')


def speaker(name):
    return b'\x47\x0D\x00' + cp(name) + b'\x00'


def zstr(b):
    return b + b'\x00'


def choice(label, text):
    """A choice-menu option: FF FF <label LE16> 00 00 <text> 00."""
    return b'\xff\xff' + struct.pack('<H', label) + b'\x00\x00' + cp(text) + b'\x00'


NAMES = ['太一', '見里', '美希', '霧', '冬子', '友貴', '桜庭', '曜子', '七香',
         '遊紗', '新川', '福原', 'みゆき', 'ミミ', '美々美', 'ママン', 'ポコ',
         'アキラ', 'ミチル', '重蔵', 'サメ', '？？？', '少女', '少年', '女']


def section_traps():
    """CLAUDE.md §6 / §7 / §9 plus the ordinary classifier branches."""
    out = bytearray()

    # §6 -- pure-printable prefixes that must survive verbatim.
    for s in ('※男心を鷲掴む…', 'Ａ定食は三時間目…', '（佐倉）（遊紗）',
              "FLOWER'Sのもう片方", '…始まりの朝'):
        out += zstr(cp(s))

    # §6 -- control-byte prefixes that must be stripped.
    out += zstr(b'\x05ABC' + cp('「あの日の話」'))
    out += zstr(b'\x18\x02' + cp('校舎の屋上から'))

    # §9 -- opcode operands that are NOT valid CP932 in front of real text.
    # 0x86 0x3A is an invalid lead/trail pair, so a strict decode of the whole
    # chunk fails at byte 0; the walker must slide one byte, not skip the NUL.
    out += zstr(b'\x86\x3a\x59\x45\xff\xff\x63\x01' + cp('「今、何時だ？」'))
    out += zstr(b'\x86\x3a\x02\x11' + cp('放送室は静まり返っていた'))

    # §7 -- two-JP-character choice options behind the preamble (kept) ...
    for i, opt in enumerate(('迫る', '帰る', '屋上', '部室', 'ボケる')):
        out += choice(0x1000 + i, opt)
    # ... and the same shape with three characters, plus bare two-character
    # strings with no preamble (dropped).
    out += choice(0x2000, 'さらに迫る')
    out += zstr(cp('迫る'))
    out += zstr(cp('帰る'))

    # Standalone dialogue (no speaker opcode) and plain narration.
    out += zstr(cp('「誰もいない教室で、俺は一人だった」'))
    out += zstr(cp('放課後の空は、いつもより赤かった。'))

    # Speaker with no following dialogue -- the name must be dropped entirely.
    out += speaker('人体模型')
    out += zstr(cp('ただの説明文がここに入る。'))

    # Strings that must be rejected: too few JP chars, low density, control
    # bytes inside the kept text.
    out += zstr(b'ABCDEFGHIJ')
    out += zstr(cp('あ'))
    out += zstr(cp('あい') + b'abcdefghijklmnop')
    out += zstr(cp('制御') + b'\x07' + cp('文字入り'))

    # Blank / whitespace-only, including U+3000 (bare .strip() eats it).
    out += zstr(cp('　　　'))
    return bytes(out)


def section_dialogue(count, tag):
    out = bytearray()
    for i in range(count):
        name = NAMES[i % len(NAMES)]
        out += speaker(name)
        out += zstr(cp('「%s、%d回目の水曜日だ」' % (tag, i)))
        if i % 3 == 0:
            out += zstr(cp('%s窓の外で、%d羽の鳥が鳴いた。' % (tag, i)))
        if i % 5 == 0:
            # Speaker immediately followed by another speaker: the first name
            # is dropped, the second pairs with the dialogue after it.
            out += speaker(name)
            out += speaker('声')
            out += zstr(cp('「%s、聞こえているか」' % tag))
    return bytes(out)


def build_payload(with_rotation):
    out = bytearray()
    out += b'\x00' * 16
    out += section_traps()
    out += section_dialogue(220, 'まえ')
    if with_rotation:
        # Filler up to just before the game-start offset, then the scenes the
        # player actually sees first.
        pad = GAME_START_OFFSET - len(out) - 64
        assert pad > 0, 'prologue outgrew the game-start offset'
        out += b'\x00' * pad
        out += speaker('桜庭')
        out += zstr(cp('「今、何時だ？」'))
        out += section_dialogue(40, 'あと')
        out += section_traps()
    return bytes(out)


# --------------------------------------------------------------------------
# LZSS encoders (decoder in 01_extract.py:lzss_decompress)
# --------------------------------------------------------------------------

class Encoder:
    """Emits an LZSS stream while mirroring the decoder's ring exactly, so a
    reference is only ever emitted for bytes the decoder will actually
    reproduce."""

    def __init__(self):
        self.ring = bytearray(b'\x20' * 4096)
        self.ring_pos = 0xFEE
        self.out = bytearray()
        self._items = []   # (is_literal, encoded bytes) -- 8 per flag byte

    def _add(self, is_literal, encoded):
        self._items.append((is_literal, encoded))
        if len(self._items) == 8:
            self._flush()

    def _flush(self):
        if not self._items:
            return
        flags = 0
        for k, (is_literal, _) in enumerate(self._items):
            if is_literal:
                flags |= (1 << k)          # LSB first
        self.out.append(flags)
        for _, encoded in self._items:
            self.out += encoded
        self._items = []

    def literal(self, b):
        self._add(True, bytes([b]))
        self.ring[self.ring_pos] = b
        self.ring_pos = (self.ring_pos + 1) & 0xFFF

    def reference(self, off, length):
        assert 3 <= length <= 18 and 0 <= off <= 0xFFF
        self._add(False, bytes([off & 0xFF, ((off >> 4) & 0xF0) | (length - 3)]))
        for j in range(length):
            b = self.ring[(off + j) & 0xFFF]
            self.ring[self.ring_pos] = b
            self.ring_pos = (self.ring_pos + 1) & 0xFFF

    def finish(self):
        # No padding: the decoder's `si >= src_len` check at the top of every
        # bit iteration ends a partial group cleanly.
        self._flush()
        return bytes(self.out)


def encode_literal(payload):
    out = bytearray()
    for i in range(0, len(payload), 8):
        out.append(0xFF)
        out += payload[i:i + 8]
    return bytes(out)


def encode_with_refs(payload):
    """Literals everywhere except runs of a repeated byte, which become
    self-extending back-references (offset = the ring slot just written)."""
    enc = Encoder()
    i = 0
    n = len(payload)
    while i < n:
        run = 1
        while i + run < n and payload[i + run] == payload[i]:
            run += 1
        if run < 8:
            for k in range(run):
                enc.literal(payload[i + k])
            i += run
            continue
        enc.literal(payload[i])
        i += 1
        run -= 1
        while run >= 3:
            length = min(18, run)
            off = (enc.ring_pos - 1) & 0xFFF   # the byte just written
            enc.reference(off, length)
            run -= length
            i += length
        for _ in range(run):
            enc.literal(payload[i])
            i += 1
    return enc.finish()


def write_case(root, name, payload, stream, decompress):
    got = decompress(struct.pack('<I', len(payload)) + stream)
    assert got == payload, '%s: encoder/decoder disagree (%d vs %d bytes)' % (
        name, len(got), len(payload))
    d = os.path.join(root, name, 'data')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'sn.bin'), 'wb') as f:
        f.write(struct.pack('<I', len(payload)) + stream)
    print('  %-14s payload %9d  stream %9d' % (name, len(payload), len(stream)))


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    root = sys.argv[1]
    decompress = _load_extract().lzss_decompress

    for name, rotate in (('full', True), ('small', False)):
        payload = build_payload(rotate)
        write_case(root, name + '_literal', payload, encode_literal(payload), decompress)
        write_case(root, name + '_ref', payload, encode_with_refs(payload), decompress)
    print('corpus written to', root)


if __name__ == '__main__':
    main()
