#!/usr/bin/env python3
"""Attribute the cycles of an xctrace CPU Profiler export to functions,
inlined functions and source lines, with checks against misattribution.

usage:
  xct.py self      XML DSYM [--top N]           self cycles by innermost inlined function
  xct.py compare   BASE_DSYM NEW_DSYM BASE_XML:NEW_XML ... [--top N]
                                                 same fixed work, base vs new, per pair
  xct.py lines     FUNC XML DSYM [--top N]      cycles by source line inside FUNC
  xct.py callers   FUNC XML DSYM [--depth D]    inline chains that FUNC's cycles sit in
  xct.py contains  XML DSYM NAME...             share of cycles with NAME on the stack,
                                                 real frames or inlined
  xct.py inclusive XML [--top N]               inclusive cycles by real (not inlined)
                                                 frame; blind to inlined functions
  xct.py check     XML DSYM                     look up every address one at a time and
                                                 compare with the batched lookups

For CPU Counters recordings (counters_template.py, then record.sh --template):
  xct.py pmi         XML DSYM [--counter C] [--top N]
                                                 a sampling mode's samples by function,
                                                 line and instruction, per counter
  xct.py bottlenecks XML                       the bottlenecks mode's shares of issue
                                                 slots: useful, discarded, processing,
                                                 delivery

DSYM is a .dSYM bundle, or the DWARF file inside one. The binary is found in
the export by the dSYM's UUID, so a wrong or stale dSYM is an error, not a
quiet misattribution. Percentages are of all the export's cycles (or, for pmi,
of the counter's samples), including samples outside the binary.
"""
import collections
import glob
import os
import random
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

BATCH = 500
SPOT_CHECKS = 24


def die(msg):
    sys.exit(f'xct.py: {msg}')


def dwarf_path(dsym):
    if os.path.isdir(dsym):
        found = glob.glob(os.path.join(dsym, 'Contents/Resources/DWARF/*'))
        if len(found) != 1:
            die(f'{dsym}: expected one DWARF file inside, found {len(found)}')
        return found[0]
    return dsym


def dsym_uuids(dwarf):
    out = subprocess.run(['dwarfdump', '--uuid', dwarf], capture_output=True, text=True).stdout
    uuids = set(re.findall(r'UUID: ([0-9A-F-]+)', out))
    if not uuids:
        die(f'{dwarf}: no UUID (is it a dSYM?)')
    return uuids


class Profile:
    """The samples of one export: weight, leaf address, the leaf's binary UUID
    and the real frames' names."""

    def __init__(self, xml_path):
        root = ET.parse(xml_path).getroot()
        by_id = {el.get('id'): el for el in root.iter() if el.get('id') is not None}

        def deref(el):
            r = el.get('ref')
            return by_id[r] if r is not None else el

        self.samples = []
        self.binaries = {}  # UUID -> (name, load address)
        self.total = 0
        self.unit = None
        for row in root.iter('row'):
            w = row.find('cycle-weight')
            unit = 'cycles'
            if w is None:
                w = row.find('weight')
                unit = 'weight'
            bt = row.find('backtrace')
            if bt is None:
                # xctrace 27 exports the stack as a tagged-backtrace.
                bt = row.find('tagged-backtrace')
            if w is None or bt is None:
                continue
            if self.unit is None:
                self.unit = unit
            elif self.unit != unit:
                die(f'{xml_path}: mixes cycle-weight and weight rows')
            weight = int(deref(w).text or 0)
            self.total += weight
            frames = [deref(f) for f in deref(bt).findall('frame')]
            if not frames:
                self.samples.append((weight, None, None, ()))
                continue
            leaf = frames[0]
            uuid = None
            b = leaf.find('binary')
            if b is not None:
                b = deref(b)
                uuid = b.get('UUID')
                if uuid:
                    self.binaries.setdefault(uuid, (b.get('name', '?'), b.get('load-addr')))
            names = tuple(f.get('name') for f in frames)
            self.samples.append((weight, leaf.get('addr'), uuid, names))
        if self.total == 0:
            die(f'{xml_path}: no weighted samples (export the cpu-profile table)')
        if self.unit != 'cycles':
            print(f'note: {xml_path} has no cycle weights; percentages are of sample weight')

    def binary_for(self, dwarf):
        uuids = dsym_uuids(dwarf)
        hits = [u for u in self.binaries if u in uuids]
        if not hits:
            seen = ', '.join(f'{n} {u}' for u, (n, _) in self.binaries.items())
            die(f'{dwarf} (UUID {", ".join(sorted(uuids))}) matches no binary in the '
                f'profile; it is the wrong or a stale dSYM. Binaries seen: {seen}')
        return hits[0], self.binaries[hits[0]][1]


