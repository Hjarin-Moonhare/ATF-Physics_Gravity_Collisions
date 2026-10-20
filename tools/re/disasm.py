"""Disassemble a Starfield.exe function, read-only.
Usage:
  python disasm.py <rva hex | id:N> [end rva hex]   the function starting at (or holding) the address, to its .pdata end
  python disasm.py at <rva hex> <before> <after>     the instructions around an address (byte counts)
Direct call and jump targets are named by their Address Library IDs; rip-relative .rdata operands show as 4 floats.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfexe  # noqa: E402

exe = sfexe.Exe()
al = sfexe.AddressLibrary()
cs = sfexe.capstone_cs()


def addr(text):
    return al[int(text[3:])] if text.startswith("id:") else int(text, 16)


if len(sys.argv) < 2:
    sys.exit(__doc__)
if sys.argv[1] == "at":
    a = addr(sys.argv[2])
    start, end = a - int(sys.argv[3]), a + int(sys.argv[4])
else:
    a = addr(sys.argv[1])
    start = exe.start_of(a) or a
    end = int(sys.argv[2], 16) if len(sys.argv) > 2 else exe.end_of(start)
print(f"; {start:#x}..{end:#x}, IDs {al.ids_of(start)}")
for i in sfexe.disasm(exe, cs, start, end):
    note = ""
    if i["c"]:
        note += f"   ; {tuple(round(x, 6) for x in i['c'])}"
    if i["call"] is not None:
        note += f"   ; ID {al.ids_of(i['call'])}"
    print(f"{i['a']:#x}: {i['m']:8s} {i['o']}{note}")
