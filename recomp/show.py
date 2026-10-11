#!/usr/bin/env python3
"""Print a range of lf2.exe's disassembly compactly: show.py START END (hex)."""
import os, re, subprocess, sys
a, b = int(sys.argv[1], 16), int(sys.argv[2], 16)
exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'game', 'lf2.exe')
asm = subprocess.run(['objdump', '-d', '--x86-asm-syntax=intel', '--section=.text',
                      '--start-address=0x%x' % a, '--stop-address=0x%x' % b, exe],
                     capture_output=True, text=True, check=True).stdout
for l in asm.splitlines():
    m = re.match(r'^\s*([0-9a-f]+):((?: [0-9a-f]{2})+)\s*\t(.*)$', l)
    if m and 'int3' not in m.group(3):
        print(m.group(1)[2:], re.sub(r' <.*?>', '', m.group(3).replace('\t', ' ')
                                     .replace('dword ptr ', '').replace('0x', '')))
