"""Step-1 parity: run the RETIRED 01_extract.py and 01_extract.exe over the
same synthetic sn.bin corpora and compare.

The game is not installed on this machine, so the reference corpus is the four
archives make_sn_bin.py builds (literal / back-referenced LZSS x with / without
entries past GAME_START_OFFSET). The Python script is gone from the working
tree; it is read back out of git at the migration baseline, the same way
the other games' step-1 runners do it.

Usage:
    python tools/run01.py [--rev REV] [--corpus full_ref]

Exit 0 = every corpus produced a byte-identical extracted_text.json and the
same stdout (timestamps and absolute paths aside).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PIPELINE = os.path.dirname(HERE)
PROJECT = os.path.dirname(PIPELINE)                        # CROSS_CHANNEL/
REPO = os.path.dirname(PROJECT)
BASELINE_REV = 'fd504ae'
EXE = os.path.join(PIPELINE, 'build', 'Release', '01_extract.exe')
CORPORA = ['full_literal', 'full_ref', 'small_literal', 'small_ref']

# The retired script derives every path from module constants, so redirect
# those rather than editing the source.
DRIVER = '''
import importlib.util, logging, os, sys
logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s',
                    datefmt='%H:%M:%S', stream=sys.stdout)
spec = importlib.util.spec_from_file_location('extract_mod', sys.argv[1])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
mod.GAME_DIR = sys.argv[2]
mod.SN_BIN = os.path.join(sys.argv[2], 'data', 'sn.bin')
mod.OUTPUT_DIR = os.path.join(sys.argv[3], 'script_output')
mod.OUTPUT_FILE = os.path.join(mod.OUTPUT_DIR, 'extracted_text.json')
os.makedirs(mod.OUTPUT_DIR, exist_ok=True)
sys.argv = ['01_extract.py']
# The __main__ block's banner, minus _run_gate_tests() -- those are pytest
# cases against the retired implementation/tests/, which the port replaced
# with GoogleTest and which write nothing to this stdout.
mod.log.info("=== Step 1: Extract text from sn.bin ===")
mod.log.info(f"Game directory: {mod.GAME_DIR}\\n")
mod.extract_all()
'''


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def normalise(blob):
    """Drop the HH:MM:SS log prefix and any absolute path."""
    out = []
    for line in blob.decode('utf-8', 'replace').replace('\r\n', '\n').split('\n'):
        line = re.sub(r'^\d\d:\d\d:\d\d ', '', line)
        line = re.sub(r'[A-Za-z]:[\\/][^\s]*extracted_text\.json', '<OUT>', line)
        line = re.sub(r'[A-Za-z]:[\\/][^\s]*sn\.bin', '<SN>', line)
        line = re.sub(r'(Game directory:|Reading:)\s*.*', r'\1 <DIR>', line)
        out.append(line)
    return out


def run_corpus(py_src, work, name):
    game = os.path.join(work, 'corpus', name)
    driver = os.path.join(work, 'driver.py')
    py_out = os.path.join(work, name, 'py')
    cpp_out = os.path.join(work, name, 'cpp')

    py = subprocess.run([sys.executable, driver, py_src, game, py_out], capture_output=True)
    cpp = subprocess.run([EXE, '--game-dir', game, '--dir', cpp_out], capture_output=True)
    if py.returncode or cpp.returncode:
        print('  %-14s rc py=%d cpp=%d' % (name, py.returncode, cpp.returncode))
        print(py.stderr.decode('utf-8', 'replace')[-2000:])
        print(cpp.stdout.decode('utf-8', 'replace')[-2000:])
        return False

    a = os.path.join(py_out, 'script_output', 'extracted_text.json')
    b = os.path.join(cpp_out, 'script_output', 'extracted_text.json')
    same_bytes = sha256(a) == sha256(b)
    same_log = normalise(py.stdout) == normalise(cpp.stdout)
    print('  %-14s json %-14s stdout %s  (%s)'
          % (name,
             'BYTE-IDENTICAL' if same_bytes else 'DIFFER',
             'IDENTICAL' if same_log else 'DIFFER',
             sha256(a)[:16]))
    if not same_log:
        for x, y in zip(normalise(py.stdout), normalise(cpp.stdout)):
            if x != y:
                print('    py : %r\n    cpp: %r' % (x, y))
    return same_bytes and same_log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rev', default=BASELINE_REV)
    ap.add_argument('--corpus', action='append', choices=CORPORA,
                    help='only this corpus (repeatable); default: all four')
    args = ap.parse_args()

    if not os.path.exists(EXE):
        raise SystemExit('!! %s not built' % EXE)

    work = tempfile.mkdtemp(prefix='crc_step1_')
    try:
        py_src = os.path.join(work, '01_extract.py')
        got = subprocess.run(['git', '-C', REPO, 'show',
                              f'{args.rev}:CROSS_CHANNEL/01_extract.py'], capture_output=True)
        if got.returncode != 0:
            raise SystemExit('cannot read 01_extract.py at %s:\n%s'
                             % (args.rev, got.stderr.decode('utf-8', 'replace')))
        with open(py_src, 'wb') as f:
            f.write(got.stdout)
        with open(os.path.join(work, 'driver.py'), 'w', encoding='utf-8') as f:
            f.write(DRIVER)

        subprocess.run([sys.executable, os.path.join(HERE, 'make_sn_bin.py'),
                        os.path.join(work, 'corpus')], check=True, capture_output=True)

        ok = True
        for name in (args.corpus or CORPORA):
            ok &= run_corpus(py_src, work, name)
        return 0 if ok else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
