"""Fetches the open-source LDAC decoder (libldacdec) and builds ldacdec_wav.exe with MSVC.

The decoder is used only to verify our LDAC output; it is not part of the product
and is not committed. Sources go to build/ldacdec-src, the binary to build/.

libldacdec is GNU C: ldacdec.h defines min/max/container_of with statement
expressions. We replace those three macros (min/max are only used as plain
statements with side-effect-free arguments). Everything else compiles as is.

usage (from a "x64 Native Tools" prompt or via build-ldacdec.cmd):
    python -I tools/ldacverify/build-ldacdec.py
"""
import os
import re
import subprocess
import sys

REPO = 'https://github.com/anonymix007/libldacdec.git'
COMMIT = 'c90094b15e25aef0e47c6d775fa94aceb36cabbc'  # verified 2026-10-22

root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
src = os.path.join(root, 'build', 'ldacdec-src')
out = os.path.join(root, 'build')

if not os.path.isdir(src):
    subprocess.check_call(['git', 'clone', '-q', REPO, src])
subprocess.check_call(['git', '-C', src, 'checkout', '-q', COMMIT])

header = os.path.join(src, 'ldacdec.h')
text = open(header, encoding='utf-8').read()
start = text.index('#define container_of')
end = text.index('typedef struct Frame frame_t;')
text = (text[:start] +
        '/* MSVC: plain macros instead of GNU statement expressions (patched by build-ldacdec.py) */\n'
        '#define min(a, b) ((a) < (b) ? (a) : (b))\n'
        '#define max(a, b) ((a) > (b) ? (a) : (b))\n\n' +
        text[end:])
open(header, 'w', encoding='utf-8').write(text)

sources = ['libldacdec.c', 'bit_allocation.c', 'huffCodes.c', 'bit_reader.c', 'utility.c', 'imdct.c', 'spectrum.c']
cmd = (['cl', '/nologo', '/O2', '/std:clatest', '/W0', '/DDOUBLE64', '/D_CRT_SECURE_NO_WARNINGS',
        '/I' + src, '/I' + os.path.join(root, 'third_party', 'libldac', 'inc'),
        os.path.join(root, 'tools', 'ldacverify', 'ldacdec_wav.c')] +
       [os.path.join(src, s) for s in sources] +
       ['/Fo' + os.path.join(out, 'ldacdec-obj') + os.sep, '/Fe' + os.path.join(out, 'ldacdec_wav.exe')])
os.makedirs(os.path.join(out, 'ldacdec-obj'), exist_ok=True)
sys.exit(subprocess.call(cmd))
