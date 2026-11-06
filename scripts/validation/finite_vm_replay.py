"""Replay frozen Tigress VM/function images with opt-in finite-dispatch.

Runs the unchanged challenge's specification, dynamic CFG and full Guest-state
oracles, including real Guest megaInit. Never injects initialized data. Input
ELF/source hashes and actually mapped libraries are recorded separately from
results; unresolved paths remain incomplete. Use --baseline with old interfaces.
"""
import argparse,hashlib,io,json,os,shutil,sys
from pathlib import Path
if os.environ.get('DRAGON_TEST_PYTHON'):
    sys.path.insert(0,os.environ['DRAGON_TEST_PYTHON'])
    import dragon
import tigress_vm_challenge as study
d=study.base.dragon

def main():
    parser=argparse.ArgumentParser(add_help=False)
    parser.add_argument('--baseline',action='store_true')
    parser.add_argument('--frozen',type=Path,required=True)
    args,remaining=parser.parse_known_args();sys.argv=[sys.argv[0],*remaining]
    if '--control' in remaining and remaining[remaining.index('--control')+1]=='c-result':
        parser.error('frozen images cannot apply a C source mutation control')
    frozen=args.frozen.expanduser().resolve()
    out=Path(remaining[remaining.index('--out')+1]).resolve();out.mkdir(parents=True,exist_ok=True)
    manifest=[];reports=[];active={}
    original_build=study.build_image;original_run=study.tf.run;original_disassemble=d.LoadedImage.disassemble
    def build_image(source,work,level,configuration):
        profile=source.parent.name;path=frozen/profile/level/'fixture.elf';data=path.read_bytes()
        work.mkdir(parents=True,exist_ok=True);(work/'fixture.elf').write_bytes(data)
        elf=study.ELFFile(io.BytesIO(data));regions=[]
        for section in elf.iter_sections():
            if section['sh_flags']&2 and section['sh_size']:
                start,size=section['sh_addr'],section['sh_size']
                entry=section.name=='.text.entry' and study.ENTRY<=start and start+size<0x2000
                assert entry or study.IMAGE_BASE<=start and start+size<=study.MEM_SIZE
                regions.append((start,bytes(size) if section['sh_type']=='SHT_NOBITS' else section.data()))
        symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()
                 if s['st_info']['type']=='STT_FUNC' and s['st_shndx']!='SHN_UNDEF'}
        assert symbols.get('oracle_entry')==study.ENTRY and 'kernel' in symbols
        digest=hashlib.sha256(data).hexdigest();active.update(case=profile+'-'+level,sha256=digest)
        manifest.append(dict(profile=profile,level=level,path=str(path),sha256=digest,
            source_sha256=hashlib.sha256(source.read_bytes()).hexdigest()))
        return data,regions,symbols
    def run(command,work,tag,*a,**kw):
        if tag=='tigress':
            source=frozen/work.name/'transformed.c'
            shutil.copy2(source,work/'transformed.c')
            (work/'frozen-transform.txt').write_text(str(source)+'\n')
            return 'Frozen transform replay\n'
        return original_run(command,work,tag,*a,**kw)
    def disassemble(image,*a,**kw):
        graph=original_disassemble(image,*a,**kw)
        if not args.baseline:
            entries=[s.address for s in image.symbols() if s.kind==d.SymbolKind.FUNCTION and not s.imported]
            for section in image.sections():
                if not section.executable or not section.data:continue
                with d.PassPipeline() as pipeline:
                    pipeline.register_finite_dispatch(section.data,section.address,entry_points=entries).add('finite-dispatch')
                    report=pipeline.run(graph)
                reports.append(dict(**active,code_base=section.address,report=report))
        return graph
    study.build_image=build_image;study.tf.run=run;d.LoadedImage.disassemble=disassemble
    try:status=study.main()
    finally:
        study.build_image=original_build;study.tf.run=original_run;d.LoadedImage.disassemble=original_disassemble
        mapped={str(Path(d._LIB).resolve())}
        mapped.update(line.split()[-1] for line in Path('/proc/self/maps').read_text().splitlines()
                      if '/libdragon' in line and line.split()[-1].startswith('/'))
        campaign=dict(baseline=args.baseline,frozen=str(frozen),inputs=manifest,reports=reports,
            memory_bytes=study.MEM_SIZE,harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            libraries={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in sorted(mapped)})
        (out/'finite-replay.json').write_text(json.dumps(campaign,indent=2))
    return status
if __name__=='__main__':raise SystemExit(main())
