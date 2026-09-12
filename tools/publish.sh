#!/usr/bin/env bash
# Materialise the PUBLISHABLE tree into a separate repository.
#
# WHY THIS IS NOT `git push`.
#
# This repository's history cannot be published. It predates the disclosure
# gate: a fixed-string scan of `git log --all -p` against the denylist returns
# 146 hits, all of them in commits made before `scrub.sh` existed. Those commits
# are not reachable by the pre-commit hook, which only ever sees what is being
# added now. Publishing the working TREE is safe; publishing the HISTORY that
# produced it is not, and no amount of scrubbing the tip changes that.
#
# A private repository is not an exception. Private is one click away from
# public and GitHub keeps what it was given, so the private repository must
# already contain exactly what a public one would.
#
# So the published repository is a SEPARATE repository with its own history,
# seeded from this tree. This script maintains it. Run it as often as you like;
# it is idempotent, it never pushes, and it never touches this repository.
#
# THE DESTINATION IS A DERIVATIVE. NEVER EDIT IT DIRECTLY.
#
# Every run clears it and writes it again from this tree, so anything authored
# there -- a local edit, a merged pull request, a change made in GitHub's web
# editor -- is silently discarded on the next run. Fixes go in THIS repository
# and travel outward. The script refuses to run over a dirty destination, which
# catches the local case; the remote case it can only warn about, so fetch
# before you publish if anyone else can push.
#
# Usage:
#   tools/publish.sh [--squash] [destination]      default: ../titanfall2vr-public
#
#   --squash   Discard the published repository's history and start it again at
#              one commit. The published history is pure derivative -- every
#              tree in it was generated from a commit here -- so there is
#              nothing in it to lose. Intended for the moment the repository
#              goes public: cleanup commits made while it was private are noise
#              a stranger should not have to read. It re-inits the destination,
#              restores the remote, the identity and the hooks, and stops. YOU
#              stage, YOU write the commit, and YOU type the force-push.
#
# What it does NOT do: push. It prints the command. Nothing leaves this machine
# without a person typing that.

set -u

squash=0
args=()
for arg in "$@"; do
    case "$arg" in
        --squash) squash=1 ;;
        -*) echo "publish: unknown option $arg"; exit 2 ;;
        *) args+=("$arg") ;;
    esac
done
set -- ${args[@]+"${args[@]}"}

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "publish: FAIL -- not inside a git repository"; exit 2; }
cd "$repo_root" || exit 2
# NORMALISE THE PATH FORM BEFORE COMPARING ANYTHING TO IT.
#
# On Windows `git rev-parse --show-toplevel` answers `C:/dev/titanfall2vr` while
# this shell's own `pwd` answers `/c/dev/titanfall2vr`. They name one directory
# and compare unequal as strings, which silently disabled the source-is-
# destination guard below: the guard was written, was tested, and appeared to
# pass -- but the test fired a DIFFERENT check (the source did not contain
# CMakeLists.txt), so the comparison it was meant to exercise was never run.
# A control has to test the property being relied on.
repo_root=$(pwd)

dest="${1:-$(cd .. && pwd)/titanfall2vr-public}"

# THE DESTINATION MUST NOT BE THE SOURCE, AND MUST NOT CONTAIN IT.
#
# Running this from inside the destination -- `cd ../titanfall2vr-public &&
# ../titanfall2vr/tools/publish.sh`, or just `./tools/publish.sh` after cd-ing
# there -- makes `git rev-parse --show-toplevel` resolve to the DESTINATION.
# The script then cleared that directory and copied it onto itself from a file
# list of things it had just deleted, producing an empty tree. It reported "280
# files" while copying none, and the empty tree was committed and pushed.
#
# Nothing was lost, because the destination is a derivative. It was still a
# self-inflicted outage on a published repository, and one comparison prevents
# it.
dest_abs=$(cd "$dest" 2>/dev/null && pwd) || dest_abs="$dest"
if [ "$dest_abs" = "$repo_root" ]; then
    echo "publish: FAIL -- the destination IS the source repository:"
    echo "         $repo_root"
    echo ""
    echo "         You are almost certainly running this from inside the published"
    echo "         repository. Run it from the source repository instead; it writes"
    echo "         to ../titanfall2vr-public by default."
    exit 2
