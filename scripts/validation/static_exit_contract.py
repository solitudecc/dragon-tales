"""Static call Exit protocol checks; not a reference for whole-callee ISA semantics.

Unicorn supplies the precise pre-callee machine snapshot (including CALL stack
effects). A C stub supplies each documented Exit/state change after opt has
run. We independently apply those changes to the snapshot and compare every
modelled register/memory byte, then check whether the caller continued.
"""
import argparse,collections,json,struct
from pathlib import Path
import function_dispatch_diff as dispatch
base=dispatch.base

# name, reason, kind, PC, valid, mirror; only exact Return/Return to 0x1007 resumes.
VARIANTS=[('normal',1,3,0x1007,1,0x1007),('redirect',1,3,0x3000,1,0x3000),
 ('branch',2,1,0x3000,1,0x3000),('call',2,2,0x3000,1,0x3000),
 ('need',3,2,0x3000,1,0x3000),('unknown',4,2,0x3000,0,0x3000),
 ('unsupported',5,0,0x3000,1,0x3000),('budget',6,0,0x3000,1,0x3000),
 ('return_wrong_kind',1,1,0x1007,1,0x1007),
 ('bad_zero_reason',0,0,0x3000,1,0x3000),('bad_reason',7,0,0x3000,1,0x3000),
 ('bad_kind',1,9,0x1007,1,0x1007),('bad_valid',1,3,0x1007,2,0x1007),
 ('bad_return_pc',1,3,0x3000,0,0x3000),('bad_transfer_pc',2,1,0x3000,0,0x3000),
 ('bad_need_pc',3,2,0x3000,0,0x3000),('bad_unknown_pc',4,2,0x3000,1,0x3000),
 ('bad_mirror',1,3,0x1007,1,0x9999)]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--trials',type=int,default=8);p.add_argument('--opt',default='opt-21');p.add_argument('--llc',default='llc-21');p.add_argument('--cc',default='gcc');args=p.parse_args()
    if args.trials<1:p.error('positive trials required')
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True);rows=[]
    fixture=dict(id='exit_contract',code_hex='31c0e8f90f0000c70307000000c3',undefined_flags=['AF'],stubs=[dict(address=0x2000,code_hex='c3')])
    prefix=dict(fixture,stubs=[],missing_targets=[0x2000])
    for name,reason,kind,pc,valid,mirror in VARIANTS:
      def model(stub):
        return f'''void model_2000(S*s,M*m,C*c,E*e){{
          ++calls2000;
          if(e->reason!=2 || e->transfer_kind!=2 || e->source_pc!=0x1002 ||
             e->next_pc!=0x2000 || e->next_pc_valid!=1 || e->return_pc!=0x1007 || e->detail!=0)__builtin_trap();
          put64(s->regs_bytes,0x8877665544332211ULL);put64(s->regs_bytes+120,0x123456789abcdef0ULL);
          m->data[0x8010]=0x5a;put64(s->regs_bytes+128,{pc});
          {'put64(s->regs_bytes+32,get64(s->regs_bytes+32)+8);' if name=='normal' else ''}
          *e=(E){{{reason},{kind},0x2000,{mirror},{valid},0,0}};
        }}'''
      for level in ('O0','O2'):
        work=out/name/level;work.mkdir(parents=True,exist_ok=True)
        try:
          exe,bad_ir,sites=dispatch.compile_case(fixture,0x1000,level,work,args,model,static_analysis=True)
          for trial in range(args.trials):
            regs,memory=base.initial(fixture,trial,0x1000)
            expected=base.reference(prefix,regs,memory,0x1000,sites,stop_before_control=True,stop_before_calls=False,full_guest=True)
            want_regs=bytearray(expected[0]);want_mem=bytearray(expected[1])
            base.put(want_regs,0,0x8877665544332211);base.put(want_regs,120,0x123456789abcdef0);base.put(want_regs,128,pc);want_mem[0x8010]=0x5a
            want_exit=(reason,kind,0x2000,mirror,valid,0,0)
            if name=='normal':
              base.put(want_mem,0x8000,7,4);base.put(want_regs,32,base.STACK+8);base.put(want_regs,128,base.SENTINEL)
              want_exit=(1,3,0x100d,base.SENTINEL,1,0,0)
            elif name.startswith('bad_'):want_exit=(5,0,pc,pc,1,0,4)
            want=(bytes(want_regs),bytes(want_mem),collections.Counter({0x2000:1}),set(),want_exit)
            actual,error=base.native(exe,regs,memory,work,trial,extra_bytes=64)
            row=dict(case=name,opt=level,trial=trial)
            if error:row.update(status=error['kind'],detail=error)
            else:
              diffs=base.compare(fixture,want,actual,bad_ir,False,include_pc=True)
              record=struct.unpack('<8Q',actual[3])
              if record[:7]!=want_exit:diffs.append(dict(field='exit',got=record[:7],expected=want_exit))
              if record[7]!=0:diffs.append(dict(field='budget',got=record[7],expected=0))
              row.update(status='different' if diffs else 'match',differences=diffs,register_fields_compared=len(base.FIELDS),memory_bytes_compared=base.MEM_SIZE)
            rows.append(row)
        except Exception as error:
          detail=getattr(error,'stderr',None)
          rows.append(dict(case=name,opt=level,status='infrastructure_error',detail=detail.decode(errors='replace') if isinstance(detail,bytes) else str(error)))
    # A non-returning opaque implementation must still be called after O2.
    # There is intentionally no output snapshot: _Exit(42) terminates the
    # child before the caller's continuation. This is a protocol check only.
    for level in ('O0','O2'):
      work=out/'noreturn'/level;work.mkdir(parents=True,exist_ok=True)
      try:
        model=lambda stub:'#include <stdlib.h>\nvoid model_2000(S*s,M*m,C*c,E*e){_Exit(42);}'
        exe,_,_=dispatch.compile_case(fixture,0x1000,level,work,args,model,static_analysis=True)
        for trial in range(args.trials):
          regs,memory=base.initial(fixture,trial,0x1000)
          actual,error=base.native(exe,regs,memory,work,trial,extra_bytes=64)
          correct=error is not None and error.get('kind')=='native_exit' and error.get('returncode')==42 and not (work/f'output-{trial}.bin').exists()
          rows.append(dict(case='noreturn',opt=level,trial=trial,status='match' if correct else 'different',observed=error))
      except Exception as error:
        rows.append(dict(case='noreturn',opt=level,status='infrastructure_error',detail=str(error)))
    summary=dict(cases=len(VARIANTS)+1,results=len(rows),statuses=dict(collections.Counter(r['status'] for r in rows)),skipped=0,
      stage='Exit protocol over Unicorn pre-callee state; not whole-callee ISA',
      register_fields_compared=sum(r.get('register_fields_compared',0) for r in rows),memory_bytes_compared=sum(r.get('memory_bytes_compared',0) for r in rows))
    (out/'results.json').write_text(json.dumps(dict(summary=summary,results=rows),indent=2));print(json.dumps(summary,indent=2))
    for row in rows:
      if row['status']!='match':print('FAIL',json.dumps(row))
    return 0 if len(rows)==(len(VARIANTS)+1)*args.trials*2 and all(r['status']=='match' for r in rows) else 1

if __name__=='__main__':raise SystemExit(main())
