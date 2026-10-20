# Reading Starfield.exe

These are the small read-only tools I used to find what TF Physics touches in the game. They map `Starfield.exe`
from disk and never write anything. `docs\ENGINE_NOTES.md` lists what they found.

## Setup

- **Python 3.10 or newer,** plus `pip install capstone numpy`.
- **The game folder:** set `STARFIELD_DIR` to it. Without it, the tools try `C:\Program Files (x86)\Steam`,
  `D:\SteamLibrary` and `E:\SteamLibrary` (each `\steamapps\common\Starfield`).
- **Address Library for SFSE Plugins,** installed: `Data\SFSE\Plugins\versionlib-<version>-0.bin`. The tools read the
  version from `Starfield.exe` itself and load the database for it, or stop and say it's missing.
- **No file here may share a name with a Python module.** A `dis.py` here once hid the standard `dis`, which `inspect`
  imports, and capstone and numpy stopped loading.

Run them from the package folder: `python tools\re\<tool>.py ...`.

- **Addresses are RVAs:** offsets from the start of the loaded exe, for the installed game version. An ID stays the
  same from one game version to the next, but its RVA changes. That's why the plugin stores IDs.
- **Writing addresses:** hex, with or without `0x`. Where a tool says `id:N`, an Address Library ID works too.
- **Byte counts** (`before`, `after`) are decimal.

| Tool | What it does |
|---|---|
| `addrlib.py` | IDs to RVAs and back, and every ID the plugin uses. |
| `disasm.py` | A function's code, or the code around an address. |
| `vtables.py` | A class's vtables, and what their slots point at. |
| `xref.py` | Who calls a function, and which code uses a string. |
| `sfexe.py` | The readers the others share, for scripts of your own. |

Run with nothing after it, each tool prints its usage.

## addrlib.py

Every run starts with the database it read:

```
...\Data\SFSE\Plugins\versionlib-1-16-244-0.bin: format 5, game 1.16.244.0, 1274968 IDs
```

**An ID's RVA:**

```
python tools\re\addrlib.py 125392 54894
ID 125392: RVA 0x2378260
ID 54894: RVA 0x872770
```

**An RVA's IDs.** An address inside a function gives the function's start and its IDs:

```
python tools\re\addrlib.py rva 2378260 2378280
RVA 0x2378260: IDs [125392]
RVA 0x2378280: IDs [] (in the function at 0x2378260, IDs [125392])
```

**Every ID the plugin uses,** read from the `{ID, offset, "name"}` entries in `src\plugin.cpp`'s address tables, with
the RVA in the installed game. The offset is added to the function's RVA (it marks a call site inside a function):

```
python tools\re\addrlib.py plugin
ID   54894 +0      -> RVA 0x872770  modifier loop
ID   83224 +0x774  -> RVA 0x131f684  call site A
```

After a game update and its new Address Library, this shows where each address is now. The plugin's start-up checks
still decide whether the code there is what it expects.

## disasm.py

**A whole function.** Give an RVA or `id:N`. An address inside a function starts from the function's start, and the
end comes from `.pdata` (an address outside every function reads 0x900 bytes from it). A second address ends it
sooner:

```
python tools\re\disasm.py id:125392
python tools\re\disasm.py 2378260 2378270
; 0x2378260..0x2378270, IDs [125392]
0x2378260: mov      rax, rsp
0x2378263: mov      qword ptr [rax + 8], rbx
0x2378267: mov      qword ptr [rax + 0x18], rsi
0x237826b: mov      qword ptr [rax + 0x20], rdi
```

Each direct `call` or `jmp` is named by its target's IDs. Each `rip`-relative operand that points into `.rdata` gets
the 16 bytes there, shown as four floats:

```
0x23782ba: call     0x365d488   ; ID [182821]
0x23783a2: lea      rax, [rip + 0x279a6a7]   ; (55.056904, 1.7444122325366847e+22, ...)
```

The floats only mean something when the instruction reads floats (`movss`, `vmulps` and the like). Behind a `lea`
there's usually a table or a string.

**The code around an address:** `at <address> <bytes before> <bytes after>`:

```
python tools\re\disasm.py at 2378280 8 12
```

The window starts exactly that many bytes back, which can be mid-instruction: then the first lines can be wrong, or
the listing can stop early. An instruction cut by the window's end isn't shown. For a clean read, start from the
function.

## vtables.py

**A class's vtables,** by its C++ name, as MSVC's RTTI spells it without `.?AV` and `@@`. A class in a namespace is
`Class@Namespace`. A class with more than one base has a vtable per base, at that base's offset in the object:

