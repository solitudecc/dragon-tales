"""Build small valid ELF/PE import fixtures for the execution oracle.

Containers supply root/local machine bytes and import-slot identity. External
fixture stubs are separately mapped in both Unicorn and Native Guest memory.
No relocated host address or host library call is used.
"""
import re,struct
from pathlib import Path

def make_image(fixture,entry,root):
    def blob(name):
        source=(root/'tests/loader/fixtures/fixtures.hpp').read_text()
        body=re.search(r'static const unsigned char '+name+r'\[\] = \{([^}]*)\}',source,re.S)[1]
        return bytearray(int(n,16) for n in re.findall(r'0x([0-9a-fA-F]{2})',body))
    def get(b,off,n):return int.from_bytes(b[off:off+n],'little')
    def put(b,off,value,n):b[off:off+n]=value.to_bytes(n,'little')
    regions=[dict(address=entry,code_hex=fixture['code_hex'])]+fixture.get('local_functions',[])
    end=max(r['address']+len(bytes.fromhex(r['code_hex'])) for r in regions)
    payload=bytearray(b'\0'*(end-entry))
    for r in regions:
        code=bytes.fromhex(r['code_hex']);start=r['address']-entry;payload[start:start+len(code)]=code
    fmt=fixture['image_format'];arch=fixture['architecture'];slot=fixture['image_slot']
    if fmt=='pe':
        if arch!='x86_64' or slot!=0x2060 or end>0x2000:raise ValueError('PE fixture layout')
        b=blob('kImpExpPeX64');nt=get(b,0x3c,4);opt=nt+24;section=opt+get(b,nt+20,2)
        put(b,opt+24,0,8);put(b,opt+16,entry,4);put(b,opt+112,0,8)
        put(b,section+8,len(payload),4);put(b,section+12,entry,4)
        put(b,section+16,(len(payload)+511)&~511,4);put(b,section+20,len(b),4)
        return bytes(b+payload+bytes((-len(payload))%512))
    if fmt!='elf':raise ValueError('unknown image fixture format')
    if arch=='x86_32':
        b=bytearray(0x1000);b[:7]=b'\x7fELF\x01\x01\x01'
        struct.pack_into('<HHIIIIIHHHHHH',b,16,3,3,1,entry,52,0x300,0,52,32,1,40,6,5)
        struct.pack_into('<IIIIIIII',b,52,1,0,0,0,0x1000+len(payload),0xa000,7,0x1000)
        strings=b'\0puts\0__gmon_start__\0';names=b'\0.text\0.dynsym\0.dynstr\0.rel.plt\0.shstrtab\0'
        b[0x230:0x230+len(strings)]=strings;b[0x280:0x280+len(names)]=names
        struct.pack_into('<IIIBBH',b,0x210,1,0,0,0x12,0,0);struct.pack_into('<IIIBBH',b,0x220,6,0,0,0x12,0,0)
        struct.pack_into('<IIII',b,0x260,slot,(1<<8)|7,slot+8,(2<<8)|6)
        entries=[(0,0,0,0,0,0,0,0,0,0),(1,1,6,entry,0x1000,len(payload),0,0,16,0),
                 (7,11,2,0x200,0x200,48,3,1,4,16),(15,3,2,0x230,0x230,len(strings),0,0,1,0),
                 (23,9,2,0x260,0x260,16,2,1,4,8),(32,3,0,0,0x280,len(names),0,0,1,0)]
        for i,fields in enumerate(entries):struct.pack_into('<10I',b,0x300+40*i,*fields)
        return bytes(b+payload)
    b=blob('kSlotsElfX64');shoff=get(b,0x28,8);count=get(b,0x3c,2)
    for i in range(count):
        section=shoff+i*64
        if get(b,section+8,8)&2:put(b,section+16,get(b,section+24,8),8)
    text=shoff+64;put(b,0x18,entry,8);put(b,text+16,entry,8);put(b,text+24,0x1000,8);put(b,text+32,len(payload),8)
    rela=get(b,shoff+4*64+24,8);put(b,rela,slot,8);put(b,rela+24,slot+8,8)
    put(b,rela+48,slot+16,8);put(b,rela+64,entry,8)
    if arch=='arm64':
        put(b,18,183,2);put(b,rela+8,(1<<32)|1026,8);put(b,rela+32,(2<<32)|1025,8);put(b,rela+56,1027,8)
    elif arch!='x86_64':raise ValueError('ELF architecture')
    struct.pack_into('<IIQQQQQQ',b,64,1,7,0,0,0,0x1000+len(payload),0xa000,0x1000)
    b.extend(bytes(0x1000-len(b)));return bytes(b+payload)
