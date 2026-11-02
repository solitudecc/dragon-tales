"""Raw x87 stack/memory differential test (i386 and x64, all RC modes).
Unicorn is the data oracle. Its missing exception/C1 updates are checked
independently using exact Fraction arithmetic, backed by x87_native.c.
No conversion through Python double is used. ONLY=substring TRIALS=128.
"""
import os, sys, random
from fractions import Fraction
sys.path.insert(0,os.path.join(os.path.dirname(__file__),'../../python'))
import dragon
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_MODE_64
from unicorn.x86_const import *
X=dragon.x86
CODE,DATA=0x100000,0x200000
TOP=(480,8,'TOP');TAG=(482,16,'TAG');CW=(484,16,'CW');SW=(486,16,'SW')
def lo(i): return (400+10*i,64,'FPU-low')
def hi(i): return (408+10*i,16,'FPU-high')
def power(e): return Fraction(1<<e) if e>=0 else Fraction(1,1<<-e)
def value80(sig,exp):
    e=exp&0x7fff
    if e==0x7fff or e and not sig>>63: return None
    return (-1 if exp>>15 else 1)*sig*power((e or 1)-16383-63)
def value_ieee(bits,width):
    f,bias=(23,127) if width==32 else (52,1023)
    exp=(bits>>f)&(2*bias+1);frac=bits&((1<<f)-1)
    if exp==2*bias+1: return None
    return (-1 if bits>>(width-1) else 1)*((1<<f)|frac if exp else frac)*power((exp or 1)-bias-f)
def classify(sig,exp):
    e=exp&0x7fff
    return 1 if not e and not sig else 2 if e in (0,0x7fff) or not sig>>63 else 0
# Full precision inputs, incl. signed zero, subnormals, S/QNaNs, ties and bounds.
RAW=[(0,0),(0,0x8000),(0x8000000000000000,0x3fff),(0xc000000000000000,0x3fff),
 (0xc000000000000000,0xbfff),(0x8000000000000001,0x3fff),(0xffffffffffffffff,0x3ffe),
 (0x8000000000000000,0x7fff),(0x8000000000000000,0xffff),
 (0xc000000000001234,0x7fff),(0x8000000000001234,0x7fff),
 (1,0),(0x7fffffffffffffff,0),(0x8000000000000000,1),
 (0x8000000000000000,0x7ffe),(0xffffffffffffffff,0x403d),(0x8000000000000000,0xc03e),
 (0xfffffffffffff800,0x3c00),(0xfffffffffffff7ff,0x3c00),(0xfffffffffffff801,0x3c00),
 (0x8000000000000000,0x3bcd),(0x8000000000000001,0x400e),(0xffffffffffffffff,0xbffe),(0x8000000000000000,0)]
IEEE32=[0,0x80000000,0x3f800000,1,0x7fffff,0x800000,0x7f800000,0xff800000,0x7fc01234,0x7f801234,0x7f7fffff]
IEEE64=[0,0x8000000000000000,0x3ff0000000000000,1,0xfffffffffffff,0x10000000000000,0x7ff0000000000000,0xfff0000000000000,0x7ff8000000001234,0x7ff0000000001234,0x7fefffffffffffff]
FORMS=['fld dword ptr [B]','fld qword ptr [B]','fld tbyte ptr [B]','fldz','fld1',
 'fild word ptr [B]','fild dword ptr [B]','fild qword ptr [B]',
 'fst dword ptr [B]','fst qword ptr [B]','fstp dword ptr [B]','fstp qword ptr [B]','fstp tbyte ptr [B]',
 'fist word ptr [B]','fist dword ptr [B]','fistp word ptr [B]','fistp dword ptr [B]','fistp qword ptr [B]',
 'fisttp word ptr [B]','fisttp dword ptr [B]','fisttp qword ptr [B]',
 'fincstp','fdecstp','fldcw word ptr [B]','fnstcw word ptr [B]','fnstsw ax','fnstsw word ptr [B]',
 'fninit','fnclex','fnop','wait','emms','fchs','fabs','ftst','fxam','fcompp','fucompp']
for i in (0,1,3):
    FORMS += [f'{op} st({i})' for op in ('fld','fst','fstp','fxch','ffree','fcom','fcomp','fucom','fucomp')]
    FORMS += [f'{op} st, st({i})' for op in ('fcomi','fucomi','fcomip','fucomip')]
