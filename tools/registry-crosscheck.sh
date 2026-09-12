#!/usr/bin/env bash
# Cross-check the three tables that have to cover each other exactly.
#
#   1. every registry name exists as an ApplyValue key
#   2. registry and the menu presentation table cover each other
#
# Both differences must be EMPTY. A registry row whose key ApplyValue does not
# handle is a control that silently does nothing; a presentation row with no
# registry row behind it is a label with no setting. Run before every commit.
set -u
cd "$(dirname "$0")/.." || exit 1
SRC=plugin/src

# ApplyValue's if-chain: every std::strcmp(name, "key") in config.cpp.
sed -n "/^void ApplyValue/,/^}/p" "$SRC/core/config.cpp" \
  | grep -o 'std::strcmp(name, "[^"]*"' | sed 's/.*"\(.*\)"/\1/' | sort -u > /tmp/rc-apply.txt

# The registry table: the first string of each row in kSettings[].
sed -n "/^const Setting kSettings\[\] = {/,/^};/p" "$SRC/core/config.cpp" \
  | grep -o '^    {"[^"]*"' | sed 's/.*"\(.*\)"/\1/' | sort -u > /tmp/rc-registry.txt

# The presentation table: the first string of each row in kRows[].
sed -n "/^const RowPresentation kRows\[\] = {/,/^};/p" "$SRC/ui/menu_overlay.cpp" \
  | grep -o '^    {"[^"]*"' | sed 's/.*"\(.*\)"/\1/' | sort -u > /tmp/rc-rows.txt

fail=0
report() { # name, file-a, file-b, message
  local diff; diff=$(comm -23 "$2" "$3")
  if [ -n "$diff" ]; then fail=1; echo "FAIL $1: $4"; echo "$diff" | sed 's/^/      /'; fi
}
report "registry-vs-apply"  /tmp/rc-registry.txt /tmp/rc-apply.txt    "registry names with no ApplyValue key"
report "registry-vs-rows"   /tmp/rc-registry.txt /tmp/rc-rows.txt     "registry rows with no presentation row"
report "rows-vs-registry"   /tmp/rc-rows.txt     /tmp/rc-registry.txt "presentation rows with no registry row"

# The ini-only defaults table: every name must be an ApplyValue key, and must
# NOT also be a registry row. A name here that ApplyValue does not handle is a
# shipped default that silently does nothing; a name in both tables is two
# declared defaults for one setting, free to disagree.
sed -n "/^constexpr IniDefault kIniDefaults\[\] = {/,/^};/p" "$SRC/core/config.cpp" \
  | grep -o '^    {"[^"]*"' | sed 's/.*"\(.*\)"/\1/' | sort -u > /tmp/rc-inidef.txt

report "inidef-vs-apply"    /tmp/rc-inidef.txt  /tmp/rc-apply.txt    "ini-only defaults with no ApplyValue key"
overlap=$(comm -12 /tmp/rc-inidef.txt /tmp/rc-registry.txt)
if [ -n "$overlap" ]; then fail=1; echo "FAIL inidef-vs-registry: declared in BOTH tables"; echo "$overlap" | sed 's/^/      /'; fi

# THE SHIPPED INI MUST ONLY NAME KEYS THAT EXIST.
#
# The old example named eight that did not -- hud.zoom, hud.scale, hud.shift_*,
# hud.anchor_* and hud.include_gun_pass -- so anyone following it set values that
# were silently discarded. Commented lines count: the file documents by example,
# and a commented example of a key that does not exist is the same lie.
grep -oE "^#? *set +[a-z0-9_.]+" plugin/titanfall2vr.ini.example 2>/dev/null   | sed 's/.*set  *//' | sort -u > /tmp/rc-inifile.txt

report "shipped-ini-vs-apply" /tmp/rc-inifile.txt /tmp/rc-apply.txt "keys named in the shipped ini that ApplyValue does not accept"

# CONTROLS.md MUST NOT DOCUMENT A KEY THAT NO LONGER EXISTS.
#
# On 2026-09-09 twenty-six retired hotkey actions were deleted from kActions and
# CONTROLS.md went on listing every one of them -- a shipped document telling a
# player about controls the build does not have. Nobody noticed, because nothing
# looked at the two together. This does.
#
# Both directions matter, and only one of them is checked here: a documented key
# that does not exist is a lie to the reader, which is what this catches. A key
# that exists and is undocumented is a gap, caught by controls-vs-code below.
sed -n "/^constexpr ActionEntry kActions\[\] = {/,/^};/p" "$SRC/core/config.cpp" \
  | grep -oE '^    \{Action::[A-Za-z0-9_]+, *"[^"]*"' | sed 's/.*"\(.*\)"/\1/' | sort -u > /tmp/rc-actions.txt
