#!/usr/bin/env bash
# DISCLOSURE SCRUB GATE — PLAN-RELEASE-2026-09-05 §3.
#
# This file deliberately contains NO tokens. The denylist lives OUTSIDE the
# repository so that publishing the repository cannot publish the list of
# things that must never be published.
#
#   default location  ~/.tf2vr-scrub/denylist.txt
#   override          TF2VR_DENYLIST=/some/other/path tools/scrub.sh
#
# Matching is FIXED-STRING and CASE-SENSITIVE on purpose. Case-insensitive
# matching turns ordinary identifiers into false positives -- an everyday
# CamelCase symbol can contain a denylist word across its word boundary -- and a
# gate that cries wolf gets switched off. Put every casing you care about in the
# denylist instead.
#
# THE EXAMPLE THAT USED TO BE HERE WAS ITSELF A TOKEN. It named a real symbol in
# this tree and quoted the four characters of it that collide, which is a
# denylist entry written out in the one file that opens by claiming it contains
# none. The case-sensitive gate passed it, because the collision is only visible
# without case. Do not illustrate this rule with a real term again: the whole
# point of keeping the list outside the repository is defeated by an example.
#
# Usage:
#   tools/scrub.sh            scan the whole working tree (tracked files)
#   tools/scrub.sh staged     scan only what is staged  (the pre-commit hook)
#   tools/scrub.sh msg FILE   scan a commit message     (the commit-msg hook)
#
# Exit 0 = clean, 1 = hits found, 2 = the gate could not run (treat as a fail).
set -u

DENY="${TF2VR_DENYLIST:-$HOME/.tf2vr-scrub/denylist.txt}"
if [ ! -r "$DENY" ]; then
    echo "scrub: FAIL — denylist not readable at $DENY"
    echo "scrub: this gate cannot pass by default. See PLAN-RELEASE-2026-09-05 §3.1."
    exit 2
fi

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "scrub: FAIL — not inside a git repository"
    exit 2
}
cd "$repo_root" || exit 2

# Vendored third-party and the CMake build tree are excluded: they are not ours
# to edit and the fetched OpenXR SDK changelog legitimately names other
# vendors' hardware.
mode="${1:-tree}"
case "$mode" in
    tree)
        hits=$(git grep -I -n -F -f "$DENY" -- \
                   ':!plugin/third_party' ':!plugin/build' 2>/dev/null)
        where="working tree"
        ;;
    staged)
        # ADDED lines only. A commit that REMOVES a token — which is what the
        # scrub itself does — must not be blocked by the very text it is
        # deleting, and `git diff` carries removed lines too. `+++` is the file
        # header, not content.
        hits=$(git diff --cached -U0 -- \
                   ':!plugin/third_party' ':!plugin/build' \
               | grep '^+' | grep -v '^+++' \
               | grep -F -f "$DENY")
        where="staged changes (added lines)"
        ;;
    msg)
        [ $# -ge 2 ] || { echo "scrub: usage: scrub.sh msg <file>"; exit 2; }
        hits=$(grep -n -F -f "$DENY" "$2")
        where="commit message"
        ;;
    *)
        echo "scrub: usage: scrub.sh [tree|staged|msg <file>]"
        exit 2
        ;;
esac

if [ -n "$hits" ]; then
    echo "scrub: FAIL — disclosure tokens found in the $where:"
    echo "$hits" | sed 's/^/      /'
    echo "scrub: nothing is published until these are gone. §3.2 has the procedure."
    exit 1
fi

echo "scrub: clean ($where)"
exit 0