def parse_block(block):
    """One address's atos -i block: [(function, file:line or None)], innermost
    first; empty for an address atos can't symbolicate."""
    chain = []
    for line in block.split('\n'):
        line = line.strip()
        if not line:
            continue
        m = re.match(r'(\S+) \(in [^)]*\)', line)
        if not m:
            chain.append(('?', None))
            continue
        loc = re.search(r'\(([^()]*:\d+)\)\s*$', line)
        # Clang's local-symbol suffixes (wmp_get_word_entry.1436) change from
        # build to build, so drop them.
        chain.append((re.sub(r'\.\d+$', '', m.group(1)), loc.group(1) if loc else None))
    return chain


def atos(dwarf, load, addrs):
    out = subprocess.run(['atos', '-i', '-o', dwarf, '-l', load] + addrs,
                         capture_output=True, text=True)
    if out.returncode != 0:
        die(f'atos failed: {out.stderr.strip()}')
    # Every address's block ends with a blank line, and an address atos can't
    # symbolicate gets an empty block. So split without stripping: strip()
    # would delete a leading empty block and shift every later address onto
    # its neighbour's function.
    return out.stdout.split('\n\n')[:-1]


def symbolicate(dwarf, load, addrs, spot_checks=SPOT_CHECKS):
    """address -> inline chain, checked: one block per address, and a sample
    of addresses (each batch's first among them) looked up one at a time must
    give the same chain."""
    chains = {}
    for c in range(0, len(addrs), BATCH):
        chunk = addrs[c:c + BATCH]
        blocks = atos(dwarf, load, chunk)
        if len(blocks) != len(chunk):
            # Don't guess at the pairing; look each address up on its own.
            blocks = ['\n'.join(atos(dwarf, load, [a])) for a in chunk]
        for a, b in zip(chunk, blocks):
            chains[a] = parse_block(b)
    if addrs and spot_checks:
        rng = random.Random(0)
        picks = set(addrs[::BATCH]) | set(rng.sample(addrs, min(spot_checks, len(addrs))))
        for a in picks:
            alone = atos(dwarf, load, [a])
            if len(alone) != 1 or parse_block(alone[0]) != chains[a]:
                die(f'batched and single lookups of {a} disagree; attribution is unreliable')
    return chains


def leaf_chains(profile, dsym):
    """Per-sample innermost-first inline chain for samples in the dSYM's binary;
    None for samples elsewhere."""
    dwarf = dwarf_path(dsym)
    uuid, load = profile.binary_for(dwarf)
    addrs = list(dict.fromkeys(a for _, a, u, _ in profile.samples if u == uuid and a))
    chains = symbolicate(dwarf, load, addrs)
    return [chains[a] if u == uuid and a else None for _, a, u, _ in profile.samples], uuid, load


def self_by_function(profile, dsym):
    chains, uuid, load = leaf_chains(profile, dsym)
    by_func = collections.Counter()
    by_pc = collections.Counter()
    for (weight, addr, u, _), chain in zip(profile.samples, chains):
        if chain is None:
            name = profile.binaries.get(u, ('<no binary>', None))[0]
            by_func[f'<other: {name}>'] += weight
        elif not chain:
            by_func['<unsymbolicated>'] += weight
        else:
            by_func[chain[0][0]] += weight
            rel = hex(int(addr, 16) - int(load, 16) + 0x100000000)
            by_pc[(rel, chain[0][0], chain[0][1])] += weight
    return by_func, by_pc


def pct(x, total):
    return 100 * x / total


def opt(args, name, default):
    if name in args:
        i = args.index(name)
        v = args[i + 1]
        del args[i:i + 2]
        return type(default)(v)
    return default


