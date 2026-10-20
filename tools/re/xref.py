"""Code in Starfield.exe that references a string (rip-relative), or that calls a function, read-only.
Usage:
  python xref.py str <string> [before] [after]    references to a string, with the code around each
  python xref.py calls <rva hex | id:N> [...]      direct E8 calls to a function, with the function each sits in
Needs numpy and capstone.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfexe  # noqa: E402

exe = sfexe.Exe()
al = sfexe.AddressLibrary()
name, va, vs, ra, rs = exe.sec(".text")
text = np.frombuffer(exe.m[ra:ra + rs], np.uint8)
disp = text[:-3].astype(np.int64) | (text[1:-2].astype(np.int64) << 8) | (text[2:-1].astype(np.int64) << 16) | \
    (text[3:].astype(np.int64) << 24)
disp = np.where(disp >= 2 ** 31, disp - 2 ** 32, disp)
pos = np.arange(len(disp), dtype=np.int64)
if len(sys.argv) < 3:
    sys.exit(__doc__)
if sys.argv[1] == "calls":
    for a in sys.argv[2:]:
        tgt = al[int(a[3:])] if a.startswith("id:") else int(a, 16)
        hits = np.nonzero((text[:-4] == 0xE8) & (va + pos[:-1] + 5 + disp[1:] == tgt))[0]
        print(f"{tgt:#x} (IDs {al.ids_of(tgt)}): {len(hits)} direct calls")
        for h in hits[:40]:
            site = va + int(h)
            fn = exe.start_of(site)
            print(f"   from {site:#x} in {fn:#x} (IDs {al.ids_of(fn) if fn else []})" if fn else f"   from {site:#x}")
else:
    cs = sfexe.capstone_cs(detail=False)
    s = sys.argv[2].encode() + b"\0"
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 0x30
    after = int(sys.argv[4]) if len(sys.argv) > 4 else 0x60
    i = exe.m.find(s)
    while i >= 0:
        if exe.m[i - 1] == 0:
            tgt = exe.rva(i)
            hits = np.nonzero(va + pos + 4 + disp == tgt)[0]
            print(f"string at RVA {tgt:#x}: {len(hits)} references")
            for h in hits[:12]:
                at = va + int(h)
                print(f"--- reference at {at:#x}")
                for ins in cs.disasm(exe.m[ra + int(h) - before:ra + int(h) + after], at - before):
                    mark = "  <==" if ins.address <= at < ins.address + ins.size else ""
                    print(f"   {ins.address:#09x}  {ins.mnemonic:6s} {ins.op_str}{mark}")
        i = exe.m.find(s, i + 1)
