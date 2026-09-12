#!/usr/bin/env python
"""Find statements that LOOK controlled by an `if` above them and are not.

THE BUG THIS EXISTS FOR, 2026-09-09. Deleting 26 retired hotkey actions removed
the `if (ActionPressed(...))` lines and left three of their bodies behind:

    if (ActionPressed(Action::ToggleFitHorizontal))
        SetFitHorizontal(!IsFitHorizontalArmed());
        SetLensShearEnabled(!LensShearEnabled());        <-- no `if`. Every frame.

That compiles without a warning at /W4 and reads correctly to a human, because
the indentation says what the author meant instead of what the compiler saw.
Three settings toggled themselves ONCE PER FRAME for a whole session: 2378
flips against 2378 frames published, in the run that ended in a hang.

The shape is: a plain statement indented DEEPER than the complete statement
above it, with no brace opening a block between them. Nothing can be
controlling it -- it is unconditional, and it is written as though it is not.

Case labels are excluded: `case 1: DoThing();` legitimately puts its body at a
deeper indent on the following lines.

Usage:  python tools/orphan-statements.py [root]
Exit 1 if any are found, so it can gate a commit.
"""
import glob, os, sys

BS = chr(92)
SKIP_PREFIX = ('if', 'for', 'while', 'else', 'do', 'return', 'case', 'default',
               '}', '{', '#', 'break', 'continue', 'goto')

def scan(path):
    out = []
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    prev, previ = None, 0
    for i, cur in enumerate(lines):
        s = cur.strip()
        if not s or s.startswith(('//', '*', '/*')):
            continue                      # comments do not change the nesting
        ci = len(cur) - len(cur.lstrip())
        if prev is not None:
            ps = prev.strip()
            if (ci > previ
                    and ps.endswith(';') and s.endswith(';')
                    and not s.startswith(SKIP_PREFIX)
                    and not ps.startswith(('case', 'default'))
                    and BS not in ps):
                out.append((i + 1, previ, ps, ci, s))
        prev, previ = cur, ci
    return out

def main(argv):
    root = argv[1] if len(argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'plugin', 'src')
    total = 0
    for path in sorted(glob.glob(os.path.join(root, '**', '*.cpp'), recursive=True) +
                       glob.glob(os.path.join(root, '**', '*.h'), recursive=True)):
        for n, pi, ps, ci, s in scan(path):
            total += 1
            print("%s:%d" % (os.path.relpath(path, os.getcwd()), n))
            print("    indent %-2d  %s" % (pi, ps[:92]))
            print("    indent %-2d  %s   <-- nothing controls this" % (ci, s[:92]))
    print("orphan statements: %d" % total)
    return 1 if total else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