def cmd_self(args):
    top = opt(args, '--top', 25)
    xml, dsym = args
    p = Profile(xml)
    by_func, by_pc = self_by_function(p, dsym)
    outside = sum(c for f, c in by_func.items() if f.startswith('<other'))
    print(f'{p.total / 1e9:.2f}G {p.unit}, {pct(outside, p.total):.1f}% outside the binary')
    for f, c in by_func.most_common(top):
        print(f'  {pct(c, p.total):6.2f}%  {f}')
    print('hottest PCs:')
    for (a, f, loc), c in by_pc.most_common(8):
        print(f'  {pct(c, p.total):6.2f}%  {a}  {f} ({loc})')


def cmd_compare(args):
    top = opt(args, '--top', 25)
    base_dsym, new_dsym, *pairs = args
    if not pairs:
        die('compare needs at least one BASE_XML:NEW_XML pair')
    results = []
    for pair in pairs:
        bx, nx = pair.split(':')
        b, n = Profile(bx), Profile(nx)
        bf, _ = self_by_function(b, base_dsym)
        nf, _ = self_by_function(n, new_dsym)
        results.append((b.total, n.total, bf, nf))
    for i, (bt, nt, _, _) in enumerate(results):
        print(f'pair {i + 1}: base {bt / 1e9:.2f}G, new {nt / 1e9:.2f}G, change {pct(nt - bt, bt):+.2f}%')
    names = set().union(*[set(bf) | set(nf) for _, _, bf, nf in results])

    # A change as a share of its pair's base total, so changes add up to the
    # total's change.
    def change(r, f):
        bt, _, bf, nf = r
        return pct(nf[f] - bf[f], bt)

    ranked = sorted(names, key=lambda f: -max(abs(change(r, f)) for r in results))[:top]
    head = ''.join(f'  {"pair " + str(i + 1):>24}' for i in range(len(results)))
    print(f'{"function (base% -> new%, change)":44}{head}')
    for f in ranked:
        cells = ''.join(f'  {pct(r[2][f], r[0]):6.2f}->{pct(r[3][f], r[1]):6.2f} {change(r, f):+6.2f}'
                        for r in results)
        print(f'  {f[:42]:42}{cells}')
    if len(results) > 1:
        same = [f for f in names
                if all(change(r, f) > 0.05 for r in results) or all(change(r, f) < -0.05 for r in results)]
        nets = [sum(change(r, f) for f in same) for r in results]
        print(f'net of the {len(same)} functions that move the same way, by more than 0.05%, '
              f'in every pair: ' + ' / '.join(f'{x:+.2f}%' for x in nets))


def cmd_lines(args):
    top = opt(args, '--top', 15)
    func, xml, dsym = args
    p = Profile(xml)
    chains, _, _ = leaf_chains(p, dsym)
    by_line = collections.Counter()
    for (weight, _, _, _), chain in zip(p.samples, chains):
        if chain and chain[0][0] == func:
            by_line[chain[0][1] or '?'] += weight
    print(f'{func}: {pct(sum(by_line.values()), p.total):.2f}% of {p.unit}')
    for loc, c in by_line.most_common(top):
        print(f'  {pct(c, p.total):6.2f}%  {loc}')
    print('note: a line\'s cycles sit on the instruction that waits, so a hot line can be '
          'a load stall or the consumer of a slow value, not its own work.')


def cmd_callers(args):
    depth = opt(args, '--depth', 3)
    func, xml, dsym = args
    p = Profile(xml)
    chains, _, _ = leaf_chains(p, dsym)
    by_chain = collections.Counter()
    for (weight, _, _, _), chain in zip(p.samples, chains):
        if chain and chain[0][0] == func:
            by_chain[' < '.join(f'{f}@{loc or "?"}' for f, loc in chain[:depth])] += weight
    print(f'{func}: {pct(sum(by_chain.values()), p.total):.2f}% of {p.unit}')
    for c, w in by_chain.most_common(12):
        print(f'  {pct(w, p.total):6.2f}%  {c}')


