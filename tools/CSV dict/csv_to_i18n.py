
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CSV → i18n C++ generator with MERGE MODE and always-on dialogs.
Generates:
  - i18n_gen_export.h
  - i18n_gen.cpp
Optionally:
  - i18n_fallback.cpp   (--emit-fallback)

CSV format:
key,<lang1>,<lang2>,...
touch_to_wake,Touch to wake,Dodirni za povratak,Zum Aufwecken berühren,Touchez pour réactiver,Toque para volver

MERGE MODE:
  --merge                   Merge with existing i18n_gen_export.h / i18n_gen.cpp in output folder
  --prefer {csv|existing}   Conflict policy for non-empty cells (default: csv)
  --overwrite-all           Overwrite existing even when CSV cell is empty (danger)
  --drop-orphans            Remove keys present only in existing table
  --dry-run                 Do not write files; print summary of what would change
"""

import argparse
import csv
import datetime as dt
import os
import re
import sys
from typing import List, Tuple, Dict, Any, Optional

# ─────────────────────────────────────────────────────────────────────────────
# Optional GUI (tkinter)
# ─────────────────────────────────────────────────────────────────────────────
def _try_import_tk():
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox
        return tk, filedialog, messagebox
    except Exception:
        return None, None, None

TK, FILEDIALOG, MSGBOX = _try_import_tk()

# Start in the folder where you launched the script
START_DIR = os.getcwd()

def browse_open_csv(title="Select translations CSV", initial=None):
    if TK is None:
        return None
    root = TK.Tk()
    root.withdraw()
    root.update_idletasks()
    path = FILEDIALOG.askopenfilename(
        title=title,
        initialdir=initial or START_DIR,
        filetypes=[("CSV", "*.csv"), ("All files", "*.*")]
    )
    root.destroy()
    return path or None

def browse_pick_folder(title="Select output folder", initial=None):
    if TK is None:
        return None
    root = TK.Tk()
    root.withdraw()
    root.update_idletasks()
    path = FILEDIALOG.askdirectory(
        title=title,
        initialdir=initial or START_DIR
    )
    root.destroy()
    return path or None

# ─────────────────────────────────────────────────────────────────────────────
# Utilities
# ─────────────────────────────────────────────────────────────────────────────
def ts() -> str:
    return dt.datetime.now().strftime("%Y%m%d_%H%M%S")

def backup_if_exists(path: str):
    if os.path.isfile(path):
        os.rename(path, f"{path}.{ts()}.bak")

def read_csv_rows(csv_path: str) -> List[List[str]]:
    # Accept UTF‑8 and UTF‑8 with BOM
    with open(csv_path, "r", encoding="utf-8-sig", newline="") as f:
        rdr = csv.reader(f)
        rows = [row for row in rdr]
    # Normalize whitespace; drop fully empty rows
    return [[c.strip() for c in r] for r in rows if any(c.strip() for c in r)]

_LANG_SANITIZE = re.compile(r"[^A-Za-z0-9_]")

def sanitize_lang_name(s: str) -> str:
    """Convert CSV header into a valid C identifier for struct fields."""
    s0 = s.strip().replace("-", "_").replace(" ", "_")
    s0 = _LANG_SANITIZE.sub("_", s0)
    if s0 and s0[0].isdigit():
        s0 = "_" + s0
    return s0 or "unnamed"

def cxx_escape(s: str) -> str:
    """Escape for C++ string literal (UTF‑8)."""
    if s is None:
        return ""
    s = s.replace("\\", "\\\\").replace('"', '\\"')
    s = s.replace("\r\n", "\n").replace("\r", "\n").replace("\t", "\\t")
    s = s.replace("\n", "\\n")
    return s

# ─────────────────────────────────────────────────────────────────────────────
# CSV validation and in-memory model
# ─────────────────────────────────────────────────────────────────────────────
def validate_and_build(rows: List[List[str]], sort_keys: bool) -> Tuple[List[str], List[Dict[str, Any]], Dict[str, Any]]:
    if not rows:
        raise SystemExit("CSV is empty.")

    header = rows[0]
    
    # Remove BOM if present
    header[0] = header[0].lstrip("\ufeff")

    if not header:
        raise SystemExit("CSV header is empty.")

    # First column must be 'key'
    if header[0].strip().lower() != "key":
        raise SystemExit("First column must be 'key' (case-insensitive).")

    raw_langs = header[1:]
    if not raw_langs:
        raise SystemExit("No language columns found (need at least one after 'key').")

    # Sanitize language names; ensure unique field names
    langs: List[str] = []
    lang_map: Dict[str, str] = {}
    seen: set = set()
    for name in raw_langs:
        cname = sanitize_lang_name(name)
        base = cname
        i = 1
        while cname in seen:
            i += 1
            cname = f"{base}_{i}"
        seen.add(cname)
        langs.append(cname)
        lang_map[name] = cname

    # Build data
    data: List[Dict[str, Any]] = []
    key_seen: set = set()
    dup_keys: List[str] = []
    empty_count = {ln: 0 for ln in langs}

    for r in rows[1:]:
        if not r or not any(r):
            continue
        # pad short rows
        if len(r) < (1 + len(raw_langs)):
            r = r + [""] * ((1 + len(raw_langs)) - len(r))

        key = r[0].strip()
        if not key or key.startswith("#") or key.startswith("//"):
            continue
        if key in key_seen:
            dup_keys.append(key)
        key_seen.add(key)

        item = {"key": key}
        for i, original in enumerate(raw_langs, start=1):
            cname = lang_map[original]
            val = r[i].strip() if i < len(r) else ""
            if not val:
                empty_count[cname] += 1
            item[cname] = val
        data.append(item)

    if sort_keys:
        data.sort(key=lambda d: d["key"])

    stats = {
        "rows": len(data),
        "langs": langs,
        "empty_per_lang": empty_count,
        "duplicates": sorted(set(dup_keys)),
        "sorted": sort_keys,
    }
    if dup_keys:
        print(f"WARNING: duplicate keys ({len(stats['duplicates'])}): "
              f"{stats['duplicates'][:10]}{' ...' if len(stats['duplicates'])>10 else ''}")
    return langs, data, stats

# ─────────────────────────────────────────────────────────────────────────────
# Existing (generated) header/cpp parsers for MERGE mode
# ─────────────────────────────────────────────────────────────────────────────
def parse_existing_header(h_path: str) -> Optional[List[str]]:
    """
    Parse i18n_gen_export.h to extract language field names.
    Looks for lines: 'const char* <lang>;' inside struct Entry.
    """
    if not os.path.isfile(h_path):
        return None
    txt = open(h_path, "r", encoding="utf-8").read()
    m = re.search(r"struct\s+Entry\s*\{(.*?)\};", txt, re.S)
    if not m:
        return None
    body = m.group(1)
    langs = re.findall(r"const\s+char\*\s+([A-Za-z0-9_]+)\s*;", body)
    # First field is 'key'; discard it if present
    langs = [ln for ln in langs if ln != "key"]
    return langs or None

def parse_existing_table(cpp_path: str, langs: List[str]) -> Dict[str, Dict[str, str]]:
    """
    Parse i18n_gen.cpp 'const Entry D[] = { ... };' using the known language list.
    Returns: { key: { lang: value } }
    """
    if not os.path.isfile(cpp_path):
        return {}
    buf = open(cpp_path, "r", encoding="utf-8").read()
    m = re.search(r"const\s+Entry\s+D\s*\[\s*\]\s*=\s*\{(.*?)\};", buf, re.S)
    if not m:
        return {}
    block = m.group(1)
    rows = re.findall(r"\{(.*?)\}", block, re.S)
    result: Dict[str, Dict[str, str]] = {}
    for r in rows:
        # Extract quoted fields with escape handling
        fields = re.findall(r'"((?:\\.|[^"\\])*)"', r)
        if not fields:
            continue
        key = fields[0]
        vals = fields[1:]
        entry: Dict[str, str] = {}
        for i, ln in enumerate(langs):
            entry[ln] = vals[i] if i < len(vals) else ""
        result[key] = entry
    return result

# ─────────────────────────────────────────────────────────────────────────────
# Merge engine
# ─────────────────────────────────────────────────────────────────────────────
def merge_models(
    csv_langs: List[str],
    csv_data: List[Dict[str, Any]],
    existing_langs: List[str],
    existing_data: Dict[str, Dict[str, str]],
    prefer: str = "csv",
    overwrite_all: bool = False,
    drop_orphans: bool = False,
    preserve_order_keys: List[str] = None,
) -> Tuple[List[str], List[Dict[str, Any]], Dict[str, int]]:
    """
    Returns merged_langs, merged_rows(list of dict with 'key' + langs), and counters.
    """
    # Union of languages: CSV first, then extras from existing
    merged_langs: List[str] = list(csv_langs)
    for ln in existing_langs:
        if ln not in merged_langs:
            merged_langs.append(ln)

    # Quick lookup for CSV rows
    csv_map: Dict[str, Dict[str, str]] = {}
    for row in csv_data:
        csv_map[row["key"]] = {ln: row.get(ln, "") for ln in merged_langs}

    # Counters
    added, updated, unchanged, orphan_kept, orphan_dropped = 0, 0, 0, 0, 0

    # Desired order: CSV keys first (preserve order), then orphans (if kept)
    keys_in_order: List[str] = preserve_order_keys[:] if preserve_order_keys else [r["key"] for r in csv_data]
    for k in existing_data.keys():
        if k not in csv_map:
            if drop_orphans:
                orphan_dropped += 1
                continue
            orphan_kept += 1
            keys_in_order.append(k)

    # Build merged rows
    merged_rows: List[Dict[str, Any]] = []
    for k in keys_in_order:
        merged_entry: Dict[str, Any] = {"key": k}
        src_csv = csv_map.get(k)
        src_ex  = existing_data.get(k)

        if src_csv and not src_ex:
            # New key from CSV
            for ln in merged_langs:
                merged_entry[ln] = src_csv.get(ln, "")
            added += 1

        elif src_ex and not src_csv:
            # Orphan from existing
            for ln in merged_langs:
                merged_entry[ln] = src_ex.get(ln, "")
            # orphan counters handled above

        else:
            # Present in both → merge per policy
            changed = False
            for ln in merged_langs:
                csv_val = (src_csv or {}).get(ln, "")
                ex_val  = (src_ex or {}).get(ln, "")
                if overwrite_all:
                    val = csv_val if prefer == "csv" else ex_val
                elif prefer == "csv":
                    val = csv_val if csv_val != "" else ex_val
                else:  # prefer existing
                    val = ex_val if ex_val != "" else csv_val
                if val != ex_val:
                    changed = True
                merged_entry[ln] = val
            if changed:
                updated += 1
            else:
                unchanged += 1

        merged_rows.append(merged_entry)

    counters = {
        "added": added,
        "updated": updated,
        "unchanged": unchanged,
        "orphan_kept": orphan_kept,
        "orphan_dropped": orphan_dropped,
    }
    return merged_langs, merged_rows, counters

# ─────────────────────────────────────────────────────────────────────────────
# Code generators
# ─────────────────────────────────────────────────────────────────────────────
def generate_header(langs: List[str]) -> str:
    fields = "\n".join([f"    const char* {ln};" for ln in langs])
    return f"""// Auto-generated from CSV — DO NOT EDIT MANUALLY
