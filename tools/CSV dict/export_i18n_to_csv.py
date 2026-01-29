
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Export translations from i18n.cpp OR i18n_gen.cpp into CSV.

Supports both:
 - static const Entry D[] = { ... };
 - const Entry D[] = { ... };

Auto-detects number of languages.
"""

import csv
import os
import re
import sys

# ────────────────────────────────────────────────────────────
# Optional GUI
# ────────────────────────────────────────────────────────────
def _try_import_tk():
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox
        return tk, filedialog, messagebox
    except Exception:
        return None, None, None

TK, FILEDIALOG, MSGBOX = _try_import_tk()

def browse_open_file(title="Select i18n file", initial=None):
    if TK is None or FILEDIALOG is None:
        return None
    root = TK.Tk()
    root.withdraw()
    root.update_idletasks()
    path = FILEDIALOG.askopenfilename(
        title=title,
        initialdir=initial or os.getcwd(),
        filetypes=[
            ("C/C++ sources", "*.cpp;*.cc;*.cxx;*.c"),
            ("All files", "*.*")
        ]
    )
    root.destroy()
    return path or None

def browse_save_file(title="Save translations.csv", initial=None, default_name="translations.csv"):
    if TK is None or FILEDIALOG is None:
        return None
    root = TK.Tk()
    root.withdraw()
    root.update_idletasks()
    path = FILEDIALOG.asksaveasfilename(
        title=title,
        initialdir=initial or os.getcwd(),
        initialfile=default_name,
        defaultextension=".csv",
        filetypes=[("CSV", "*.csv"), ("All files", "*.*")]
    )
    root.destroy()
    return path or None

# ────────────────────────────────────────────────────────────
# Parsing engine
# ────────────────────────────────────────────────────────────

def parse_entries(buf: str):
    """
    Extract all { "...", "...", ... } rows from either:
      static const Entry D[] = { ... };
    or:
      const Entry D[] = { ... };
    """
    # 1. Find the initializer block
    block_match = re.search(
        r"(static\s+const\s+Entry\s+D\s*\[\s*\]\s*=\s*\{(.*?)\};)",
        buf, re.S)

    if not block_match:
        block_match = re.search(
            r"(const\s+Entry\s+D\s*\[\s*\]\s*=\s*\{(.*?)\};)",
            buf, re.S)

    if not block_match:
        raise RuntimeError(
            "Could not find the Entry D[] = { ... } table in this file."
        )

    block = block_match.group(2)

    # 2. Extract rows: { "key", "en", ... }
    row_pattern = re.compile(r"\{(.*?)\}", re.S)
    row_matches = row_pattern.findall(block)

    rows = []
    max_fields = 0

    for r in row_matches:
        # Extract fields inside quotes:
        # supports escaped content like \" or \\ or \n
        field_pattern = re.compile(r'"((?:\\.|[^"\\])*)"')
        fields = field_pattern.findall(r)

        if not fields:
            continue

        rows.append(fields)
        max_fields = max(max_fields, len(fields))

    if not rows:
        raise RuntimeError("Found D[] table but extracted no translation rows.")

    return rows, max_fields


def write_csv(rows, n_fields, out_path, bom=True):
    encoding = "utf-8-sig" if bom else "utf-8"
    with open(out_path, "w", encoding=encoding, newline="") as f:
        writer = csv.writer(f, quoting=csv.QUOTE_ALL)

        # Header: first column "key", then L1, L2, ... automatically
        header = ["key"] + [f"lang_{i}" for i in range(1, n_fields)]
        writer.writerow(header)

        for row in rows:
            writer.writerow(row)


# ────────────────────────────────────────────────────────────
# Main runner
# ────────────────────────────────────────────────────────────

def main():
    print("=== Export i18n.cpp or i18n_gen.cpp → CSV ===\n")

    # Guess likely file
    guesses = [
        "i18n.cpp",
        "i18n_gen.cpp",
        os.path.join("src", "intl", "i18n.cpp"),
        os.path.join("src", "intl", "i18n_gen.cpp")
    ]
    in_path = next((g for g in guesses if os.path.isfile(g)), None)

    print("Select your i18n file…")
    chosen = browse_open_file(title="Select i18n.cpp or i18n_gen.cpp",
                              initial=os.path.dirname(in_path) if in_path else None)
    if chosen:
        in_path = chosen

    if not in_path:
        print("No file selected.")
        sys.exit(1)

    # Read and parse
    buf = open(in_path, "r", encoding="utf-8").read()
    try:
        rows, n_fields = parse_entries(buf)
    except Exception as e:
        print(f"ERROR: {e}")
        sys.exit(2)

    print(f"Found {len(rows)} rows, each with {n_fields} fields.")

    # Where to save?
    default_out = os.path.join(os.path.dirname(os.path.abspath(in_path)),
                               "translations.csv")

    out_path = browse_save_file(
        title="Save translations.csv",
        initial=os.path.dirname(default_out),
        default_name=os.path.basename(default_out)
    )
    if not out_path:
        out_path = default_out
        print(f"No output chosen → using default: {out_path}")

    write_csv(rows, n_fields, out_path, bom=True)
    print(f"\nExport complete: {out_path}\n")


if __name__ == "__main__":
    main()
