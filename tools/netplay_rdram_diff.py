#!/usr/bin/env python3
# Compares two dumps of RDRAM that netplay wrote (MP_NETPLAY_DUMP_FRAME, src/netplay/core/netplay_core.cpp) and names each
# stretch of words that differs by the symbol it lies in (syms/), with both runs' words, so the place a replay went
# its own way can be read off.
#
# Usage: tools/netplay_rdram_diff.py <a.rdram> <b.rdram> [max stretches, default 80]

import bisect
import pathlib
import re
import sys

repo = pathlib.Path(__file__).resolve().parent.parent
syms = []
for name in ("marioparty.us.datasyms.toml", "marioparty.us.syms.toml"):
    text = (repo / "syms" / name).read_text()
    for m in re.finditer(r'name = "([^"]+)", vram = (0x[0-9A-Fa-f]+)', text):
        syms.append((int(m.group(2), 16), m.group(1)))
syms.sort()
starts = [s[0] for s in syms]


def symbol(addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0:
        return "?"
    return f"{syms[i][1]}+0x{addr - syms[i][0]:X}"


a = pathlib.Path(sys.argv[1]).read_bytes()
b = pathlib.Path(sys.argv[2]).read_bytes()
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 80

stretches = []
for off in range(0, min(len(a), len(b)), 4):
    if a[off:off + 4] != b[off:off + 4]:
        if stretches and off - stretches[-1][1] <= 16:
            stretches[-1][1] = off
        else:
            stretches.append([off, off])

print(f"{len(stretches)} stretches differ")
for start, end in stretches[:limit]:
    addr = 0x80000000 + start
    words = (end - start) // 4 + 1
    print(f"{addr:08X} {words:5d} words  {symbol(addr):40s} {a[start:start + 8].hex()} | {b[start:start + 8].hex()}")