def cmd_contains(args):
    xml, dsym, *wanted = args
    if not wanted:
        die('contains needs at least one NAME')
    p = Profile(xml)
    chains, _, _ = leaf_chains(p, dsym)
    combo = collections.Counter()
    for (weight, _, _, names), chain in zip(p.samples, chains):
        present = set(names) | {f for f, _ in (chain or [])}
        combo[tuple(w for w in wanted if w in present)] += weight
    for w in wanted:
        print(f'  {pct(sum(c for k, c in combo.items() if w in k), p.total):6.2f}%  {w}')
    if len(wanted) > 1:
        print('by combination:')
        for k, c in combo.most_common(12):
            print(f'  {pct(c, p.total):6.2f}%  {" + ".join(k) if k else "(none)"}')


def cmd_inclusive(args):
    top = opt(args, '--top', 30)
    (xml,) = args
    p = Profile(xml)
    incl = collections.Counter()
    for weight, _, _, names in p.samples:
        for n in set(names):
            incl[n] += weight
    print(f'{p.total / 1e9:.2f}G {p.unit}; real frames only, so inlined functions are '
          'counted in their callers (use contains for them)')
    for n, c in incl.most_common(top):
        print(f'  {pct(c, p.total):6.2f}%  {n}')


def cmd_check(args):
    xml, dsym = args
    p = Profile(xml)
    dwarf = dwarf_path(dsym)
    uuid, load = p.binary_for(dwarf)
    addrs = list(dict.fromkeys(a for _, a, u, _ in p.samples if u == uuid and a))
    batched = symbolicate(dwarf, load, addrs, spot_checks=0)
    bad = 0
    for a in addrs:
        alone = atos(dwarf, load, [a])
        if len(alone) != 1 or parse_block(alone[0]) != batched[a]:
            bad += 1
    print(f'{len(addrs)} addresses in the binary; {bad} batched lookups disagree with single ones')
    sys.exit(1 if bad else 0)


def parse_with_refs(xml_path):
    """The export's root and a function that follows an element's ref."""
    root = ET.parse(xml_path).getroot()
    by_id = {el.get('id'): el for el in root.iter() if el.get('id') is not None}

    def deref(el):
        r = el.get('ref')
        return by_id[r] if r is not None else el
    return root, deref


class PmiSamples:
    """The samples of a CPU Counters sampling mode (the pmi-sample table): each
    one's counter and the address of the instruction that triggered it. A
    sample stands for a fixed number of its counter's events, and its address
    is the instruction that caused them: the mispredicted branch or the load
    that missed, not an instruction that waited. The binaries' load addresses
    come from the time-profile table that record.sh exports beside it."""

    def __init__(self, xml_path):
        root, deref = parse_with_refs(xml_path)
        self.samples = []  # (counter, address)
        for row in root.iter('row'):
            values = [deref(c) for c in row]
            numbers = [int(v.text) for v in values if v.tag == 'uint64']
            if len(numbers) != 3:
                die(f'{xml_path}: expected a pmi-sample row with three numbers: '
                    'count, counter and address')
            self.samples.append((numbers[1], numbers[2]))
        if not self.samples:
            die(f'{xml_path}: no pmi samples (export the pmi-sample table)')
        time_path = re.sub(r'\.xml$', '', xml_path) + '.time.xml'
        if not os.path.exists(time_path):
            die(f'{time_path} is missing; record.sh exports it beside {xml_path}')
        time_root, time_deref = parse_with_refs(time_path)
        self.binaries = {}  # UUID -> (name, load address)
        for b in time_root.iter('binary'):
            b = time_deref(b)
            if b.get('UUID') and b.get('load-addr'):
                self.binaries.setdefault(b.get('UUID'), (b.get('name', '?'), b.get('load-addr')))

    binary_for = Profile.binary_for


def text_range(dwarf):
    """The binary's __TEXT segment, unslid, from the dSYM's load commands."""
    out = subprocess.run(['otool', '-l', dwarf], capture_output=True, text=True).stdout
    m = re.search(r'segname __TEXT\n\s+vmaddr (0x[0-9a-f]+)\n\s+vmsize (0x[0-9a-f]+)', out)
    if not m:
        die(f'{dwarf}: no __TEXT segment in its load commands')
    return int(m.group(1), 16), int(m.group(2), 16)


