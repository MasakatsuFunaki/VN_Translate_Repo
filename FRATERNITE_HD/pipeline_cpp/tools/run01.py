"""Step-1 parity: compare retired 01_extract.py against 01_extract.exe.

    python tools/run01.py [--rev REV]

Reads the Python script from git at the baseline rev, runs both over the
synthetic archive, exits 0 when extracted_text.json and stdout match.
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
PROJECT = os.path.dirname(PIPELINE)                        # FRATERNITE_HD/
REPO = os.path.dirname(PROJECT)
BASELINE_REV = 'fd504ae'
EXE = os.path.join(PIPELINE, 'build', 'Release', '01_extract.exe')

DRIVER = '''
import importlib.util, logging, os, sys
logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s',
                    datefmt='%H:%M:%S', stream=sys.stdout)
spec = importlib.util.spec_from_file_location('extract_mod', sys.argv[1])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
mod.PAC_DIR = os.path.join(sys.argv[2], 'pac')
mod.OUTPUT_DIR = os.path.join(sys.argv[3], 'script_output')
mod.OUTPUT_FILE = os.path.join(mod.OUTPUT_DIR, 'extracted_text.json')
os.makedirs(mod.OUTPUT_DIR, exist_ok=True)
sys.argv = ['01_extract.py']
mod.main()
'''


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def normalise(blob):
    """Drop the HH:MM:SS log prefix and the absolute output path."""
    out = []
    for line in blob.decode('utf-8', 'replace').replace('\r\n', '\n').split('\n'):
        line = re.sub(r'^\d\d:\d\d:\d\d ', '', line)
        line = re.sub(r'[A-Za-z]:[\\/].*extracted_text\.json', '<OUT>', line)
        line = re.sub(r'Check that .*', 'Check that <PAC>', line)
        out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rev', default=BASELINE_REV)
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix='frt_step1_')
    try:
        py_src = os.path.join(work, '01_extract.py')
        got = subprocess.run(['git', '-C', REPO, 'show',
                              f'{args.rev}:FRATERNITE_HD/01_extract.py'],
                             capture_output=True)
        if got.returncode != 0:
            raise SystemExit('cannot read 01_extract.py at %s:\n%s'
                             % (args.rev, got.stderr.decode('utf-8', 'replace')))
        with open(py_src, 'wb') as f:
            f.write(got.stdout)

        game = os.path.join(work, 'game')
        subprocess.run([sys.executable, os.path.join(HERE, 'make_ypf.py'), game],
                       check=True, capture_output=True)

        driver = os.path.join(work, 'driver.py')
        with open(driver, 'w', encoding='utf-8') as f:
            f.write(DRIVER)
        py = subprocess.run([sys.executable, driver, py_src, game,
                             os.path.join(work, 'py')], capture_output=True)
        cpp = subprocess.run([EXE, '--game-dir', game, '--dir', os.path.join(work, 'cpp')],
                             capture_output=True)
        print('rc  py=%d cpp=%d' % (py.returncode, cpp.returncode))
        if py.returncode or cpp.returncode:
            print(py.stdout.decode('utf-8', 'replace')[-2000:])
            print(cpp.stdout.decode('utf-8', 'replace')[-2000:])
            return 1

        a = os.path.join(work, 'py', 'script_output', 'extracted_text.json')
        b = os.path.join(work, 'cpp', 'script_output', 'extracted_text.json')
        same_bytes = sha256(a) == sha256(b)
        print('  step1 extracted_text.json  %s  (%s)'
              % ('BYTE-IDENTICAL' if same_bytes else 'DIFFER', sha256(a)))
        same_log = normalise(py.stdout) == normalise(cpp.stdout)
        print('  step1 stdout               %s' % ('IDENTICAL' if same_log else 'DIFFER'))
        if not same_log:
            for x, y in zip(normalise(py.stdout), normalise(cpp.stdout)):
                if x != y:
                    print('    py : %r\n    cpp: %r' % (x, y))
        return 0 if (same_bytes and same_log) else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
