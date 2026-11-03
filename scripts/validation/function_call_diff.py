"""Execute x64 CALL boundaries and the complete root Guest RET.

External C models implement the exact fixture guest instructions and Guest
RET over the same state/memory; Unicorn executes their machine bytes. No
Legacy PC/SP/return-slot normalizations. All native snapshots remain checked.
"""
import function_dispatch_diff as dispatch
base=dispatch.base

def model(stub):
    address=stub['address'];name=f'model_{address:x}'
    code=[f'void {name}(S*s,M*m,C*c,E*e){{++calls{address:x};unsigned char*r=s->regs_bytes;']
    if 'rax' in stub:code.append(f'put64(r,{stub["rax"]}ULL);')
    for name,value in stub.get('gpr_writes',{}).items():code.append(f'put64(r+{base.GPRS.index(name)*8},{value}ULL);')
    if 'copy_gpr' in stub:
        dst,src,width=stub['copy_gpr'];code.append(f'put64(r+{base.GPRS.index(dst)*8},get64(r+{base.GPRS.index(src)*8})'+ ('&0xffffffffULL' if width==32 else '')+');')
    if 'set_cf' in stub:code.append(f'r[136]={stub["set_cf"]};')
    if 'store' in stub:
        code.append(f'uint64_t at=get64(r+24);uint32_t v={stub["store"]}u;if(m->size<4 || at>m->size-4){{*e=(E){{5,0,0,0,0,0,3}};return;}}memcpy(m->data+at,&v,4);')
    if 'copy_xmm' in stub:
        dst,src=stub['copy_xmm'];code.append(f'memcpy(r+{143+8*dst},r+{143+8*src},8);memcpy(r+{271+8*dst},r+{271+8*src},8);')
    if 'fill_xmm' in stub:
        n=stub['fill_xmm'];code.append(f'memset(r+{143+8*n},255,8);memset(r+{271+8*n},255,8);')
    if stub.get('x87_pi'):
        code.append('r[480]=7;put16(r+482,0x3fff);put16(r+484,0x37f);put16(r+486,0x3800);put64(r+470,0xc90fdaa22168c235ULL);put16(r+478,0x4000);')
    if stub.get('x87_pop_store'):
        code.append('uint64_t at=get64(r+24);unsigned top=r[480]&7;uint16_t tag=get16(r+482);if(at>m->size-10 || ((tag>>(2*top))&3)==3){*e=(E){5,0,0,0,0,0,2};return;}memcpy(m->data+at,r+400+10*top,10);put16(r+482,tag|(3u<<(2*top)));r[480]=(top+1)&7;put16(r+486,(get16(r+486)&~(0x3800|0x200))|((uint16_t)r[480]<<11));')
    if stub.get('load_rax32'):
        code.append('uint64_t at=get64(r+24);uint32_t v;if(at>m->size-4){*e=(E){5,0,0,0,0,0,3};return;}memcpy(&v,m->data+at,4);put64(r,v);')
    ret_pc=address+len(bytes.fromhex(stub['code_hex']))-1
    code.append(f'uint64_t sp=get64(r+32);if(m->size<8 || sp>m->size-8){{put64(r+128,{ret_pc});*e=(E){{5,3,{ret_pc},{ret_pc},1,0,3}};return;}}uint64_t next=get64(m->data+sp);put64(r+32,sp+8);put64(r+128,next);*e=(E){{1,3,{ret_pc},next,1,0,0}};}}')
    return '\n'.join(code)

if __name__=='__main__':
    raise SystemExit(dispatch.main(fixtures_path=base.ROOT/'tests/fixtures/llvm-call-state.json',model_factory=model,stop_before_calls=False,full_guest=True))
