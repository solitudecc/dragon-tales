#!/usr/bin/env python3
"""Recheck frozen byte streams and report rows without executing native code."""
import collections
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(root / 'sources'))
import guest_edge_worklist_diff as model

report = json.loads((root / 'report.json').read_text())
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
for name, expected in report['artifact_sha256'].items():
    assert digest(root / name) == expected, name
for name, expected in report['source_sha256'].items():
    assert digest(root / 'sources' / name) == expected, name
rows = collections.defaultdict(list)
for row in report['rows']:
    label = ('control-' + row['control'] if 'control' in row else row['stage']) + '-' + row['level']
    rows[label].append(row)
assert len(rows) == 22
normal = faults = required = detected = 0
for label, stream_rows in rows.items():
    control = stream_rows[0].get('control')
    keys = (list(model.INPUTS) if control is None else
            [k for k in model.INPUTS if k[0] == model.CONTROL_TARGETS[control]])
    assert [tuple(r['key']) for r in stream_rows] == keys, label
    with gzip.open(root / (label + '.bin.gz'), 'rb') as stream:
        for key, row in zip(keys, stream_rows):
            actual = stream.read(model.SNAPSHOT_BYTES)
            expected = model.model(key, control)
            assert actual == expected, (label, key)
            assert hashlib.sha256(actual).hexdigest() == row['snapshot_sha256']
            fields = model.differences(actual, model.model(key))
            assert fields == row['fields'], (label, key)
            if control:
                faults += 1
                required += bool(model.differences(expected, model.model(key)))
                detected += bool(fields)
            else:
                normal += 1
        assert stream.read(1) == b'', label
with gzip.open(root / 'expected.bin.gz', 'rb') as stream:
    for key in model.INPUTS:
        assert stream.read(model.SNAPSHOT_BYTES) == model.model(key)
    assert stream.read(1) == b''
assert (normal, faults, required, detected) == (2280, 2220, 776, 776)
assert report['semantic_ok'] and report['coverage_ok'] and report['rejection_ok']
assert report['normal_mismatches'] == 0
assert not any(report[k] for k in ('excluded', 'skipped', 'normalized', 'errors'))
result = dict(ok=True, streams=22, expected_streams=1, normal=normal,
              fault_observations=faults, required=required, detected=detected,
              artifact_count=len(report['artifact_sha256']), source_sha256=report['source_sha256'],
              library_sha256=report['library_sha256'], overall_ok=report['ok'], precision_ok=report['precision_ok'])
print(json.dumps(result, sort_keys=True))