FORMS += [f'{op} {width} ptr [B]' for op in ('fcom','fcomp') for width in ('dword','qword')]
def rounded_integer(v,rc):
    if rc==0: return round(v)
    if rc==1: return v.numerator//v.denominator
    if rc==2: return -((-v.numerator)//v.denominator)
    return int(v)
def status_oracle(text,raw,page,rc):
    """Return defined mask/value; condition-code undefined bits are excluded."""
    sig,exp=raw;v=value80(sig,exp);m=text.split()[0]
    mask,bits=0xff,0
    if m in ('fld','fild','fldz','fld1','fxch','fchs','fabs','fincstp','fdecstp'): mask|=0x200
    if m=='fld' and ('dword ptr' in text or 'qword ptr' in text):
        w=32 if 'dword' in text else 64;f,bias=(23,127) if w==32 else (52,1023)
        x=int.from_bytes(page[:w//8],'little');e=x>>f & (2*bias+1);fr=x&((1<<f)-1)
        bits|=1 if e==2*bias+1 and fr and not (fr>>(f-1)) else 0
        bits|=2 if not e and fr else 0
    if m in ('fist','fistp','fisttp'):
        w=16 if 'word ptr' in text and 'dword' not in text and 'qword' not in text else 32 if 'dword' in text else 64
        r=rounded_integer(v,3 if m=='fisttp' else rc) if v is not None else None
        invalid=r is None or r<-(1<<(w-1)) or r>=(1<<(w-1))
        bits|=1 if invalid else 32 if r!=v else 0;mask|=0x200
        if not invalid and m!='fisttp' and abs(r)>abs(v): bits|=0x200
    if m in ('fst','fstp') and ('dword ptr' in text or 'qword ptr' in text):
        w=32 if 'dword' in text else 64;bias=127 if w==32 else 1023
        unsupported=bool(exp&0x7fff) and not sig>>63
        nan=(exp&0x7fff)==0x7fff and bool(sig&0x7fffffffffffffff)
        signaling=nan and not (sig>>62)&1
        bits|=int(unsupported or signaling)
        if v is not None:
            # Independent integer/Fraction rounding including overflow and subnormals.
            av=abs(v);e=av.numerator.bit_length()-av.denominator.bit_length() if av else 0
            if av and av<power(e): e-=1
            step=power(max(e,1-bias)-(23 if w==32 else 52))
            r=rounded_integer(v/step,rc)*step
            overflow=abs(r)>=power(bias+1)
            inf=overflow and (rc==0 or rc==1 and v<0 or rc==2 and v>0)
            if overflow: r=(-1 if v<0 else 1)*(power(bias+1)-power(bias-(23 if w==32 else 52)))
            inexact=overflow or r!=v
            bits|=8 if overflow else 0;bits|=16 if av<power(1-bias) and inexact else 0;bits|=32 if inexact else 0
            if inexact:
                mask|=0x200
                if inf or abs(r)>av: bits|=0x200
    if m.startswith(('fcom','fucom')) or m=='ftst':
        # Compare values/CC via Unicorn; independently validate IE/DE only below.
        return None
    return mask,bits
rng=random.Random(871);ok=bad=unlifted=executions=oracle_gaps=0
for arch,mode,B in [(dragon.Architecture.X86_32,UC_MODE_32,'ebx'),(dragon.Architecture.X86_64,UC_MODE_64,'rbx'),(dragon.Architecture.X86_64,UC_MODE_64,'ebx + 0x200004'),(dragon.Architecture.X86_64,UC_MODE_64,'rip + 1048570')]:
  a=dragon.Assembler(arch,dragon.Configuration());d=dragon.Disassembler(arch,dragon.Configuration())
  for form in FORMS:
    wrapped=mode==UC_MODE_64 and B.startswith('ebx')
    if (wrapped or B.startswith('rip')) and 'B' not in form:continue
    text=form.replace('B',B)
    if os.getenv('ONLY') and os.environ['ONLY'] not in text: continue
    code=bytes(a.assemble(text));block=d.lift_trace(d.disassemble(code,CODE).instructions())
    if block.unlifted(): unlifted+=1;print('UNLIFTED',B,text,flush=True);continue
    wrong=[]
    for trial in range(int(os.getenv('TRIALS','128'))):
      top=trial%8;rc=(trial//len(RAW))%4;control=0x7f|([0,2,3][trial%3]<<8)|(rc<<10)
      raw=[RAW[(trial+i*3)%len(RAW)] if trial<len(RAW)*2 else (rng.getrandbits(63)|(1<<63),rng.randrange(1,0x7fff)|(rng.randrange(2)<<15)) for i in range(8)]
      if trial<4*len(RAW):raw[top]=RAW[trial%len(RAW)]
      tag=sum((classify(*raw[i]) if ((i-top)&7)<4 else 3)<<(2*i) for i in range(8))
      page=bytearray(rng.randbytes(64))
      m=text.split()[0]
      if ('dword ptr' in text or 'qword ptr' in text) and m in ('fld','fcom','fcomp'):
          w=32 if 'dword' in text else 64;vals=IEEE32 if w==32 else IEEE64
          page[:w//8]=vals[trial%len(vals)].to_bytes(w//8,'little')
      elif m=='fld':page[:10]=raw[(top+1)&7][0].to_bytes(8,'little')+raw[(top+1)&7][1].to_bytes(2,'little')
      if m=='fldcw':page[:2]=control.to_bytes(2,'little')
      u=Uc(UC_ARCH_X86,mode);u.mem_map(CODE,4096);u.mem_map(DATA,4096);u.mem_write(CODE,code);u.mem_write(DATA,bytes(page))
      initial_base=0xaabbccddfffffffc if wrapped else DATA
      u.reg_write(UC_X86_REG_RBX if mode==UC_MODE_64 else UC_X86_REG_EBX,initial_base)
      initax=0x123456789abcdef0 if mode==UC_MODE_64 else 0x9abcdef0
      u.reg_write(UC_X86_REG_RAX if mode==UC_MODE_64 else UC_X86_REG_EAX,initax);u.reg_write(UC_X86_REG_EFLAGS,0x8d7)
      for i,(sig,exp) in enumerate(raw):u.reg_write(UC_X86_REG_FP0+i,(sig,exp))
      u.reg_write(UC_X86_REG_FPTAG,tag);u.reg_write(UC_X86_REG_FPSW,top<<11);u.reg_write(UC_X86_REG_FPCW,control)
      u.emu_start(CODE,CODE+len(code),count=1)
      ex=dragon.SymbolicExecutor();ex.set_register(X.RBX,initial_base);ex.set_register(X.RAX,initax)
      for i,(sig,exp) in enumerate(raw):ex.set_register(lo(i),sig);ex.set_register(hi(i),exp)
      for r,v in [(TOP,top),(TAG,tag),(CW,control),(SW,top<<11)]:ex.set_register(r,v)
      for f in ('CF','PF','AF','ZF','SF','OF'):ex.set_register(getattr(X,f),1)
      ex.map_memory(DATA,bytes(page));result=ex.run(block)
      checks=[('run',result.ok,True),('TOP',ex.concrete_register(TOP),(u.reg_read(UC_X86_REG_FPSW)>>11)&7),('TAG',ex.concrete_register(TAG),u.reg_read(UC_X86_REG_FPTAG)),('CW',ex.concrete_register(CW),u.reg_read(UC_X86_REG_FPCW)),('AX',ex.concrete_register(X.RAX),u.reg_read(UC_X86_REG_RAX if mode==UC_MODE_64 else UC_X86_REG_EAX))]
      for i in range(8):
          sig,exp=u.reg_read(UC_X86_REG_FP0+i);checks += [(f'R{i}.lo',ex.concrete_register(lo(i)),sig),(f'R{i}.hi',ex.concrete_register(hi(i)),exp)]
      for offset in range(0,64,2):checks += [(f'mem{offset}',ex.concrete_memory(DATA+offset,16),int.from_bytes(u.mem_read(DATA+offset,2),'little'))]
      # Narrow, documented Unicorn bugs independently reproduced by x87_native.c.
      # Correct only the affected reference fields; all other data use Unicorn.
      sig,exp=raw[top]
      if m=='fld' and ('dword ptr' in text or 'qword ptr' in text):
          w=32 if 'dword' in text else 64;f,bias=(23,127) if w==32 else (52,1023)
          x=int.from_bytes(page[:w//8],'little');fr=x&((1<<f)-1)
          if (x>>f)&(2*bias+1)==2*bias+1 and fr and not (fr>>(f-1)):
              n=f'R{(top-1)&7}.lo'
              checks=[(name,got,want|(1<<62) if name==n else want) for name,got,want in checks];oracle_gaps+=1
      if m in ('fst','fstp') and (exp&0x7fff)==0x7fff and sig>>63 and sig&0x7fffffffffffffff and not (sig>>62)&1 and ('dword ptr' in text or 'qword ptr' in text):
          w=32 if 'dword' in text else 64;f,bias=(23,127) if w==32 else (52,1023)
          bits=((exp>>15)<<(w-1))|((2*bias+1)<<f)|(1<<(f-1))|((sig>>(63-f))&((1<<f)-1))
          checks=[(n,g,(bits>>(int(n[3:])*8))&65535 if n.startswith('mem') and int(n[3:])<w//8 else v) for n,g,v in checks];oracle_gaps+=1
      if m=='fisttp':
          w=16 if 'word ptr' in text and 'dword' not in text and 'qword' not in text else 32 if 'dword' in text else 64
          v=value80(sig,exp);r=int(v) if v is not None else None
          if r is None or r<-(1<<(w-1)) or r>=(1<<(w-1)):
              bits=1<<(w-1)
              checks=[(n,g,(bits>>(int(n[3:])*8))&65535 if n.startswith('mem') and int(n[3:])<w//8 else v) for n,g,v in checks];oracle_gaps+=1
      oracle=status_oracle(text,raw[top],page,rc)
      if oracle is not None:
          mask,want=oracle
          # Preserving instructions retain initial zero CC and low status.
          checks += [('SW',ex.evaluate(ex.read_register(SW)&mask),want)]
          if u.reg_read(UC_X86_REG_FPSW)&mask!=want:oracle_gaps+=1
      else:
          # Independent exception-priority oracle for comparisons.
          def categories(pair):
              s,e=pair;e&=0x7fff
              nan=e==0x7fff and bool(s>>63) and bool(s&0x7fffffffffffffff)
              unsupported=bool(e) and not s>>63
              return nan,nan and not (s>>62)&1,unsupported,not e and bool(s)
          left=categories(raw[top])
          if 'ptr' in text:
              w=32 if 'dword' in text else 64;f,bias=(23,127) if w==32 else (52,1023)
              bits=int.from_bytes(page[:w//8],'little');e=(bits>>f)&(2*bias+1);fr=bits&((1<<f)-1)
              right=(e==2*bias+1 and bool(fr),e==2*bias+1 and bool(fr) and not (fr>>(f-1)),False,not e and bool(fr))
          elif m=='ftst':right=(False,False,False,False)
          else:
              import re
              operand=re.search(r'st\((\d)\)',text);index=int(operand[1]) if operand else 1
              right=categories(raw[(top+index)&7])
          unordered=left[0] or right[0] or left[2] or right[2]
          invalid=left[2] or right[2] or (left[1] or right[1] if m.startswith('fucom') else unordered)
          low=int(bool(invalid))| (2 if not unordered and (left[3] or right[3]) else 0)
          checks += [('compare exceptions',ex.evaluate(ex.read_register(SW)&0xff),low)]
          # QEMU correctly computes comparison CC/flags but does not set IE/DE.
          mask=0x4700
          checks += [('compareCC',ex.evaluate(ex.read_register(SW)&mask),u.reg_read(UC_X86_REG_FPSW)&mask)]
      if m.startswith(('fcomi','fucomi')):
          flags=u.reg_read(UC_X86_REG_EFLAGS)
          checks += [(f,ex.concrete_register(getattr(X,f)),(0 if f in ('AF','SF','OF') else (flags>>bit)&1)) for f,bit in [('CF',0),('PF',2),('AF',4),('ZF',6),('SF',7),('OF',11)]]
      if m=='fxam': checks += [('XAM',ex.evaluate(ex.read_register(SW)&0x4700),u.reg_read(UC_X86_REG_FPSW)&0x4700)]
      wrong=[f'{n}={g!r} want {w!r}' for n,g,w in checks if g!=w]
      executions+=1
      if wrong:print('DIFF',B,text,'trial',trial,'rc',rc,'raw',raw[top],wrong[:4],flush=True);break
    if wrong:bad+=1
    else:ok+=1
print(f'{ok} forms ok; {bad} diff; {unlifted} unlifted; {executions} executions; {oracle_gaps} Unicorn reference exceptions checked independently',flush=True)
sys.exit(bool(bad or unlifted))
