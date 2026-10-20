"""Read-only helpers for reading Starfield.exe from disk: sections, RVA <-> file offset, function ends from .pdata,
MSVC vtables by class name, the Address Library database (format 5), and capstone disassembly.

The game folder: the STARFIELD_DIR environment variable, else the usual Steam folders. Needs capstone for the
disassembly parts (pip install capstone). Nothing here writes to the game folder.
"""
import bisect
import mmap
import os
import struct

USUAL = [r"C:\Program Files (x86)\Steam\steamapps\common\Starfield",
         r"D:\SteamLibrary\steamapps\common\Starfield",
         r"E:\SteamLibrary\steamapps\common\Starfield"]


def game_dir():
    d = os.environ.get("STARFIELD_DIR")
    if d:
        return d
    for d in USUAL:
        if os.path.exists(os.path.join(d, "Starfield.exe")):
            return d
    raise SystemExit("Starfield not found: set STARFIELD_DIR to the game folder")


def exe_version(path=None):
    """(major, minor, build, private) of Starfield.exe, from its VS_FIXEDFILEINFO (signature 0xFEEF04BD)."""
    path = path or os.path.join(game_dir(), "Starfield.exe")
    with open(path, "rb") as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        i = m.rfind(struct.pack("<I", 0xFEEF04BD))
        if i < 0:
            raise SystemExit(f"{path}: no version resource")
        ms, ls = struct.unpack_from("<II", m, i + 8)
        return ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF


class Exe:
    def __init__(self, path=None):
        path = path or os.path.join(game_dir(), "Starfield.exe")
        self.path = path
        self.f = open(path, "rb")
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        m = self.m
        pe, = struct.unpack_from("<I", m, 0x3C)
        nsec, = struct.unpack_from("<H", m, pe + 6)
        opt, = struct.unpack_from("<H", m, pe + 20)
        self.base, = struct.unpack_from("<Q", m, pe + 24 + 24)
        self.secs = []
        for i in range(nsec):
            o = pe + 24 + opt + 40 * i
            name = m[o:o + 8].rstrip(b"\0").decode()
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", m, o + 8)
            self.secs.append((name, vaddr, vsize, raddr, rsize))
        pd = self.sec(".pdata")
        self.pdata = sorted((b, e) for b, e, u in (struct.unpack_from("<III", m, pd[3] + 12 * k) for k in range(pd[4] // 12)) if b)
        self.pstart = [b for b, e in self.pdata]

    def sec(self, name):
        return next(s for s in self.secs if s[0] == name)

    def off(self, rva):
        """File offset of an RVA, or None."""
        for n, va, vs, ra, rs in self.secs:
            if va <= rva < va + rs:
                return ra + rva - va
        return None

    def rva(self, off):
        """RVA of a file offset, or None."""
        for n, va, vs, ra, rs in self.secs:
            if ra <= off < ra + rs:
                return va + off - ra
        return None

    def section_of(self, rva):
        for n, va, vs, ra, rs in self.secs:
            if va <= rva < va + vs:
                return n
        return None

    def u64(self, rva):
        return struct.unpack_from("<Q", self.m, self.off(rva))[0]

    def start_of(self, rva):
        """Start of the .pdata range holding rva, or None. For a function split into chunks this is the chunk's start."""
        i = bisect.bisect_right(self.pstart, rva) - 1
        if i >= 0 and self.pdata[i][0] <= rva < self.pdata[i][1]:
            return self.pdata[i][0]
        return None

    def end_of(self, rva, fallback=0x900):
        """End of the function starting at (or holding) rva, from .pdata."""
        i = bisect.bisect_right(self.pstart, rva) - 1
        if i >= 0 and self.pdata[i][0] <= rva < self.pdata[i][1]:
            e = self.pdata[i][1]
            while i + 1 < len(self.pdata) and self.pdata[i + 1][0] == e:
                i += 1
                e = self.pdata[i][1]
            return e
        return rva + fallback

    def vtables(self, cls):
        """[(object offset, vtable RVA)] of an MSVC class, via TypeDescriptor -> CompleteObjectLocator -> vtable."""
        i = self.m.find(f".?AV{cls}@@".encode() + b"\0")
        if i < 0:
            return []
        td = self.rva(i - 0x10)
        rd = self.sec(".rdata")
        blob = self.m[rd[3]:rd[3] + rd[4]]
        found = []
        j = blob.find(struct.pack("<I", td))
        while j >= 0:
            col = j - 12
            sig, offs, cdo, t, cd, me = struct.unpack_from("<6I", blob, col)
            if sig == 1 and me == rd[1] + col:
                k = blob.find(struct.pack("<Q", self.base + rd[1] + col))
                if k >= 0:
                    found.append((offs, rd[1] + k + 8))
            j = blob.find(struct.pack("<I", td), j + 1)
        return sorted(found)

    def vslot(self, vtable_rva, slot):
        """RVA of a vtable's entry."""
        return self.u64(vtable_rva + 8 * slot) - self.base

    def rtti_name(self, vtable_rva):
        """The class name a vtable belongs to (vtable[-1] -> COL -> TypeDescriptor), or None."""
        col = self.u64(vtable_rva - 8) - self.base
        o = self.off(col)
        if o is None:
            return None
        td, = struct.unpack_from("<I", self.m, o + 12)
        to = self.off(td + 0x10)
        if to is None:
            return None
        return self.m[to:self.m.find(b"\0", to)].decode(errors="replace")


class AddressLibrary:
    """Data\\SFSE\\Plugins\\versionlib-<a>-<b>-<c>-0.bin, format 5: a 0x60-byte header (format, game version x4,
    name[64], pointer size, ..., ID count at +0x5C), then one uint32 RVA per ID (the ID is the index)."""

    def __init__(self, path=None, version=None):
        """path: a database file; else the installed game's own (version: "1.16.244", default the exe's version)."""
        if path is None:
            plugins = os.path.join(game_dir(), "Data", "SFSE", "Plugins")
            version = version or ".".join(map(str, exe_version()[:3]))
            path = os.path.join(plugins, f"versionlib-{version.replace('.', '-')}-0.bin")
            if not os.path.exists(path):
                raise SystemExit(f"{path} not found: install Address Library for SFSE Plugins for game {version}")
        raw = open(path, "rb").read()
        self.path = path
        self.format, *self.version = struct.unpack_from("<5i", raw, 0)
        if self.format != 5:
            raise SystemExit(f"{path}: format {self.format}, not 5")
        count, = struct.unpack_from("<I", raw, 0x5C)
        self.rvas = struct.unpack_from(f"<{count}I", raw, 0x60)
        self.ids = {}
        for i, r in enumerate(self.rvas):
            if r:
                self.ids.setdefault(r, []).append(i)

    def __getitem__(self, idn):
        return self.rvas[idn]

    def ids_of(self, rva):
        return self.ids.get(rva, [])


def capstone_cs(detail=True):
    try:
        import capstone
    except ImportError:
        raise SystemExit("needs capstone: pip install capstone")
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    cs.detail = detail
    return cs


def disasm(exe, cs, start, end):
    """[{a: address, m: mnemonic, o: operands, c: 4 floats at a rip-relative .rdata operand or None, call: target}]"""
    from capstone import x86
    code = exe.m[exe.off(start):exe.off(start) + (end - start)]
    out = []
    for ins in cs.disasm(code, start):
        const = None
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                tgt = ins.address + ins.size + op.mem.disp
                o = exe.off(tgt)
                if o is not None and exe.section_of(tgt) == ".rdata":
                    const = struct.unpack_from("<4f", exe.m, o)
        call = ins.operands[0].imm if ins.mnemonic in ("call", "jmp") and ins.operands and \
            ins.operands[0].type == x86.X86_OP_IMM else None
        out.append({"a": ins.address, "m": ins.mnemonic, "o": ins.op_str, "c": const, "call": call})
    return out
