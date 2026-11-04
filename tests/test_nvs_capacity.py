#!/usr/bin/env python3
"""31-slot NVS capacity contract, using the real save encoder witness and
IDF 5.5.3's official nvs_partition_gen.py. Offline only: this is not a
physical commit/GC/power-loss test."""
import argparse
import csv
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
NVS_PAGE = 4096
NVS_ENTRIES = 126
NVS_ENTRY_SIZE = 32
NVS_TYPE_BLOB_INDEX = 0x48
DEFAULT_GEN = r"C:/Espressif/frameworks/esp-idf-v5.5.3/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
DEFAULT_PY = r"C:/Espressif/python_env/idf5.5_py3.14_env/Scripts/python.exe"


def fail(msg):
    print("FAIL:", msg)
    return 1


def parse_partitions(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [x.strip() for x in line.split(",")]
            if len(parts) < 5:
                continue
            name, typ, sub, off, size = parts[:5]
            rows.append((name, typ, sub, int(off, 0), int(size, 0)))
    return rows


def read_define(path, name):
    text = open(path, encoding="utf-8").read()
    m = re.search(r"^#define\s+%s\s+(\d+)" % re.escape(name), text, re.M)
    return int(m.group(1)) if m else None


def parse_witness(path):
    text = open(path, encoding="utf-8").read()
    ord_m = re.search(r"ordinary=(\d+)\s+ordinary_ok=(\d+)", text)
    max_m = re.search(r"max=(\d+)\s+max_ok=(\d+)", text)
    if not ord_m or not max_m:
        raise ValueError("witness output does not contain ordinary/max lines")
    return int(ord_m.group(1)), int(max_m.group(1)), int(ord_m.group(2)), int(max_m.group(2))


def write_blob(path, size):
    with open(path, "wb") as f:
        f.write(bytes((i * 37 + 11) & 0xFF for i in range(size)))


def make_csv(outdir, payload_size, slots):
    rows = [["key", "type", "encoding", "value"], ["manosaba", "namespace", "", ""]]
    for i in range(slots - 1):
        p = os.path.join(outdir, "s%02d.bin" % i)
        write_blob(p, payload_size)
        rows.append(["s%02d" % i, "file", "binary", p])
    p = os.path.join(outdir, "auto.bin")
    write_blob(p, payload_size)
    rows.append(["auto", "file", "binary", p])
    for key, size in (("unlock", 81), ("prog", 2), ("set", 8)):
        p = os.path.join(outdir, key + ".bin")
        write_blob(p, size)
        rows.append([key, "file", "binary", p])
    csv_path = os.path.join(outdir, "nvs.csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        csv.writer(f).writerows(rows)
    return csv_path


def parse_image(path):
    data = open(path, "rb").read()
    if len(data) % NVS_PAGE:
        raise ValueError("NVS image is not page aligned")
    pages = len(data) // NVS_PAGE
    used = 0
    free_pages = 0
    slots = {}
    other = []
    for pg in range(pages):
        base = pg * NVS_PAGE
        state = struct.unpack_from("<I", data, base)[0]
        table = data[base + 32:base + 64]
        entries = 0
        for i in range(NVS_ENTRIES):
            st = (table[i // 4] >> ((i % 4) * 2)) & 3
            if st != 2:
                continue
            entries += 1
            raw = data[base + 64 + i * NVS_ENTRY_SIZE:base + 64 + (i + 1) * NVS_ENTRY_SIZE]
            if len(raw) != NVS_ENTRY_SIZE:
                raise ValueError("short entry")
            typ = raw[1]
            key = raw[8:24].split(b"\0")[0].decode("latin1")
            if typ == NVS_TYPE_BLOB_INDEX:
                size = struct.unpack_from("<H", raw, 24)[0]
                slots[key] = size
            elif key and all(32 <= ord(ch) < 127 for ch in key):
                other.append(key)
        used += entries
        if state == 0xFFFFFFFF and entries == 0:
            free_pages += 1
    return pages, used, free_pages, slots, other


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--witness-log", required=True)
    ap.add_argument("--partitions", default=os.path.join(ROOT, "partitions.csv"))
    ap.add_argument("--save-header", default=os.path.join(ROOT, "main", "gal_save.h"))
    ap.add_argument("--gen", default=os.environ.get("NVS_PARTITION_GEN", DEFAULT_GEN))
    ap.add_argument("--idf-python", default=os.environ.get("IDF_PYTHON", DEFAULT_PY))
    args = ap.parse_args()

    ordinary, max_len, ordinary_ok, max_ok = parse_witness(args.witness_log)
    cap = read_define(args.save_header, "GAL_SAVE_MAX")
    if cap is None:
        return fail("GAL_SAVE_MAX missing")
    if not ordinary_ok or not max_ok:
        return fail("witness encoder failed (ordinary=%d max=%d)" % (ordinary, max_len))
    if max_len > cap:
        return fail("max witness %d > GAL_SAVE_MAX %d" % (max_len, cap))

    parts = {r[0]: r for r in parse_partitions(args.partitions)}
    need = {"nvs": (0x9000, 0x26000), "phy_init": (0x2f000, 0x1000),
            "factory": (0x30000, 0x7d0000)}
    for name, (off, size) in need.items():
        if name not in parts or parts[name][3] != off or parts[name][4] != size:
            return fail("partition %s expected off=0x%x size=0x%x, got %r" %
                        (name, off, size, parts.get(name)))
    if 0x30000 + 0x7d0000 > 0x800000:
        return fail("factory exceeds 8 MiB")

    outdir = tempfile.mkdtemp(prefix="nvs-capacity-", dir=os.path.join(ROOT, "build"))
    csv_path = make_csv(outdir, cap, 31)
    image = os.path.join(outdir, "nvs-0x26000.bin")
    cmd = [args.idf_python, args.gen, "generate", csv_path, image, "0x26000", "--version", "2"]
    print("NVS_GEN_CMD:", " ".join(cmd))
    run = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print("NVS_GEN_DIR:", outdir)
    if run.stdout:
        print(run.stdout[-2000:])
    if run.stderr:
        print(run.stderr[-2000:])
    if run.returncode:
        return fail("official nvs_partition_gen.py failed rc=%d" % run.returncode)

    pages, used, free_pages, slots, other = parse_image(image)
    expected_slots = {"s%02d" % i for i in range(30)} | {"auto"}
    expected_meta = {"unlock", "prog", "set"}
    if pages != 38:
        return fail("page count %d != 38" % pages)
    if set(slots) != expected_slots | expected_meta:
        return fail("blob indexes %r != expected %r" % (sorted(slots),
                    sorted(expected_slots | expected_meta)))
    bad = {k: v for k, v in slots.items() if k in expected_slots and v != cap}
    if bad:
        return fail("slot payload sizes differ: %r" % bad)
    for k, want in (("unlock", 81), ("prog", 2), ("set", 8)):
        if slots.get(k) != want:
            return fail("%s payload %r != %d" % (k, slots.get(k), want))
    if free_pages < 1:
        return fail("no free/GC page (free_pages=%d, used_entries=%d)" % (free_pages, used))
    # One replacement of a max-size slot needs a chunk/index plus data entries.
    update_entries = (cap + NVS_ENTRY_SIZE - 1) // NVS_ENTRY_SIZE + 2
    if pages * NVS_ENTRIES - used < update_entries:
        return fail("free entries %d < one-update requirement %d" %
                    (pages * NVS_ENTRIES - used, update_entries))
    if used > pages * NVS_ENTRIES:
        return fail("entry overflow")
    print("WITNESS ordinary=%d max=%d cap=%d slots=%d" % (ordinary, max_len, cap, len(slots)))
    print("NVS pages=%d entries=%d used=%d free_entries=%d free_pages=%d other_keys=%r" %
          (pages, pages * NVS_ENTRIES, used, pages * NVS_ENTRIES - used, free_pages, sorted(set(other))))
    print("NVS_CAPACITY PASS upper_payload=%d generated=%s" % (cap, image))
    return 0


if __name__ == "__main__":
    sys.exit(main())
