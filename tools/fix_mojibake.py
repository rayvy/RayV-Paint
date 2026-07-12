#!/usr/bin/env python3
"""Fix classic UTF-8 mis-decoded-as-CP1251 mojibake in source files.

UTF-8 special punctuation (—, →, …, ·, °, …) was misread as Windows-1251
and re-saved as UTF-8, producing Cyrillic+junk clusters like 'вЂ\"', 'в†’'.
ImGui then draws garbage glyphs in UI labels.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

# Build replacements from Unicode code points (source-file encoding-safe).
# Order: longest first.
def _s(*cps: int) -> str:
    return "".join(chr(c) for c in cps)

REPLACEMENTS: list[tuple[str, str]] = [
    # multi-char sequences first
    (_s(0x0432, 0x0402, 0x201D), "—"),   # em dash  E2 80 94
    (_s(0x0432, 0x0402, 0x201C), "–"),   # en dash  E2 80 93
    (_s(0x0432, 0x0402, 0x00A6), "…"),   # ellipsis E2 80 A6
    (_s(0x0432, 0x0402, 0x00A2), "•"),   # bullet   E2 80 A2  (if present)
    (_s(0x0432, 0x2020, 0x2019), "→"),   # right arrow E2 86 92
    (_s(0x0432, 0x2020, 0x2018), "←"),   # left arrow  E2 86 90
    (_s(0x0432, 0x2020, 0x201D), "↔"),   # lr arrow    E2 86 94
    (_s(0x0432, 0x2020, 0x2018), "←"),
    (_s(0x0432, 0x2020, 0x2019), "→"),
    (_s(0x0432, 0x2030, 0x00A0), "≠"),   # not equal   E2 89 A0
    (_s(0x0432, 0x2030, 0x20AC), "≈"),   # approx      E2 89 88
    (_s(0x0432, 0x2030, 0x00A4), "≤"),   # le          E2 89 A4
    (_s(0x0432, 0x2030, 0x00A5), "≥"),   # ge          E2 89 A5
    # Flip / cancel icons (ASCII-safe UI — glyphs often missing in default font)
    (_s(0x0432, 0x2021, 0x201E), "H"),   # was U+21C4 ⇄ flip-H (E2 87 84)
    (_s(0x0432, 0x2021, 0x2026), "V"),   # was U+21C5 ⇅ flip-V (E2 87 85)
    (_s(0x0432, 0x045A, 0x2022), "X"),   # was U+2715 ✕ cancel  (E2 9C 95)
    (_s(0x0412, 0x00B7), "·"),           # middle dot C2 B7
    (_s(0x0412, 0x00B0), "°"),           # degree     C2 B0
    (_s(0x0412, 0x00AB), "«"),           # guillemet  C2 AB
    (_s(0x0412, 0x00BB), "»"),
    (_s(0x0413, 0x2014), "×"),           # multiply   C3 97
    (_s(0x0413, 0x00B7), "÷"),           # divide     C3 B7
]


def fix_text(text: str) -> tuple[str, int]:
    n = 0
    for bad, good in REPLACEMENTS:
        if bad in text:
            c = text.count(bad)
            text = text.replace(bad, good)
            n += c
    return text, n


def remaining_cyrillic_clusters(text: str) -> list[tuple[str, int]]:
    """Report leftover non-ascii that still look like mojibake (for review)."""
    seen: dict[str, int] = {}
    for m in re.finditer(r"[^\x00-\x7F]+", text):
        s = m.group()
        # Skip pure punctuation we intentionally keep
        if all(ord(c) in {
            0x2014, 0x2013, 0x2026, 0x00B7, 0x00B0, 0x2192, 0x2190, 0x2194,
            0x2260, 0x2248, 0x00D7, 0x00AB, 0x00BB, 0x2022, 0x2713, 0x21D2, 0x21D4,
            0x2264, 0x2265, 0x00F7,
        } or (0x0400 <= ord(c) <= 0x04FF) for c in s):
            # still flag if contains Cyrillic letters mixed with symbols
            if any(0x0400 <= ord(c) <= 0x04FF for c in s):
                seen[s] = seen.get(s, 0) + 1
        else:
            # mixed junk
            if any(0x0400 <= ord(c) <= 0x04FF for c in s) or any(ord(c) > 0x00FF for c in s):
                seen[s] = seen.get(s, 0) + 1
    return sorted(seen.items(), key=lambda x: -x[1])


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    targets = list((root / "src").rglob("*.cpp")) + list((root / "src").rglob("*.h"))
    total = 0
    changed_files = 0
    for path in sorted(targets):
        raw = path.read_bytes()
        bom = raw.startswith(b"\xef\xbb\xbf")
        data = raw[3:] if bom else raw
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            print(f"SKIP (not utf-8): {path.relative_to(root)}")
            continue
        fixed, n = fix_text(text)
        if n == 0:
            continue
        out = fixed.encode("utf-8")
        if bom:
            out = b"\xef\xbb\xbf" + out
        path.write_bytes(out)
        print(f"{n:4d}  {path.relative_to(root)}")
        total += n
        changed_files += 1

        left = remaining_cyrillic_clusters(fixed)
        if left:
            print(f"      leftover clusters:")
            for s, c in left[:12]:
                cps = " ".join(f"U+{ord(ch):04X}" for ch in s)
                print(f"        {c:3d}  {s!r}  {cps}")

    print(f"\nDone: {total} replacements in {changed_files} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
