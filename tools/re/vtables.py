"""MSVC vtables of Starfield.exe classes, read-only.
Usage:
  python vtables.py Class [Class ...]               every vtable of each class (object offset, RVA, IDs)
  python vtables.py slots <vtable rva hex | id:N> <slot> [slot ...]
                                                     what each slot points at, with the first instructions
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sfexe  # noqa: E402

exe = sfexe.Exe()
al = sfexe.AddressLibrary()
if len(sys.argv) < 2:
    sys.exit(__doc__)
if sys.argv[1] == "slots":
    vt = al[int(sys.argv[2][3:])] if sys.argv[2].startswith("id:") else int(sys.argv[2], 16)
    print(f"vtable {vt:#x} ({exe.rtti_name(vt)})")
    cs = sfexe.capstone_cs(detail=False)
    for s in map(int, sys.argv[3:]):
        fn = exe.vslot(vt, s)
        print(f"slot {s} (+{8 * s:#x}) -> {fn:#x}, IDs {al.ids_of(fn)}")
        o = exe.off(fn)
        for k, ins in enumerate(cs.disasm(exe.m[o:o + 0x60], fn)):
            print(f"   {ins.address:#x}: {ins.mnemonic} {ins.op_str}")
            if ins.mnemonic in ("ret", "jmp") or k > 14:
                break
else:
    for cls in sys.argv[1:]:
        found = exe.vtables(cls)
        if not found:
            print(cls, "not found")
        for offs, vt in found:
            print(f"{cls}: vtable RVA {vt:#x} (object offset {offs:#x}), IDs {al.ids_of(vt)}")
