"""Emit CP932 codec digests for ut_cp932.

    python tools/gen_cp932_digest.py

Python's cp932 codec is the parity reference; the C++ encoder is table-driven.
"""
import hashlib

enc = []
for cp in range(0x10000):
    try:
        b = chr(cp).encode('cp932')
    except UnicodeEncodeError:
        continue
    enc.append('%04X:%s' % (cp, b.hex().upper()))
enc_blob = '\n'.join(enc) + '\n'
print('encode entries: %d' % len(enc))
print('encode sha256 : %s' % hashlib.sha256(enc_blob.encode()).hexdigest())

dec = []
for b in range(0x100):
    try:
        s = bytes([b]).decode('cp932')
    except UnicodeDecodeError:
        continue
    dec.append('%02X>%s' % (b, ''.join('%04X' % ord(c) for c in s)))
n_single = len(dec)
n_pairs = 0
for lead in list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD)):
    for trail in list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)):
        n_pairs += 1
        try:
            s = bytes([lead, trail]).decode('cp932')
        except UnicodeDecodeError:
            continue
        dec.append('%02X%02X>%s' % (lead, trail, ''.join('%04X' % ord(c) for c in s)))
dec_blob = '\n'.join(dec) + '\n'
print('decode singles accepted: %d of 256' % n_single)
print('decode pairs tried: %d, accepted: %d' % (n_pairs, len(dec) - n_single))
print('decode sha256 : %s' % hashlib.sha256(dec_blob.encode()).hexdigest())
