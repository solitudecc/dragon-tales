"""Tigress Flatten goto/indirect using the unchanged C/Unicorn/native oracle.

Optional --inline-helper performs one explicit source substitution equivalent
to helper(y,x), removing the need for interprocedural effects in that campaign.
The original source campaign remains separate. No comparison field is removed.
--reuse-inputs replays the exact ELF bytes and transformed C of a prior run.
Unknown dispatches remain incomplete; this script does not count them as passes.
"""
import argparse,hashlib,io,json,os,shutil,sys
from pathlib import Path
if os.environ.get('DRAGON_TEST_PYTHON'):
    sys.path.insert(0,os.environ['DRAGON_TEST_PYTHON'])
    import dragon
import tigress_function_diff as study
from elftools.elf.elffile import ELFFile
d=study.base.dragon

def main():
    p=argparse.ArgumentParser(add_help=False)
    p.add_argument('--baseline',action='store_true')
    p.add_argument('--inline-helper',action='store_true')
    p.add_argument('--gcc-compile',action='store_true',
                   help='compile Guest C with x86_64-linux-gnu-gcc instead of Clang')
    p.add_argument('--max-values',type=int,default=64)
    p.add_argument('--max-operations',type=int,default=1000000)
    p.add_argument('--reuse-inputs',type=Path)
    args,remaining=p.parse_known_args()
    if args.gcc_compile and '--clang' not in remaining:
        remaining+=['--clang','x86_64-linux-gnu-gcc']
    sys.argv=[sys.argv[0],*remaining]
    out=Path(remaining[remaining.index('--out')+1]).resolve();out.mkdir(parents=True,exist_ok=True)
    os.environ.setdefault('CILLY_NATIVE_CC','x86_64-linux-gnu-gcc')
    variant='explicitly-inlined-helper' if args.inline_helper else 'original-helper-call'
    compiler_family='gnu-gcc' if args.gcc_compile else 'clang'
    if args.reuse_inputs:
        args.reuse_inputs=args.reuse_inputs.resolve()
        previous=json.loads((args.reuse_inputs/'finite-campaign.json').read_text())
        assert previous['source_variant']==variant
        assert previous.get('compiler_family','clang')==compiler_family
        assert '--control' not in remaining, 'source mutation controls need fresh inputs'
    reports=[];inputs={};active={}
    source=study.source;build=study.build_image;run=study.run;compile_image=study.compile_image
    def source_variant(seed,wrong=False):
        text=source(seed,wrong)
        if args.inline_helper:
            assert text.count('out[1]=helper(y,x);')==1
            text=text.replace('out[1]=helper(y,x);','out[1]=(y+x)^(y*9u);')
        return text
    def frozen_build(c_source,work,level,configuration):
        work=Path(work)
        if args.reuse_inputs:
            original=args.reuse_inputs/work.relative_to(out)/'fixture.elf'
            work.mkdir(parents=True,exist_ok=True)
            data=original.read_bytes()
            assert hashlib.sha256(data).hexdigest()==previous['inputs'][str(work.relative_to(out))]
            (work/'fixture.elf').write_bytes(data)
            elf=ELFFile(io.BytesIO(data));regions=[]
            for section in elf.iter_sections():
                if section['sh_flags']&2 and section['sh_size']:
                    regions.append((section['sh_addr'],bytes(section['sh_size']) if section['sh_type']=='SHT_NOBITS' else section.data()))
            symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()
                     if s['st_info']['type']=='STT_FUNC' and s['st_shndx']!='SHN_UNDEF'}
            result=data,regions,symbols
        else:result=build(c_source,work,level,configuration)
        inputs[str(work.relative_to(out))]=hashlib.sha256(result[0]).hexdigest()
        return result
    def frozen_run(command,work,tag,*a,**kw):
        if args.reuse_inputs and tag=='tigress':
            original=args.reuse_inputs/Path(work).relative_to(out)/'transformed.c'
            shutil.copy2(original,Path(work)/'transformed.c')
            (Path(work)/'frozen-transform.txt').write_text(str(original)+'\n')
            return 'Frozen transformed C replay\n'
        if args.gcc_compile:
            command=[x for x in command if not str(x).startswith('--target=')]
        return run(command,work,tag,*a,**kw)
    original_disassemble=d.LoadedImage.disassemble
    def disassemble(image,*a,**kw):
        graph=original_disassemble(image,*a,**kw)
        if not args.baseline:
            entries=[s.address for s in image.symbols()
                     if s.kind==d.SymbolKind.FUNCTION and not s.imported]
            for section in image.sections():
                if not section.executable or not section.data:continue
                with d.PassPipeline() as pipeline:
                    pipeline.register_finite_dispatch(section.data,section.address,entry_points=entries).add(
                        'finite-dispatch',max_values=args.max_values,max_operations=args.max_operations)
                    report=pipeline.run(graph)
                    reports.append(dict(case=active.get('case'),input_sha256=active.get('sha256'),
                                        code_base=section.address,report=report))
        return graph
    def compile_with_passes(data,symbols,work,*a,**kw):
        active.update(case=str(Path(work).relative_to(out)),sha256=hashlib.sha256(data).hexdigest())
        return compile_image(data,symbols,work,*a,**kw)
    study.source=source_variant;study.build_image=frozen_build;study.run=frozen_run
    study.compile_image=compile_with_passes;d.LoadedImage.disassemble=disassemble
    try:status=study.main()
    finally:
        study.source=source;study.build_image=build;study.run=run
        study.compile_image=compile_image;d.LoadedImage.disassemble=original_disassemble
        campaign=dict(baseline=args.baseline,source_variant=variant,compiler_family=compiler_family,
            pass_limits=dict(max_values=args.max_values,max_operations=args.max_operations),
            inputs=inputs,reports=reports,
            reuse_inputs=str(args.reuse_inputs) if args.reuse_inputs else None)
        (out/'finite-campaign.json').write_text(json.dumps(campaign,indent=2))
    return status
if __name__=='__main__':raise SystemExit(main())