fi
case "$repo_root/" in
    "$dest_abs"/*)
        echo "publish: FAIL -- the source repository is inside the destination."
        echo "         source:      $repo_root"
        echo "         destination: $dest_abs"
        echo "         Clearing the destination would delete the source."
        exit 2 ;;
esac
# `tools/publish.sh` must be the one in the source, not a copy in the
# destination that happens to be on the path.
if [ ! -f "$repo_root/plugin/CMakeLists.txt" ] || [ ! -f "$repo_root/tools/scrub.sh" ]; then
    echo "publish: FAIL -- $repo_root does not look like the titanfall2vr source repository."
    exit 2
fi

# ---------------------------------------------------------------------------
# THE EXCLUSION LIST, and the reason for each. PLAN-RELEASE section 7.
# ---------------------------------------------------------------------------
#
#   docs/                   THE ENTIRE DEVELOPMENT JOURNAL. 81 files, 1.2 MB of
#                           KICKOFF-*, RESULT-*, HANDOFF-*, PLAN-* and dated
#                           evidence documents: working notes addressed to
#                           whoever was doing the work that day, full of
#                           abandoned theories, instrument readings and internal
#                           vocabulary. PLAN-RELEASE section 7 shipped them on
#                           the theory that they make the reasoning checkable.
#                           Reversed 2026-09-11 by the wearer, on seeing what it
#                           actually looks like to a stranger opening the repo.
#   docs/dev/               working state: STATE-*, kickoffs, handoffs. Same.
#   RELEASE-CHECKLIST.md    the internal gate for cutting a release, carrying
#                           the wearer's own attestations, the regions nobody
#                           had read yet, and an UNSIGNED signature line. Its
#                           closing paragraph says the repository stays private
#                           until the reader is satisfied -- which reads badly
#                           from inside a public repository. It was never on
#                           this list; it shipped because nothing excluded it.
#   CLAUDE.md               instructions to the agent working on this repo. It
#                           describes a directory layout -- docs/, docs/dev/,
#                           docs/archive/ -- that no longer exists once the
#                           above are excluded, so publishing it would ship a
#                           map of a place nobody can go.
#
# KEPT, and it is the ONLY thing under docs/ that ships: docs/images, which
# holds the two controller diagrams CONTROLS.md embeds. Nothing else in the
# shipped documentation references docs/ at all -- checked, not assumed.
#
# Spelled as four exclusions rather than ':!docs' plus a positive re-include:
# mixing a positive pathspec in restricts the WHOLE selection to it, so that
# spelling silently published nothing at all. Caught by counting the result,
# which is the only reason to count it.
exclude_paths=(
    ':!docs/archive'
    ':!docs/dev'
    ':(exclude,glob)docs/*.md'
    ':(exclude,glob)docs/*.txt'
    ':!CLAUDE.md'
    ':!RELEASE-CHECKLIST.md'
)

# ---------------------------------------------------------------------------
# GATES. Every one of these has to pass before a single file is copied.
# ---------------------------------------------------------------------------
fail=0

if [ -n "$(git status --porcelain)" ]; then
    echo "publish: FAIL -- the working tree is dirty. Commit first; what gets"
    echo "         published must be a commit you can point at."
    fail=1
fi

tools/scrub.sh >/dev/null 2>&1 || { echo "publish: FAIL -- scrub.sh is not clean."; fail=1; }

# The crosscheck's COUNTS, not its exit code alone. It has passed on three empty
# lists before now.
cross=$(tools/registry-crosscheck.sh 2>&1)
counts=$(printf '%s\n' "$cross" | grep '^counts:')
if ! printf '%s\n' "$cross" | grep -q '^PASS'; then
    echo "publish: FAIL -- registry-crosscheck did not pass:"
    printf '%s\n' "$cross" | sed 's/^/      /'
    fail=1
elif [ -z "$counts" ] || printf '%s\n' "$counts" | grep -qE '=0( |$)'; then
    echo "publish: FAIL -- registry-crosscheck passed with a zero count: $counts"
    fail=1
fi

# The whole-word case-insensitive sweep. The gate itself is case-SENSITIVE by
# design, so this is the check that catches a token whose casing nobody thought
# to add to the list. Embedded substrings are excluded by -w, which is the
# false-positive class the gate is case-sensitive to avoid.
DENY="${TF2VR_DENYLIST:-$HOME/.tf2vr-scrub/denylist.txt}"
if [ -r "$DENY" ]; then
    ci=$(git grep -I -l -w -i -F -f "$DENY" -- \
             ':!plugin/third_party' ':!plugin/build' "${exclude_paths[@]}" 2>/dev/null)
    if [ -n "$ci" ]; then
        echo "publish: FAIL -- a denylist word appears as a whole word, in some casing, in:"
        printf '%s\n' "$ci" | sed 's/^/      /'
        fail=1
    fi
else
    echo "publish: FAIL -- denylist not readable at $DENY"
    fail=1
fi

[ "$fail" -eq 0 ] || { echo "publish: nothing was copied."; exit 1; }

# ---------------------------------------------------------------------------
# MATERIALISE
# ---------------------------------------------------------------------------
source_sha=$(git rev-parse --short HEAD)
files=$(git ls-files -- ':!plugin/third_party/imgui/imgui_demo.cpp' "${exclude_paths[@]}")
count=$(printf '%s\n' "$files" | grep -c .)
[ "$count" -gt 0 ] || { echo "publish: FAIL -- the file list came back empty."; exit 1; }

mkdir -p "$dest" || exit 1

# A DIRTY DESTINATION MEANS SOMEONE EDITED THE DERIVATIVE. Refuse rather than
# overwrite it: the next line clears the directory, and a change made only there
# is gone with no trace of what it was.
if [ -d "$dest/.git" ] && [ -n "$(git -C "$dest" status --porcelain 2>/dev/null)" ]; then
    echo "publish: FAIL -- $dest has uncommitted changes."
    git -C "$dest" status --short | sed 's/^/      /'
    echo "publish: that directory is generated from this one and is about to be cleared."
    echo ""
    echo "         USUALLY THIS IS THE LAST RUN'S OUTPUT, not an edit: publish, commit,"
    echo "         push is the cycle, and running publish twice without committing lands"
    echo "         here. If that is what this is, commit there and run this again."
    echo ""
    echo "         If instead someone edited the derivative, move those changes into THIS"
    echo "         repository first -- the next run discards them with no record of what"
    echo "         they were."
    exit 1
fi

# Same question for the remote: if it has moved ahead of the local copy, someone
# pushed to it directly and this run is about to bury that.
if [ -d "$dest/.git" ] && git -C "$dest" rev-parse --verify -q origin/main >/dev/null 2>&1; then
    behind=$(git -C "$dest" rev-list --count main..origin/main 2>/dev/null || echo 0)
    if [ "${behind:-0}" -gt 0 ]; then
        echo "publish: WARNING -- origin/main is $behind commit(s) ahead of the local publish repo."
        echo "         Someone pushed to the published repository directly. Look at those"
        echo "         commits before you force anything over them. (This count is from the"
        echo "         last fetch; run 'git -C \"$dest\" fetch' for a current answer.)"
    fi
fi

if [ ! -d "$dest/.git" ]; then
    git -C "$dest" init -q -b main || exit 1
    echo "publish: initialised a new repository at $dest"
fi

# --- --squash: start the published history again at one commit --------------
#
# Safe because the destination holds nothing of its own: every tree it has ever
# had was generated from a commit in this repository, and the two checks above
# are what establish that is still true. The remote URL, the identity and the
# scrub hooks are captured and restored, because losing any of them turns the
# next publish into a silent mistake.
if [ "$squash" -eq 1 ]; then
    old_remote=$(git -C "$dest" remote get-url origin 2>/dev/null || true)
    old_name=$(git -C "$dest" config user.name 2>/dev/null || true)
    old_email=$(git -C "$dest" config user.email 2>/dev/null || true)
    old_commits=$(git -C "$dest" rev-list --count HEAD 2>/dev/null || echo 0)

    rm -rf "$dest/.git" || exit 1
    git -C "$dest" init -q -b main || exit 1
    [ -n "$old_remote" ] && git -C "$dest" remote add origin "$old_remote"
    [ -n "$old_name" ] && git -C "$dest" config user.name "$old_name"
    [ -n "$old_email" ] && git -C "$dest" config user.email "$old_email"

    mkdir -p "$dest/.git/hooks"
    printf '#!/usr/bin/env bash\nexec tools/scrub.sh staged\n' > "$dest/.git/hooks/pre-commit"
    printf '#!/usr/bin/env bash\nexec tools/scrub.sh msg "$1"\n' > "$dest/.git/hooks/commit-msg"
    chmod +x "$dest/.git/hooks/pre-commit" "$dest/.git/hooks/commit-msg"

    echo "publish: SQUASH -- discarded $old_commits published commit(s); history restarts at one."
    echo "publish:          remote, identity and scrub hooks restored."
fi

# Clear everything the last run put there, so a file deleted here disappears
# there too. .git is preserved; nothing else in the destination is.
find "$dest" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} + || exit 1

copied=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    mkdir -p "$dest/$(dirname "$f")" || exit 1
    cp -p "$f" "$dest/$f" || { echo "publish: FAIL -- could not copy $f"; exit 1; }
    copied=$((copied + 1))
done <<EOF
$files
EOF

# COUNT WHAT WAS COPIED, NOT WHAT WAS LISTED.
#
# This reported "280 files ->" while copying ZERO, and the empty tree was
# committed and pushed before anyone looked. `$count` is the length of a list;
# it says nothing about whether a single byte moved. The loop also used to run
# inside a pipeline, so its subshell could not have reported a failure even if
# it had noticed one -- hence the here-document above.
#
# This project has a rule for exactly this shape, learned from a gate that
# passed on three empty lists: read the counts, and make the count come from the
# thing being measured.
if [ "$copied" -ne "$count" ]; then
    echo "publish: FAIL -- listed $count files but copied $copied."
    exit 1
fi

# A POSITIVE CONTROL, because a count can agree with itself and still be wrong.
for probe in README.md tools/scrub.sh plugin/CMakeLists.txt; do
    [ -f "$dest/$probe" ] || { echo "publish: FAIL -- $probe is not in $dest after copying."; exit 1; }
done

echo "publish: $copied files -> $dest (copied and spot-checked, not merely listed)"
echo "publish: excluded $(git ls-files -- docs ':!docs/images' | grep -vc '^docs/images/') development-journal files, plus CLAUDE.md. Only docs/images ships."
echo ""
echo "Source commit (this repository, NOT published): $source_sha"
echo ""
echo "Next:"
echo "  cd \"$dest\""
echo "  git add -A && git status --short | head"
if [ "$squash" -eq 1 ]; then
    echo "  git commit -m 'titanfall2vr v0.1.1'"
    echo "  git remote add origin <url>          # only if it printed no remote above"
    echo "  git push --force origin main"
    echo ""
    echo "THE PUSH IS A FORCE PUSH and it replaces the published history."
    echo "Read the commit you just wrote before you type it: it is the only one a"
    echo "stranger will ever see."
else
    echo "  git commit -m '<what changed>'"
    echo "  git remote add origin <url>          # once"
    echo "  git push -u origin main"
fi
echo ""
echo "NOTHING HAS BEEN PUSHED. That is deliberate."
