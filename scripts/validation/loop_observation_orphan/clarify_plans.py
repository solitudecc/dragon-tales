"""Reproduce the executed v2 authority wording without changing commands/pins.

--check-only compares in memory to the frozen plans; it never overwrites them.
"""
import argparse
import hashlib
import json
from pathlib import Path

def sha(raw):return hashlib.sha256(raw).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root',type=Path)
    parser.add_argument('--check-only',action='store_true')
    args=parser.parse_args();plans={}
    for mode in ['good','mutant']:
        old=args.root/'plans'/(mode+'.json');new=args.root/'plans'/(mode+'-v2.json')
        data=json.loads(old.read_bytes())
        data['root_GO']='isolated orphan unit CPU4-7 after bit released NZbuild, concurrently with main CTest CPU0-3 explicitly authorized; no second CTest suite; original 180/512/64'
        data['authorization_clarification']={
          'old_plan_sha256':sha(old.read_bytes()),
          'reason':'clarify ambiguous no-concurrent-ctest wording before any target; one main suite on disjoint cores is explicitly authorized',
          'source_commands_pins_guards_budgets_unchanged':True,'old_plan_target_calls':0}
        payload=(json.dumps(data,sort_keys=True,indent=2)+'\n').encode()
        if args.check_only:assert new.read_bytes()==payload
        else:
            assert not new.exists();new.write_bytes(payload)
        plans[mode]=dict(path=str(new),sha256=sha(payload))
    ready=args.root/'READY-v2.json'
    payload=(json.dumps(dict(schema='orphan-policy-ready-v2',plans=plans,
      source_commands_pins_guards_unchanged=True,target_calls=0),indent=2,sort_keys=True)+'\n').encode()
    if args.check_only:assert ready.read_bytes()==payload
    else:
        assert not ready.exists();ready.write_bytes(payload)
    print(json.dumps(dict(exact_plan_bytes_verified=args.check_only,
      ready_sha256=sha(payload),plans=plans),indent=2))
if __name__=='__main__':main()
