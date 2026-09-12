# pescan — offline PE analysis for this project

Built for `PLAN-CURRENT.md` P0 and kept because this repo does most of its
work by reading `client.dll`, `engine.dll` and `server.dll` on disk, and this
machine has **no Python, no objdump, no radare, no IDA** — only MSVC. Every
prior session that decoded a struct or found a hook target did it by hand.
This is that work, made repeatable.

It reads a PE from disk. It never touches a running process and never needs
the game launched.

## Build

```bash
cl /nologo /std:c++20 /EHsc /O2 pescan.cpp /Fe:pescan.exe
```

from a `vcvars64` shell. Nothing else is required.

## Trust it, but check it first

`pescan selftest <pe>` sweeps **every** `.pdata` function from `begin` to
`end`. A decoder that mis-sizes an instruction desynchronises and either hits
an undecodable byte or overshoots the function end, so both are counted. On
this build's `client.dll`:

```
functions 46493  instructions 1738832  undecodable 434  end-mismatch 25  (0.99% bad)
```

The residue is jump tables embedded in `.text`, not mis-sized instructions.
The decoder marks what it cannot decode (`<== UNDECODED`) rather than guessing,
which is the property that matters — a silent desync would invent addresses.

## Commands

All addresses are RVAs in hex.

| command | what it answers |
| --- | --- |
| `sections <pe>` | image base, sections, `.pdata` entry count |
| `pdata <pe> <rva>` | the `.pdata` entry containing `rva`, and its chain |
| `extent <pe> <rva>` | the REAL function: root entry plus every fragment chaining to it |
| `fullfunc <pe> <rva>` | disassemble that whole extent |
| `dis <pe> <rva> [n]` | linear disassembly |
| `dump <pe> <rva> <len>` | hex dump |
| `xref <pe> <rva>` | rip-relative references to `rva`, with the owning function |
| `xrefrange <pe> <lo> <hi>` | the same for a whole range — one sweep for a whole vtable |
| `callers <pe> <rva>` | direct `E8`/`E9` references |
| `vtxref <pe> <rva>` | absolute-qword references — vtable slots and pointer globals |
| `vcall <pe> <disp>` | every `call [reg+disp]`, i.e. one vtable slot image-wide |
| `ffieldscan <pe> <fn> <disp>` | every memory operand at `disp` inside a function |
| `dispscan <pe> <disp>` | the same image-wide, stack bases excluded |
| `vec3scan <pe> <disp>` | float/int triples at `disp`/`+4`/`+8` off one base |
| `str` / `strxref <pe> <text>` | find an ASCII string, and what references it |

## The two rules it exists to enforce

**`.pdata`-check every address before hooking it.** `extent` resolves
`UNW_FLAG_CHAININFO` to the real function entry. `F1-PLACEMENT-WRITER-FOUND.md`
records what hooking a continuation fragment would have done: a `ret` against
whatever happened to be at `[rsp]`.

**Find the consumer before spending a run on a write.** `xref`, `vtxref`,
`vcall` and `ffieldscan` are how `P0-CMD-SEAM-2026-08-19.md` established that
`CUserCmd+0x28` is read in five places on the client and written in none — and
how it found `CInput::VerifyUserCmd` reverting any post-`CreateMove` edit
before it reaches the wire.

## Worked example — how the command seam was found

```bash
pescan xref     client.dll 11BE1F8    # +attack kbutton -> GetButtonBits 0x251620
pescan vtxref   client.dll 251620     # -> vtable slot 0x905230, base 0x905218 => +0x18
pescan vcall    client.dll 18         # -> the only CInput caller: 0x254FC3
pescan extent   client.dll 254D10     # -> CInput::CreateMove, one fragment, no chaining
pescan ffieldscan client.dll 254D10 C # -> movss [rdi+0xC/0x10/0x14] = worldViewAngles
```
