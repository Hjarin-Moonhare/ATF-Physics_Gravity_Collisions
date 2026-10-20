"""Address Library lookups, read-only.
Usage:
  python addrlib.py <ID> [ID ...]            the RVA of each ID
  python addrlib.py rva <hex> [hex ...]      the IDs of each RVA (the function holding it, when it isn't a start)
  python addrlib.py plugin                    every ID TF Physics uses (src\\plugin.cpp's tables), with its RVA
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfexe  # noqa: E402

al = sfexe.AddressLibrary()
print(f"{al.path}: format {al.format}, game {'.'.join(map(str, al.version))}, {len(al.rvas)} IDs")
if len(sys.argv) < 2:
    sys.exit(__doc__)
if sys.argv[1] == "rva":
    exe = sfexe.Exe()
    for h in sys.argv[2:]:
        r = int(h, 16)
        ids = al.ids_of(r)
        start = exe.start_of(r)
        extra = f" (in the function at {start:#x}, IDs {al.ids_of(start)})" if not ids and start else ""
        print(f"RVA {r:#x}: IDs {ids}{extra}")
elif sys.argv[1] == "plugin":
    src = open(os.path.join(os.path.dirname(__file__), "..", "..", "src", "plugin.cpp"), encoding="utf-8").read()
    for idn, off, what in re.findall(r"\{(\d+), (0x[0-9A-Fa-f]+|0), \"([^\"]+)\"\}", src):
        print(f"ID {idn:>7} +{off:<6} -> RVA {al[int(idn)] + int(off, 16):#x}  {what}")
else:
    for a in sys.argv[1:]:
        print(f"ID {a}: RVA {al[int(a)]:#x}")
