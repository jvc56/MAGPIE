#!/usr/bin/env python3
"""GPU time from a Metal System Trace recorded by record.sh.

usage: gpu.py busy OUT.gpu.xml PROCESS [--skip SECONDS]
       gpu.py shaders OUT.shaders.xml PROCESS [--skip SECONDS] [--top N]

busy     the process's GPU time from the metal-gpu-intervals table: the union
         of its intervals by channel (Compute for MPSGraph and compute
         kernels) over the window from its first interval, after --skip
         seconds, to its last; and the idle gaps between them. This is the
         GPU actually executing, not the time callers spend inside a
         synchronous call such as MPSGraph's runWithMTLCommandQueue, which
         also counts encoding on the CPU and waiting.
shaders  GPU time by shader (kernel) from metal-shader-profiler-intervals,
         which a template from metal_template.py records. MPSGraph puts a
         whole graph in one compute encoder, so encoder labels don't name
         kernels; this does (gemm_*, sdpa_*, reduce_*, ...).

PROCESS is the start of the process name as Instruments shows it (the
binary's file name). --skip leaves out the start, such as graph compilation.
"""
import sys
import xml.etree.ElementTree as ET
from collections import Counter


def die(msg):
    sys.exit(f'gpu.py: {msg}')


def rows(path):
    """Each row of an xctrace table export as {column: (fmt, text)}, with
    xctrace's id/ref compression resolved."""
    seen = {}
    columns = None
    for _, elem in ET.iterparse(path, events=('end',)):
        if elem.tag == 'schema':
            columns = [col.findtext('mnemonic') for col in elem.findall('col')]
            continue
        if elem.tag != 'row':
            continue
        values = {}
        for column, child in zip(columns, list(elem)):
            ref = child.get('ref')
            if ref is not None:
                values[column] = seen[ref]
                continue
            for sub in child.iter():
                if sub.get('id') is not None:
                    seen[sub.get('id')] = (sub.get('fmt', ''), (sub.text or '').strip())
            values[column] = (child.get('fmt', ''), (child.text or '').strip())
        elem.clear()
        yield values


def union(intervals):
    total, end = 0, None
    for start, stop in sorted(intervals):
        if end is None or start > end:
            total += stop - start
            end = stop
        elif stop > end:
            total += stop - end
            end = stop
    return total


def option(name, default):
    return type(default)(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default


def process_intervals(path, process):
    out = []
    for row in rows(path):
        if row.get('process', ('', ''))[0].startswith(process):
            start = int(row['start'][1])
            out.append((row, start, start + int(row['duration'][1])))
    if not out:
        die(f'no intervals of a process named {process}* in {path}')
    return out


def busy(path, process, skip):
    by_channel = {}
    intervals = process_intervals(path, process)
    first = min(start for _, start, _ in intervals) + int(skip * 1e9)
    last = max(stop for _, _, stop in intervals)
    for row, start, stop in intervals:
        if stop > first:
            by_channel.setdefault(row['channel-name'][0], []).append((max(start, first), stop))
    window = last - first
    print(f'window {window / 1e9:.3f} s')
    for name, ivs in sorted(by_channel.items(), key=lambda kv: -union(kv[1])):
        time = union(ivs)
        print(f'  {name:<10} {len(ivs):>8} intervals  busy {time / 1e9:8.3f} s '
              f'{100 * time / window:5.1f}%')
    merged = sorted(iv for ivs in by_channel.values() for iv in ivs)
    print(f'  any channel busy {union(merged) / 1e9:.3f} s, '
          f'{100 * union(merged) / window:.1f}% of the window')
    gaps, end = [], None
    for start, stop in merged:
        if end is not None and start > end:
            gaps.append(start - end)
        end = stop if end is None else max(end, stop)
    if gaps:
        gaps.sort()
        q = lambda f: gaps[min(len(gaps) - 1, int(f * len(gaps)))] / 1e3
        print(f'  idle gaps: {len(gaps)}, median {q(0.5):.1f} us, p90 {q(0.9):.1f} us, '
              f'total {sum(gaps) / 1e9:.3f} s')


def shaders(path, process, skip, top):
    intervals = process_intervals(path, process)
    first = min(start for _, start, _ in intervals) + int(skip * 1e9)
    time, count = Counter(), Counter()
    for row, start, stop in intervals:
        if start < first:
            continue
        # "gemm_a18 (27)": the number is the pipeline; group by kernel.
        name = (row['name'][0] or row['label'][0]).split(' (')[0]
        time[name] += stop - start
        count[name] += 1
    total = sum(time.values())
    if total == 0:
        die('no shader intervals; record with a metal_template.py template')
    print(f'{total / 1e9:.3f} s of shader time (shares, not wall time: kernels overlap)')
    for name, value in time.most_common(top):
        print(f'  {100 * value / total:5.1f}%  {count[name]:>7} x {value / count[name] / 1e3:7.1f} us'
              f'  {name}')


def main():
    if len(sys.argv) < 4 or sys.argv[1] not in ('busy', 'shaders'):
        sys.exit(__doc__)
    if sys.argv[1] == 'busy':
        busy(sys.argv[2], sys.argv[3], option('--skip', 0.0))
    else:
        shaders(sys.argv[2], sys.argv[3], option('--skip', 0.0), option('--top', 15))


if __name__ == '__main__':
    main()