#pragma once
#include <stddef.h>

struct Entry {{
    const char* key;
{fields}
}};

extern "C" {{
    extern const Entry* g_i18n_gen_table;
    extern const size_t g_i18n_gen_count;
}}
"""

def generate_cpp(langs: List[str], data: List[Dict[str, Any]]) -> str:
    lines = []
    lines.append('// Auto-generated from CSV — DO NOT EDIT MANUALLY')
    lines.append('#include "i18n.h"')
    lines.append('#include "i18n_gen_export.h"\n')
    lines.append("const Entry D[] = {")
    for row in data:
        key = cxx_escape(row["key"])
        values = [cxx_escape(row.get(ln, "")) for ln in langs]
        joined = '", "'.join(values)
        lines.append(f'    {{ "{key}", "{joined}" }},')
    lines.append("};\n")
    lines.append('extern "C" {')
    lines.append("    const Entry* g_i18n_gen_table = D;")
    lines.append("    const size_t g_i18n_gen_count = sizeof(D) / sizeof(D[0]);")
    lines.append("}")
    return "\n".join(lines) + "\n"

def generate_fallback_cpp(langs: List[str], data: List[Dict[str, Any]]) -> str:
    lines = []
    lines.append('// Auto-generated fallback table — optional')
    lines.append('#include "i18n.h"')
    lines.append("// This file can be used instead of the generated table if desired.\n")
    lines.append("struct Entry {")
    lines.append("    const char* key;")
    for ln in langs:
        lines.append(f"    const char* {ln};")
    lines.append("};\n")
    lines.append("static const Entry D_builtin[] = {")
    for row in data:
        key = cxx_escape(row["key"])
        values = [cxx_escape(row.get(ln, "")) for ln in langs]
        joined = '", "'.join(values)
        lines.append(f'    {{ "{key}", "{joined}" }},')
    lines.append("};\n")
    lines.append("// Implement your own tr() to search D_builtin if you want a built-in table.")
    return "\n".join(lines) + "\n"

def write_text_file(path: str, content: str):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    backup_if_exists(path)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)

def summarize(stats: Dict[str, Any], counters: Dict[str, int] = None):
    print("\n=== Summary ===")
    print(f"Rows        : {stats['rows']}")
    print(f"Languages   : {', '.join(stats['langs'])}")
    print(f"Sorted      : {'yes' if stats['sorted'] else 'no'}")
    empties = stats["empty_per_lang"]
    if any(empties.values()):
        print("Empty cells :")
        for ln, cnt in empties.items():
            if cnt:
                print(f"  - {ln}: {cnt}")
    if stats["duplicates"]:
        print(f"Duplicates  : {len(stats['duplicates'])} → {stats['duplicates'][:10]}{' ...' if len(stats['duplicates'])>10 else ''}")
    if counters:
        print("Merge result:")
        for k, v in counters.items():
            print(f"  {k:14s}: {v}")
    print()

# ─────────────────────────────────────────────────────────────────────────────
# CLI / GUI orchestrator
# ─────────────────────────────────────────────────────────────────────────────
def main():
    p = argparse.ArgumentParser(
        description="CSV → i18n generator with MERGE MODE (creates i18n_gen_export.h, i18n_gen.cpp)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    p.add_argument("--csv", help="Path to translations.csv (ignored unless --nogui)")
    p.add_argument("--out", help="Output folder (ignored unless --nogui)", default=".")
    p.add_argument("--sort-keys", action="store_true", help="Sort rows alphabetically by key")
    p.add_argument("--emit-fallback", action="store_true", help="Also emit optional i18n_fallback.cpp")
    p.add_argument("--nogui", action="store_true", help="Disable dialogs and use CLI args / console prompts")
    # MERGE flags
    p.add_argument("--merge", action="store_true", help="Merge with existing i18n_gen_export.h / i18n_gen.cpp found in output folder")
    p.add_argument("--prefer", choices=["csv", "existing"], default="csv", help="Conflict policy when both CSV and existing have values")
    p.add_argument("--overwrite-all", action="store_true", help="Overwrite existing values even when CSV cell is empty (dangerous)")
    p.add_argument("--drop-orphans", action="store_true", help="Drop keys that exist only in existing table")
    p.add_argument("--dry-run", action="store_true", help="Do not write files; only print summary")
    args = p.parse_args()

    # Always-open dialogs unless --nogui or tkinter not available
    if not args.nogui and TK is not None:
        csv_path = browse_open_csv(initial=START_DIR)
        if not csv_path:
            print("No CSV selected; abort.")
            sys.exit(1)
        out_dir = browse_pick_folder(initial=START_DIR)
        if not out_dir:
            print("No output folder selected; abort.")
            sys.exit(1)
    else:
        csv_path = args.csv
        if not csv_path:
            print("No GUI and no --csv provided.")
            csv_path = input("Enter CSV path: ").strip('"').strip()
            if not csv_path:
                sys.exit("No CSV provided. Abort.")
        if not os.path.isfile(csv_path):
            sys.exit(f"CSV not found: {csv_path}")
        out_dir = args.out or "."
        if out_dir == ".":
            ans = input("Use current folder for output? [Y/n]: ").strip().lower() or "y"
            if ans not in ("y", "yes"):
                out_dir = input("Enter output folder: ").strip('"').strip()
                if not out_dir:
                    sys.exit("No output folder provided. Abort.")

    os.makedirs(out_dir, exist_ok=True)

    # Read & validate CSV
    rows = read_csv_rows(csv_path)
    csv_langs, csv_data, stats = validate_and_build(rows, sort_keys=args.sort_keys)

    counters = None
    if args.merge:
        h_path   = os.path.join(out_dir, "i18n_gen_export.h")
        cpp_path = os.path.join(out_dir, "i18n_gen.cpp")
        if not os.path.isfile(h_path) or not os.path.isfile(cpp_path):
            print(f"MERGE requested, but missing existing files in: {out_dir}")
            print("Expecting i18n_gen_export.h and i18n_gen.cpp. Proceeding without merge.")
        else:
            existing_langs = parse_existing_header(h_path) or []
            existing_data  = parse_existing_table(cpp_path, existing_langs)
            preserve_order = [r["key"] for r in csv_data]
            merged_langs, merged_rows, counters = merge_models(
                csv_langs=csv_langs,
                csv_data=csv_data,
                existing_langs=existing_langs,
                existing_data=existing_data,
                prefer=args.prefer,
                overwrite_all=args.overwrite_all,
                drop_orphans=args.drop_orphans,
                preserve_order_keys=preserve_order
            )
            csv_langs, csv_data = merged_langs, merged_rows

    # Generate files (or just summarize)
    h_text  = generate_header(csv_langs)
    cpp_text = generate_cpp(csv_langs, csv_data)

    summarize(stats, counters)

    if args.dry_run:
        print("Dry-run: no files written.")
        return

    h_path   = os.path.join(out_dir, "i18n_gen_export.h")
    cpp_path = os.path.join(out_dir, "i18n_gen.cpp")
    write_text_file(h_path, h_text)
    write_text_file(cpp_path, cpp_text)

    if args.emit_fallback:
        fb_text = generate_fallback_cpp(csv_langs, csv_data)
        fb_path = os.path.join(out_dir, "i18n_fallback.cpp")
        write_text_file(fb_path, fb_text)

    print("Generated files:")
    print(f"  ✔ {h_path}")
    print(f"  ✔ {cpp_path}")
    if args.emit_fallback:
        print(f"  ✔ {fb_path}")
    print("\nRebuild your firmware to take effect.")

if __name__ == "__main__":
    main()

