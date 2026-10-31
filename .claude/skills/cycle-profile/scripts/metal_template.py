#!/usr/bin/env python3
"""Write an xctrace template that records Metal System Trace with Instruments'
shader profiler on (for gpu.py shaders) and, optionally, a GPU counter set.

usage: metal_template.py OUT.tracetemplate [--counters SET]
       metal_template.py --list

Instruments chooses these only in its UI, and xctrace has no option for
them, so this edits a copy of the installed Xcode's own Metal System Trace
template: shaderprofiler on, and with --counters, counterprofile set to the
set's number and counterdevice to this Mac's GPU (its IORegistry id, which
the trace's device-gpu-info table shows as accelerator-id).

The sets' numbers come from Instruments' own table
(-[XRGPUTracingSetupElement profileNameFromCounterProfile:], Xcode 27.2).
A trace records the set it used: xctrace export --toc shows "Counter Set:".
On an M5 Max (AGXAcceleratorG17X) with Xcode 27.2, every set is accepted but
xctrace samples only "RT Unit Active"; see SKILL.md section 6.
"""
import os
import plistlib
import re
import subprocess
import sys

COUNTER_SETS = {
    'Raw Limiters': 2,
    'Performance Limiters': 3,
    'Performance Utilizations': 4,
    'Render Basic': 5,
    'Compute Basic': 6,
    'Rasterizer and Pixel Backend': 7,
    'Memory 1': 8,
    'Memory 2': 9,
    'Memory 3': 10,
    'Sampler': 11,
    'Raytracing Limiters': 14,
}


def die(msg):
    sys.exit(f'metal_template.py: {msg}')


def gpu_registry_id():
    out = subprocess.run(['ioreg', '-r', '-c', 'IOAccelerator', '-d', '1'],
                         capture_output=True, text=True).stdout
    ids = re.findall(r'<class \w+, id (0x[0-9a-f]+)', out)
    if len(ids) != 1:
        die(f'expected one GPU in ioreg, found {len(ids)}')
    return int(ids[0], 16)


def main():
    if sys.argv[1:] == ['--list']:
        for name, number in COUNTER_SETS.items():
            print(f'{number:3}  {name}')
        return
    if len(sys.argv) not in (2, 4) or (len(sys.argv) == 4 and sys.argv[2] != '--counters'):
        sys.exit(__doc__)
    counters = None
    if len(sys.argv) == 4:
        if sys.argv[3] not in COUNTER_SETS:
            die(f'unknown counter set {sys.argv[3]!r}; --list shows them')
        counters = COUNTER_SETS[sys.argv[3]]
    out = subprocess.run(['xcode-select', '-p'], capture_output=True, text=True)
    if out.returncode != 0:
        die('xcode-select -p failed; is Xcode installed?')
    stock = os.path.join(out.stdout.strip(), '..', 'Applications', 'Instruments.app', 'Contents',
                         'Packages', 'GPU.instrdst', 'Contents', 'Templates',
                         'Metal System Trace.tracetemplate')
    if not os.path.exists(stock):
        die(f'no Metal System Trace template at {stock}')
    archive = plistlib.load(open(stock, 'rb'))
    objects = archive['$objects']

    def resolve(uid):
        value = objects[uid.data] if isinstance(uid, plistlib.UID) else uid
        return value['NS.string'] if isinstance(value, dict) and 'NS.string' in value else value

    def put(value):
        objects.append(value)
        return plistlib.UID(len(objects) - 1)

    settings = {'shaderprofiler': True, 'shaderprofilerinternal': True}
    if counters is not None:
        device = gpu_registry_id()
        settings.update(counterprofile=counters, counterprofileinternal=counters,
                        counterdevice=device, counterdeviceinternal=device)
    changed = set()
    for obj in objects:
        if not (isinstance(obj, dict) and 'NS.keys' in obj):
            continue
        for idx, key in enumerate(obj['NS.keys']):
            name = resolve(key)
            if name in settings:
                obj['NS.objects'][idx] = put(settings[name])
                changed.add(name)
    if changed != set(settings):
        die(f'{stock} lacks {sorted(set(settings) - changed)}; has the template changed?')
    plistlib.dump(archive, open(sys.argv[1], 'wb'), fmt=plistlib.FMT_BINARY)
    print(sys.argv[1])


if __name__ == '__main__':
    main()
