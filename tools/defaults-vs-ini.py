#!/usr/bin/env python
"""Prove that a fresh install reproduces a given ini, setting for setting.

THE WEARER'S REQUIREMENT, MADE CHECKABLE. "The config that I already had --
THAT SHOULD BE THE DEFAULT FOR EVERY SETTING. PERIOD." That is a claim about two
things agreeing, and until this existed the only way to test it was to install
the shipping build into a clean profile and look at it in a headset.

It reads the defaults out of the source -- kSettings[] (the panel rows) and
kIniDefaults[] (the ini-only keys) -- and compares them against every live
`set` line in the ini you point it at. Three answers come back:

  DIVERGENCE          a key the ini sets to something other than the default.
                      Each one is a setting a new player gets differently from
                      whoever owns that ini.
  DISCARDED           a live `set` line naming a key ApplyValue does not accept.
                      Silently ignored by the build today. A renamed key looks
                      exactly like this.
  NO CODE DEFAULT     a key that exists but is deliberately undefaulted --
                      render.width/height, where 0 means derive from the
                      headset, and the stereo keys.

Usage:  python tools/defaults-vs-ini.py "<path to titanfall2vr.ini>"

Exit 1 on divergence or discarded lines, 0 otherwise, so it can gate a release.
"""
import os, re, sys

def num(x): return float(x.rstrip('fF'))

def block(src, head):
    i = src.index(head)
    return src[i:src.index('\n};', i)]

def main(argv):
    if len(argv) != 2:
        print(__doc__.strip()); return 2
    ini = argv[1]
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    cfg = open(os.path.join(here, 'plugin', 'src', 'core', 'config.cpp'),
               encoding='utf-8', errors='replace').read()

    apply_keys = set(re.findall(r'std::strcmp\(name,\s*"([^"]+)"\)', cfg))
    ab = block(cfg, 'constexpr ActionEntry kActions[] = {')
    actions = set(re.findall(r'\{Action::\w+,\s*"([^"]+)"', ab))

    # kIniDefaults[]: one {"name", value} per line.
    defaults = {}
    for line in block(cfg, 'constexpr IniDefault kIniDefaults[] = {').split('\n'):
        m = re.match(r'^    \{"([^"]+)",\s*(-?[0-9.]+f?)\s*\},?\s*$', line)
        if m: defaults[m.group(1)] = num(m.group(2))

    # A DEFAULT MAY BE A NAMED CONSTANT, NOT A LITERAL, and one is.
    #
    # render.scale's default has to be the same number in three places -- the
    # g_renderScale initialiser, the seed for a headset this build has never
    # seen, and the kSettings row below -- because that seed WINS over the ini,
    # so a default changed in only one of them is inert. It is therefore a
    # constant, kDefaultRenderScale. This parser hit it and failed loudly
    # instead of skipping the row, which was the right thing to do; this is the
    # fix. Every `constexpr float kName = 1.23f;` in the headers is resolved,
    # and a name that still cannot be resolved is reported exactly as loudly as
    # an unparsable row -- it would otherwise read as agreeing while meaning
    # anything at all.
    consts = {}
    hdrs = os.path.join(here, 'plugin', 'src')
    for root, _dirs, files in os.walk(hdrs):
        for fn in files:
            if not fn.endswith('.h'):
                continue
            try:
                text = open(os.path.join(root, fn), encoding='utf-8',
                            errors='replace').read()
            except OSError:
                continue
            for cm in re.finditer(
                    r'constexpr\s+float\s+([A-Za-z_]\w*)\s*=\s*(-?[0-9.]+)f?\s*;', text):
                consts[cm.group(1)] = float(cm.group(2))

    # kSettings[]: the row's default is the THIRD float after the getter --
    # the field order is getter, min, max, default.
    sb = block(cfg, 'const Setting kSettings[] = {')
    rows = re.split(r'\n    \{"', sb)[1:]
    unparsed = []
    FLOAT_OR_NAME = r'(-?[0-9.]+f?|[A-Za-z_]\w*)'
    for r in rows:
        name = r.split('"', 1)[0]
        m = re.search(r'\b[A-Za-z_]\w*,\s*' + FLOAT_OR_NAME + r',\s*'
                      + FLOAT_OR_NAME + r',\s*' + FLOAT_OR_NAME + r'\s*,', r)
        if not m:
            unparsed.append(name)
            continue
        raw = m.group(3)
        if re.match(r'^-?[0-9.]+f?$', raw):
            defaults[name] = num(raw)
        elif raw in consts:
            defaults[name] = consts[raw]
        else:
            unparsed.append('%s (unresolved constant %s)' % (name, raw))

    # A ROW THIS CANNOT PARSE IS NOT A ROW THAT AGREES. Reading fewer rows than
    # exist would make a real divergence invisible and still print a clean
    # result, which is the failure this project has a rule about.
    if unparsed:
        print("FAIL: could not read the default of %d kSettings row(s): %s"
              % (len(unparsed), ', '.join(unparsed)))
        return 1

    live = {}
    for n, line in enumerate(open(ini, encoding='utf-8', errors='replace'), 1):
        t = line.strip()
        if not t or t[0] in '#;': continue
        m = re.match(r'^set\s+([A-Za-z0-9_.]+)\s*=?\s*(\S+)', t)
        if m: live[m.group(1)] = (m.group(2), n)   # last wins, as the parser does

    diverge, discarded, nodefault = [], [], []
    for k, (v, n) in sorted(live.items()):
        if k not in apply_keys and k not in actions:
            discarded.append((n, k, v)); continue
        if k not in defaults:
            nodefault.append(k); continue
        try: lv = num(v)
        except ValueError: continue
        if abs(lv - defaults[k]) > 1e-6: diverge.append((k, lv, defaults[k]))

    print("ini: %s" % ini)
    print("defaults read from source: %d   live `set` lines: %d" % (len(defaults), len(live)))
    print()
    print("DIVERGENCE from the shipped defaults: %d" % len(diverge))
    for k, lv, dv in diverge:
        print("   %-34s ini=%-12g default=%g" % (k, lv, dv))
    print("DISCARDED -- set, but ApplyValue does not accept the name: %d" % len(discarded))
    for n, k, v in discarded:
        print("   line %-6d %s = %s" % (n, k, v))
    print("no code default (expected: render.width/height and the stereo keys): %d" % len(nodefault))

    return 1 if (diverge or discarded) else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
