#!/usr/bin/env python3
"""Write an xctrace template that records CPU Counters in one guided mode.

usage: counters_template.py MODE OUT.tracetemplate
       counters_template.py --list

The modes are Instruments' guided CPU Counters modes for this Mac's chip:
  bottlenecks         shares of the CPU's issue slots: useful work, discarded
                      work (mostly mispredicted branches), and processing and
                      delivery stalls. Counts only; no samples.
  discarded_sampling  samples the instructions that caused discarded work:
                      mispredicted branches and memory-order flushes.
  l1d_miss_sampling   samples the instructions that miss in the L1 data cache
                      or its TLB.
--list prints the modes this chip supports, with the events each one samples.

Instruments chooses a mode only in its UI, and xctrace has no option for it,
so this edits a copy of the installed Xcode's own CPU Counters template.
"""
import json
import os
import plistlib
import re
import subprocess
import sys


def die(msg):
    sys.exit(f'counters_template.py: {msg}')


def developer_dir():
    out = subprocess.run(['xcode-select', '-p'], capture_output=True, text=True)
    if out.returncode != 0:
        die('xcode-select -p failed; is Xcode installed?')
    return out.stdout.strip()


def stock_template():
    path = os.path.join(developer_dir(), '..', 'Applications', 'Instruments.app', 'Contents',
                        'Resources', 'templates', 'CPU Counters.tracetemplate')
    if not os.path.exists(path):
        die(f'no CPU Counters template at {path}')
    return path


def chip_modes():
    """The guided modes of this chip: name -> (display name, triggers, synopsis)."""
    path = os.path.join(developer_dir(), '..', 'SharedFrameworks', 'RecountDT.framework',
                        'Versions', 'A', 'Resources', 'Analysis', 'bottleneck.json')
    analysis = json.load(open(path))
    io = subprocess.run(['ioreg', '-l'], capture_output=True, text=True).stdout
    chips = set(re.findall(r'"compatible" = <"arm-io,(t\d+)"', io))
    supported = None
    for platform in analysis['platforms']:
        if chips & set(platform['platforms']):
            supported = set(platform['supported_mode_names'])
    if supported is None:
        die(f'chip {chips or "?"} is not in {path}')
    return {m['name']: (m['display_name'], [t['metric'] for t in m['triggers']], m['synopsis'])
            for m in analysis['modes'] if m['name'] in supported}


def write_template(mode, display_name, out_path):
    archive = plistlib.load(open(stock_template(), 'rb'))
    objects = archive['$objects']

    def deref(x):
        return objects[x.data] if isinstance(x, plistlib.UID) else x

    edited = 0
    for obj in objects:
        if not (isinstance(obj, dict) and 'NS.keys' in obj):
            continue
        keys = [deref(k) for k in obj['NS.keys']]
        if 'optionsEncoded' not in keys:
            continue
        uid = obj['NS.objects'][keys.index('optionsEncoded')]
        holder = objects[uid.data]
        data = holder['NS.data'] if isinstance(holder, dict) else holder
        options = json.loads(data)
        if 'selectedCountingMode' not in options:
            continue
        options['configurationType'] = {'guided': {}}
        options['selectedCountingMode'] = {'analysisMode': 'bottleneck', 'countingMode': mode}
        options['selectedCountingModeDisplayName'] = display_name
        encoded = json.dumps(options, separators=(',', ':')).encode()
        if isinstance(holder, dict):
            holder['NS.data'] = encoded
        else:
            objects[uid.data] = encoded
        edited += 1
    if edited != 1:
        die(f'expected one CPU Counters options entry in the stock template, found {edited}')
    with open(out_path, 'wb') as f:
        plistlib.dump(archive, f, fmt=plistlib.FMT_BINARY)


def main(args):
    modes = chip_modes()
    if args == ['--list']:
        for name, (display, triggers, synopsis) in modes.items():
            print(f'{name}: {display}. {synopsis}')
            if triggers:
                print(f'    samples, by counter from lowest: {", ".join(triggers)}')
        return
    if len(args) != 2:
        sys.exit(__doc__)
    mode, out_path = args
    if mode not in modes:
        die(f'{mode} is not a mode of this chip; choose from {", ".join(modes)}')
    write_template(mode, modes[mode][0], out_path)
    print(out_path)


if __name__ == '__main__':
    main(sys.argv[1:])
