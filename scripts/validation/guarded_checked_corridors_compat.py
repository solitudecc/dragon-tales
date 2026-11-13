#!/usr/bin/env python3
"""Exact prepared main/no-Z3 IR and report comparison, with byte controls."""
import argparse
import hashlib
import json
from pathlib import Path

from guarded_checked_corridors_oracle import MODES


def main(root):
    left, right = root/'main', root/'noz3'
    output = root/'byte-compat'
    output.mkdir(exist_ok=False)
    paths = {p.relative_to(left) for p in left.rglob('*.ll')}
    assert paths == {p.relative_to(right) for p in right.rglob('*.ll')}
    reports = {p.relative_to(left) for p in left.rglob('*.json')
               if p.stem in MODES and not p.name.endswith('.command.json')}
    assert reports == {p.relative_to(right) for p in right.rglob('*.json')
                       if p.stem in MODES and not p.name.endswith('.command.json')}
    rows, controls = [], 0
    for relative in sorted(paths | reports):
        a, b = (left/relative).read_bytes(), (right/relative).read_bytes()
        assert a == b, ('main/no-Z3 raw bytes differ', str(relative))
        rows.append(dict(path=str(relative), size=len(a), sha256=hashlib.sha256(a).hexdigest()))
        if relative in paths:
            changed = bytearray(b)
            changed[len(changed)//2] ^= 1
            assert a != changed
            controls += 1
    assert len(paths) == controls and len(reports) == 18
    record = dict(identical_ir_files=len(paths), identical_reports=len(reports),
                  damaged_ir_byte_controls=controls, rows=rows,
                  normalization=False, native_executions=0)
    (output/'RESULT.json').write_text(json.dumps(record, indent=2)+'\n')
    print(json.dumps({k:v for k,v in record.items() if k != 'rows'}))


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('root',type=Path)
    main(parser.parse_args().root)
