import struct
def inputs():
 values=[0,1,2,3,4,5,0xffffffff,0xfffffffe,0x80000000,0x7fffffff,0xaaaaaaaa,0x55555555,0x12345678,0xdeadbeef,0x10000,0xffff]
 rows=[]
 for raw in values:
  for offset in range(4):
   for pattern in range(4):
    b=bytearray(96);struct.pack_into('<I',b,0,raw);b[16]=pattern&1;b[17]=offset;b[32:]=bytes((i*37+pattern*71+len(rows))&255 for i in range(64));rows.append(bytes(b))
 assert len(rows)==256;return rows
def model(wire,fault=False):
 value=struct.unpack_from('<I',wire)[0];offset=wire[17];arena=bytearray(wire[32:])
 for _ in range(5):
  value=(value+1)&0xffffffff;arena[offset:offset+4]=value.to_bytes(4,'little')
  if value>=4:break
 else:raise AssertionError('independent loop bound')
 result=value^(1 if fault else 0)
 return result.to_bytes(8,'little')+arena+offset.to_bytes(8,'little')