sort -u /tmp/rc-actions.txt /tmp/rc-apply.txt > /tmp/rc-known.txt

# CONTROLS.md NO LONGER NAMES INI KEYS, so this compares DISPLAY NAMES.
#
# The keys were taken out of the player documentation deliberately: the mod has
# a settings panel, and a document answering "how do I change this" with a key
# name and a text file documents the developer's route, not the player's. The
# shipped ini documents every key in its own comments, which is where somebody
# editing it is already looking.
#
# This gate then read zero rows and said so rather than passing on an empty
# list. What it checks now is the same guarantee in the new vocabulary: the
# label in kSettings and the first column of the settings tables must be the
# same set. BOTH directions, where the key version only checked one -- a
# documented setting that does not exist is a lie to the reader, and a setting
# that exists undocumented is a gap.
#
# Scoped to the Settings reference section: earlier tables list controller
# inputs, whose first column is a thumbstick, not a setting.
sed -n "/^const Setting kSettings\[\] = {/,/^};/p" "$SRC/core/config.cpp" \
  | grep -oE '^    \{"[^"]*", *"[^"]*"' | sed 's/.*, *"\(.*\)"/\1/' | sort -u > /tmp/rc-labels.txt

sed -n '/^## Settings reference/,$p' CONTROLS.md 2>/dev/null \
  | grep -E '^\| [A-Z]' | sed 's/^| *//; s/ *|.*//' \
  | grep -v '^Setting$' | sort -u > /tmp/rc-controls.txt

report "controls-vs-code" /tmp/rc-controls.txt /tmp/rc-labels.txt "settings documented in CONTROLS.md that kSettings does not define"
report "code-vs-controls" /tmp/rc-labels.txt /tmp/rc-controls.txt "settings in kSettings that CONTROLS.md does not document"

# NO STATEMENT MAY PRETEND TO BE INSIDE AN `if` IT IS NOT INSIDE.
#
# Deleting 26 retired hotkey actions on 2026-09-09 removed their
# `if (ActionPressed(...))` lines and left three bodies behind, indented as
# though still controlled. They toggled three settings ONCE PER FRAME -- 2378
# flips against 2378 frames -- and /W4 said nothing, because the code is
# valid. The indentation was the only thing that was wrong, and indentation
# is the one thing the compiler does not read.
#
# So the deleting-a-mechanism rule at the top of CLAUDE.md gets a gate.
if command -v python >/dev/null 2>&1; then
    if ! python tools/orphan-statements.py > /tmp/rc-orphans.txt 2>&1; then
        fail=1; echo "FAIL orphan-statements: a statement is indented under an if that does not control it"
        sed 's/^/      /' /tmp/rc-orphans.txt
    fi
else
    echo "WARN: no python on PATH; the orphan-statement check did not run."
fi

na=$(wc -l < /tmp/rc-apply.txt); nr=$(wc -l < /tmp/rc-registry.txt); nw=$(wc -l < /tmp/rc-rows.txt)
ni=$(wc -l < /tmp/rc-inidef.txt)
nf=$(wc -l < /tmp/rc-inifile.txt)
nc=$(wc -l < /tmp/rc-controls.txt)
echo "counts: apply=$na settings=$nr rows=$nw inidefaults=$ni shipped-ini=$nf controls=$nc"

# A ZERO COUNT IS A BROKEN GATE, NOT A CLEAN ONE.
#
# On 2026-09-08 the sources moved from a flat src/ into subsystem directories.
# The three sed commands above still pointed at the old paths, so all three
# lists came back EMPTY -- and three empty lists have no differences between
# them, so this script printed "PASS: all six checks clean. and exited 0
# while checking absolutely nothing. It is the exact shape this project has a
# rule about: a null result from an instrument that never reached its target is
# about the instrument.
#
# Any of the three at zero now fails loudly instead.
if [ "$na" -eq 0 ] || [ "$nr" -eq 0 ] || [ "$nw" -eq 0 ] || [ "$ni" -eq 0 ] || [ "$nc" -eq 0 ]; then
    echo "FAIL: a list came back EMPTY -- this gate did not read what it thinks it read."
    echo "      Check the paths at the top of this script against the tree."
    exit 1
fi
if [ "$fail" -eq 0 ]; then echo "PASS: all eight checks clean."; else echo "one or more checks FAILED."; fi
exit "$fail"
