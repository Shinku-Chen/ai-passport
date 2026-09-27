#!/usr/bin/env python3
"""Ad-hoc check: every character of a *displayed* UI string must exist in the
generated LVGL font subset, otherwise the widget renders a missing-glyph box.

A literal counts as displayed UI text when it contains at least one CJK
character; pure format/log/`#include` literals are skipped on purpose. The C
source is UTF-8, so `\\xHH` escapes are single bytes and can be decoded
byte-wise before the UTF-8 decode.
"""
import io
import re
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

BS = chr(92)
CJK = re.compile("[\u3400-\u4dbf\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]")


def strip_comments(src):
    src = re.sub(r"/[*].*?[*]/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def decode_literal(lit):
    """Undo the escapes a C compiler would apply, keeping UTF-8 bytes intact."""
    out = bytearray()
    i = 0
    while i < len(lit):
        ch = lit[i]
        if ch == BS and i + 1 < len(lit):
            nxt = lit[i + 1]
            if nxt == "x":
                j = i + 2
                hexs = ""
                while j < len(lit) and lit[j] in "0123456789abcdefABCDEF" and len(hexs) < 2:
                    hexs += lit[j]
                    j += 1
                out.append(int(hexs, 16))
                i = j
                continue
            out.extend({"n": b"\n", "r": b"\r", "t": b"\t"}.get(nxt, nxt.encode("utf-8")))
            i += 2
            continue
        out.extend(ch.encode("utf-8"))
        i += 1
    return out.decode("utf-8", errors="replace")


def strip_logs(src):
    """Blank out ESP_LOGx(...) calls: those strings go to the USB console, not to LVGL."""
    out = list(src)
    i = 0
    while True:
        start = src.find("ESP_LOG", i)
        if start < 0:
            break
        j = src.find("(", start)
        if j < 0:
            break
        depth = 0
        in_str = False
        k = j
        while k < len(src):
            ch = src[k]
            if in_str:
                if ch == BS:
                    k += 2
                    continue
                if ch == '"':
                    in_str = False
            elif ch == '"':
                in_str = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        for m in range(start, min(k + 1, len(out))):
            if out[m] != "\n":
                out[m] = " "
        i = k + 1
    return "".join(out)


def literals(src):
    # C string literal: " ... " with backslash escapes; double the backslash so the
    # regex sees a real escaped backslash inside the character class.
    backslash = BS + BS
    pattern = re.compile('"((?:[^"' + backslash + ']|' + backslash + '.)*)"', re.S)
    return [decode_literal(x) for x in pattern.findall(src)]


def main():
    syms = set(open("assets/fonts/tsxx_symbols.txt", encoding="utf-8").read())
    for junk in "\n\r ":
        syms.discard(junk)

    bad = []
    checked = 0
    for path in ("main/tsxx_app.c", "main/tsxx_ui.c", "main/main.c"):
        src = strip_comments(open(path, encoding="utf-8").read())
        src = strip_logs(src)
        line_no = 0
        for raw_line in src.split("\n"):
            line_no += 1
            for text in literals(raw_line):
                if not CJK.search(text):
                    continue
                checked += 1
                miss = sorted({ch for ch in text
                               if ch not in syms and ch not in "\n\r\t "})
                if miss:
                    bad.append((path, line_no, "".join(miss), text))
    print("checked %d displayed UI literals" % checked)
    if not bad:
        print("OK: every displayed UI character is in the font subset")
        return 0
    for path, line_no, miss, text in bad:
        print("MISSING %s:%d %r in %r" % (path, line_no, miss, text[:44]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