def cmd_pmi(args):
    top = opt(args, '--top', 15)
    only = opt(args, '--counter', -1)
    xml, dsym = args
    p = PmiSamples(xml)
    dwarf = dwarf_path(dsym)
    _, load = p.binary_for(dwarf)
    text_start, text_size = text_range(dwarf)
    slide = int(load, 16) - text_start
    per_counter = collections.Counter(c for c, _ in p.samples)
    print('samples by counter (in a mode\'s trigger order, lowest counter first; '
          'counters_template.py --list names them):')
    for c in sorted(per_counter):
        print(f'  counter {c}: {per_counter[c]}')
    inside = sorted({a for _, a in p.samples if 0 <= a - slide - text_start < text_size})
    chains = symbolicate(dwarf, load, [hex(a) for a in inside])
    for counter in sorted(per_counter):
        if only not in (-1, counter):
            continue
        total = per_counter[counter]
        by_func, by_line, by_pc = collections.Counter(), collections.Counter(), collections.Counter()
        outside = 0
        for c, a in p.samples:
            if c != counter:
                continue
            chain = chains.get(hex(a))
            if not chain:
                outside += 1
                continue
            func, loc = chain[0]
            by_func[func] += 1
            by_line[(func, loc)] += 1
            by_pc[(hex(a - slide), func, loc)] += 1
        print(f'counter {counter}: {total} samples, {pct(outside, total):.1f}% outside the binary')
        print('  by function:')
        for f, n in by_func.most_common(top):
            print(f'    {pct(n, total):6.2f}%  {f}')
        print('  by line:')
        for (f, loc), n in by_line.most_common(top):
            print(f'    {pct(n, total):6.2f}%  {f} ({loc})')
        print('  by instruction (unslid, as llvm-objdump prints it):')
        for (a, f, loc), n in by_pc.most_common(top):
            print(f'    {pct(n, total):6.2f}%  {a}  {f} ({loc})')


def cmd_bottlenecks(args):
    (xml,) = args
    root, deref = parse_with_refs(xml)
    # Each row is one metric over one interval of one process. Instruments
    # averages the metrics weighted by time; their scale in the export isn't
    # documented, so this reports each as a share of the four's sum, weighting
    # each interval by its cycles. Precise rows, where present, are used alone.
    intervals = collections.defaultdict(dict)
    for row in root.iter('row'):
        values = [deref(c) for c in row]
        start = values[0].text
        process = next((v.get('fmt') for v in values if v.tag == 'process'), '?')
        number = next(int(v.text) for v in values if v.tag == 'uint64')
        fraction = next(float(v.text) for v in values if v.tag == 'fixed-decimal')
        name = next(v.text for v in values if v.tag == 'string')
        precise = next((v.text for v in values if v.tag == 'boolean'), '0')
        intervals[(process, precise, start)][name] = number if name == 'cycle' else fraction
    if not intervals:
        die(f'{xml}: no metric rows (export the MetricAggregationForProcess table)')
    processes = collections.Counter(p for p, _, _ in intervals)
    process = processes.most_common(1)[0][0]
    precise = '1' if any(p == process and q == '1' for p, q, _ in intervals) else '0'
    names = ['useful', 'discarded', 'processing', 'delivery']
    sums = dict.fromkeys(names, 0.0)
    for (p, q, _), metrics in intervals.items():
        if p != process or q != precise:
            continue
        for n in names:
            sums[n] += metrics.get(n, 0.0) * metrics.get('cycle', 1)
    total = sum(sums.values())
    print(f'{process}, {"precise" if precise == "1" else "imprecise"} intervals, '
          'shares of issue slots:')
    for n in names:
        print(f'  {pct(sums[n], total):5.1f}%  {n}')


COMMANDS = {'self': cmd_self, 'compare': cmd_compare, 'lines': cmd_lines, 'callers': cmd_callers,
            'contains': cmd_contains, 'inclusive': cmd_inclusive, 'check': cmd_check,
            'pmi': cmd_pmi, 'bottlenecks': cmd_bottlenecks}

if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        sys.exit(__doc__)
    try:
        COMMANDS[sys.argv[1]](sys.argv[2:])
    except ValueError:
        sys.exit(__doc__)
