#!/usr/bin/env python3
"""Checks res/lang/*.toml: key parity with English, "{}" parity, keys the code
uses that English lacks, keys nothing uses, and cvar rows with no tooltip."""
import re
import sys
import tomllib
from pathlib import Path

root = Path(__file__).resolve().parent.parent
langs = {p.stem: tomllib.loads(p.read_text(encoding="utf8")) for p in sorted((root / "res/lang").glob("*.toml"))}
en = langs["en"]
problems = 0


def report(msg):
    global problems
    problems += 1
    print(msg)


def flat(table):
    return {(sec, k): v for sec, body in table.items() if isinstance(body, dict) for k, v in body.items()}


en_flat = flat(en)
for code, table in langs.items():
    if code == "en":
        continue
    mine = flat(table)
    for key in sorted(en_flat.keys() - mine.keys()):
        report(f"{code}: missing [{key[0]}] {key[1]}")
    for key in sorted(mine.keys() - en_flat.keys()):
        report(f"{code}: not in English [{key[0]}] {key[1]}")
    for key in sorted(en_flat.keys() & mine.keys()):
        if en_flat[key].count("{}") != mine[key].count("{}"):
            report(f"{code}: [{key[0]}] {key[1]} has a different number of {{}}")
        if not mine[key].strip():
            report(f"{code}: [{key[0]}] {key[1]} is empty")
    if "name" not in table:
        report(f"{code}: no name")

# Keys the code asks for.
used = set()
sources = [p for p in (root / "src").rglob("*") if p.suffix in (".cpp", ".h")]
pat_tr = re.compile(r'\bTr(?:Or)?\(\s*"(\w+)"\s*,\s*"(\w+)"')
for path in sources:
    text = path.read_text(encoding="utf8", errors="replace")
    for sec, key in pat_tr.findall(text):
        used.add((sec, key))
    if path.name == "settings.cpp":
        for m in re.finditer(r'\bT\(\s*"(\w+)"\s*\)', text):
            used.add(("settings", m.group(1)))
        for m in re.finditer(r'Tip\(\s*"(\w+)"', text):
            used.add(("settings", m.group(1) + "_tip"))
        for m in re.finditer(r'Draw(?:Cvar|Multiplier|Sensitivity|GameVolume)Row\(\s*"(\w+)"', text):
            used.add(("settings", m.group(1)))
        # Rows that show a tooltip when one exists; all of them should have one.
        for m in re.finditer(r'Draw(?:Cvar|Sensitivity)Row\(\s*"(\w+)"', text):
            used.add(("settings", m.group(1) + "_tip"))
        used.add(("settings", "vulkan_device_tip"))
        for m in re.finditer(r'DrawRowLabel\(\s*"(\w+)"', text):
            used.add(("settings", m.group(1)))
        for m in re.finditer(r'DrawMultiplierRow\(\s*"(\w+)"', text):
            used.add(("settings", m.group(1) + "_tip"))
        for m in re.finditer(r'DrawGameVolumeRow\(\s*"(\w+)"', text):
            used.add(("settings", m.group(1)))
        for m in re.finditer(r'OptionText\(\s*"(\w+)"', text):
            prefix = m.group(1)
            for sec, key in en_flat:
                if sec == "settings" and key.startswith(prefix):
                    used.add((sec, key))
# Keys composed at run time.
used |= {("settings", "game_volume_tip")}
# IntroText keys live in a table in ui_text.cpp.
ui_text = (root / "src/installer/ui_text.cpp").read_text(encoding="utf8")
for block in re.findall(r"kKeys = \{(.*?)\};|kPhaseKeys = \{(.*?)\};|kProgressKeys = \{(.*?)\};", ui_text, re.S):
    for chunk in block:
        for key in re.findall(r'"(\w+)"', chunk):
            used.add(("intro", key))

for key in sorted(used - en_flat.keys()):
    sec, k = key
    # A missing tooltip is allowed: the row shows the cvar's description.
    if k.endswith("_tip"):
        report(f"en: no tooltip [{sec}] {k} (row falls back to the cvar description)")
    else:
        report(f"en: used but missing [{sec}] {k}")
for key in sorted(en_flat.keys() - used):
    report(f"en: unused [{key[0]}] {key[1]}")

print(f"{len(langs)} languages, {len(en_flat)} English keys, {problems} problem(s)")
sys.exit(1 if problems else 0)
