"""Build a synthetic bn.ypf from analysys/ybn_samples for offline step-1 testing.

    python tools/make_ypf.py <out_dir>

Seven entries exercise story-first ordering, dedup, sidecars, and encryption.
Deterministic output pinned by ut_extract.EndToEnd_SyntheticArchive.
"""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))          # FRATERNITE_HD/
SAMPLES = os.path.join(PROJECT, 'analysys', 'ybn_samples')

NAME_XOR = 0xFF
YSTB_SCRIPT_KEY = 0x6594DAC3


def xor_ystb(blob):
    """Toggle the YSTB body cipher (the retired 01_extract.decrypt_ystb)."""
    if len(blob) <= 0x20 or not blob.startswith(b'YSTB'):
        return blob
    _, _, _, csz, asz, rsz, osz, _ = struct.unpack_from('<4sIIIIIII', blob, 0)
    if 0x20 + csz + asz + rsz + osz != len(blob):
        return blob
    buf = bytearray(blob)
    off = 0x20
    for sz in (csz, asz, rsz, osz):
        end4 = off + (sz & ~3)
        for i in range(off, end4, 4):
            w = struct.unpack_from('<I', buf, i)[0] ^ YSTB_SCRIPT_KEY
            struct.pack_into('<I', buf, i, w)
        rem = sz & 3
        if rem:
            k = YSTB_SCRIPT_KEY
            for j in range(rem):
                buf[end4 + j] ^= (k & 0xFF)
                k >>= 8
        off += sz
    return bytes(buf)


def sample(name):
    with open(os.path.join(SAMPLES, name), 'rb') as f:
        return f.read()


def build():
    yst = xor_ystb(sample('yst00001.ybn'))   # re-encrypt
    cfg = sample('yscfg.ybn')
    yse = sample('yse.ybn')
    lst = sample('yst_list.ybn')

    files = [
        ('ysbin\\yst00156.ybn', yst),
        ('ysbin\\yst00157.ybn', yst),
        ('ysbin\\yst00001.ybn', yst),
        ('ysbin\\yscfg.ybn', cfg),
        ('ysbin\\yse.ybn', yse),
        ('ysbin\\yst_list.ybn', lst),
        ('ysbin\\yst.ybn', cfg),
    ]

    index_size = sum(27 + len(n) for n, _ in files)
    data_off = 32 + index_size

    index, data = bytearray(), bytearray()
    for name, blob in files:
        packed = zlib.compress(blob, 9)
        nb = bytes(b ^ NAME_XOR for b in name.encode('cp932'))
        index += struct.pack('<I', 0)                     # name_hash (unused)
        index += bytes([len(nb) ^ NAME_XOR])
        index += nb
        index += bytes([0, 1])                            # type, compressed
        index += struct.pack('<II', len(blob), len(packed))
        index += struct.pack('<Q', data_off + len(data))
        index += struct.pack('<I', 0)                     # data_hash (unused)
        data += packed

    hdr = b'YPF\x00' + struct.pack('<III', 490, len(files), index_size) + b'\x00' * 16
    assert len(index) == index_size, (len(index), index_size)
    return hdr + bytes(index) + bytes(data)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'synth')
    pac = os.path.join(out_dir, 'pac')
    os.makedirs(pac, exist_ok=True)
    blob = build()
    path = os.path.join(pac, 'bn.ypf')
    with open(path, 'wb') as f:
        f.write(blob)
    print('%s  %d bytes' % (path, len(blob)))


if __name__ == '__main__':
    main()