```
python tools\re\vtables.py hknpBSWorld
hknpBSWorld: vtable RVA 0x4d62020 (object offset 0x0), IDs [471046]
hknpBSWorld: vtable RVA 0x4d62040 (object offset 0x18), IDs [471050]
hknpBSWorld: vtable RVA 0x4d622c0 (object offset 0x20), IDs [471048]
```

It looks up `class` names only. A type declared `struct` (`.?AU` in RTTI) and a mistyped name both print "not found".

**What slots point at:** a vtable (RVA or `id:N`), then slot numbers. For each one: the function, its IDs, and its
first instructions (up to 16, or to the first `ret` or `jmp`):

```
python tools\re\vtables.py slots id:471048 6
vtable 0x4d622c0 (.?AVhknpBSWorld@@)
slot 6 (+0x30) -> 0x2369020, IDs [125224]
   0x2369020: mov rax, qword ptr [rcx + 0xa38]
   0x2369027: ret
```

A short getter like this one shows where a field lives.

## xref.py

Both forms scan the whole `.text` section, which takes about 2 s.

**Who calls a function:** every direct `call` to it, and the function each one sits in. Up to 40 are listed, and the
count is all of them:

```
python tools\re\xref.py calls id:125392
0x2378260 (IDs [125392]): 3 direct calls
   from 0x2375de2 in 0x2375b90 (IDs [125360])
   from 0x238e19a in 0x238e024 (IDs [])
   from 0x238e985 in 0x238e80b (IDs [])
```

- `IDs []` means the database has no ID for that function start.
- A call through a vtable or a function pointer isn't direct, so it doesn't show; `vtables.py` finds those.
- A tail call (a `jmp` to the function) doesn't show either.

**Which code uses a string:** the exact text, as a whole 8-bit string (not part of a longer one, not UTF-16). For each
copy in the exe, it gives every `rip`-relative reference, with the code around it: 48 bytes before and 96 after, or
your own counts after the text. Up to 12 references are shown per copy:

```
python tools\re\xref.py str SpringBone 16 16
string at RVA 0x4b512d0: 1 references
--- reference at 0x8771d5
   0x08771c5  mov    dword ptr [rip + 0x59a1671], 0
   0x08771cf  xor    r8d, r8d
   0x08771d2  lea    rdx, [rip + 0x42da0f7]  <==
```

A string the code only reaches through a table shows 0 references. Text that isn't in the exe as a whole string prints
nothing.

## sfexe.py

The readers the other tools share. A script saved in `tools\re\` can import it:

```python
import sys

import sfexe

exe = sfexe.Exe()                       # Starfield.exe, mapped read-only
al = sfexe.AddressLibrary()             # the database for that exe's version
start = al[int(sys.argv[1])]            # an ID's RVA
end = exe.end_of(start)                 # the function's end, from .pdata
for ins in sfexe.disasm(exe, sfexe.capstone_cs(), start, end):
    if ins["m"] == "call" and ins["call"] is not None:
        print(f"{ins['a']:#x} calls {ins['call']:#x}, IDs {al.ids_of(ins['call'])}")
```

Saved as `tools\re\calls_in.py`, `python tools\re\calls_in.py 125392` lists the 14 direct calls in the keyframe
function.

| Name | What it gives |
|---|---|
| `game_dir()` | The game folder: `STARFIELD_DIR`, or the first of the usual Steam folders that has `Starfield.exe`. |
| `exe_version()` | `Starfield.exe`'s version from its version resource, such as `(1, 16, 244, 0)`. |
| `Exe()` | The exe, mapped read-only. `secs` holds each section as (name, RVA, virtual size, file offset, file size). `off(rva)` and `rva(offset)` convert, `section_of(rva)` names a section, `u64(rva)` reads 8 bytes. `start_of(rva)` and `end_of(rva)` give a function's bounds from `.pdata`. `vtables(cls)`, `vslot(vtable, slot)` and `rtti_name(vtable)` read vtables. |
| `AddressLibrary()` | The database, format 5. `al[id]` is an RVA (0 = no address), `al.ids_of(rva)` lists the IDs at an RVA, and `al.version` is the game version. Pass `path=` for another file, or `version="1.16.244"` for another installed version. |
| `capstone_cs()` | A capstone x86-64 disassembler. |
| `disasm(exe, cs, start, end)` | The instructions as dicts: `a` address, `m` mnemonic, `o` operands, `c` the four floats at an `.rdata` operand (or `None`), `call` a direct call's or `jmp`'s target (or `None`). |

## An example: the keyframe function the world collision uses

```
python tools\re\disasm.py id:125392
python tools\re\vtables.py slots id:471048 6 14 26
```
