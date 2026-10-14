"""Read-only D1D5D0 ABI fixture. No build, injection or game launch."""
import sys,struct
from pathlib import Path

from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn.x86_const import *
import pefile
import hashlib
exe = Path(sys.argv[1])
assert hashlib.sha256(exe.read_bytes()).hexdigest() == '16a400f6e8fc10dbe446a9e57717de9fbe975ef17c004f4ca405e2e4e09cb314', 'Unmapped executable'
p=pefile.PE(str(exe))
u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(0x400000,0x2000000);u.mem_write(0x400000,p.get_memory_mapped_image());u.mem_map(0x4000000,0x400000)
rig=0x4000000;catalog=0x4001000;entries=0x4002000;request=0x4003000;queue=0x4004000;slots=0x4005000;registry=0x4006000;manager=0x4008000;vtable=0x4009000;names=0x400a000
retain=0x4020000;selector=0x4020100;stop=0x4030000;stack=0x43ff000;next_node=0x4010000
refs={};allocations=[]
def w(a,v):u.mem_write(a,struct.pack('<I',v&0xffffffff))
def r(a):return struct.unpack('<I',u.mem_read(a,4))[0]
def ret(v=0,n=0):
 sp=u.reg_read(UC_X86_REG_ESP);ra=r(sp);u.reg_write(UC_X86_REG_EAX,v);u.reg_write(UC_X86_REG_ESP,sp+4+n);u.reg_write(UC_X86_REG_EIP,ra)
def hook(uc,a,n,_):
 global next_node
 sp=uc.reg_read(UC_X86_REG_ESP)
 if a==0xA71E80:
  assert r(sp+4)==31 and r(sp+8)==1;ret(catalog,8)
 elif a==0xAFDF70:
  assert r(sp+4)==0x1f and r(sp+8)==0x6c
  node=next_node;next_node+=0x100;allocations.append(node);ret(node)
 elif a==0xAFCA30:
  dst,src,size=r(sp+4),r(sp+8),r(sp+12);u.mem_write(dst,bytes(u.mem_read(src,size)));ret(dst)
 elif a==retain:
  handle=r(sp+4);refs[handle]=refs.get(handle,0)+1;ret(0,4)
 elif a==selector:
  assert r(sp+4)==entries and r(sp+8)==0xffffffff and r(sp+12)==0x12340001
  u.mem_write(r(sp+16),struct.pack('<4i',2,-1,-1,-1));u.mem_write(r(sp+20),struct.pack('<4f',1,0,0,0));ret(0,20)
u.hook_add(UC_HOOK_CODE,hook)
w(0x19fdfe4,registry);w(0x19f4d50,manager);w(manager,vtable);w(vtable+0xa4,retain)
w(catalog+0x10,1);w(catalog+0x14,entries);w(entries,4)
w(rig+0x50,names);w(rig+0x54,1);w(names,0x123456)
w(request,0x12340000);w(request+4,21);w(request+8,22);w(request+0xc,0x123456);w(request+0x34,0x12340001);w(request+0x38,150)
u.mem_write(request+0x3c,struct.pack('<f',1.25));u.mem_write(request+0x40,struct.pack('<6fB3x',1,2,3,4,5,6,1));w(request+0x64,31);w(request+0x68,0);u.mem_write(request+0x6c,b'\x01')
w(queue+4,slots);w(queue+8,20)
def call(state=0):
 w(stack,stop)
 for i,v in enumerate([request,state,-1,queue,0]):w(stack+4+i*4,v)
 u.reg_write(UC_X86_REG_ESP,stack);u.reg_write(UC_X86_REG_ECX,rig);u.emu_start(0xD1D5D0,stop,count=5000)
 assert u.reg_read(UC_X86_REG_EIP)==stop and u.reg_read(UC_X86_REG_ESP)==stack+24
 node=r(slots+((r(queue+0x18)+19)%20)*4)
 assert node==allocations[-1]
 assert r(node)==0 and r(node+4)==0 and r(node+0x2c)==31
 assert r(node+0x30)==21 and r(node+0x34)==22
 assert r(node+0x38)==(state&0xffffffff) and r(node+0x3c)==0xffffffff and r(node+0x40)==150
 assert struct.unpack('<f',u.mem_read(node+0x28,4))[0]==1.25
 assert bytes(u.mem_read(node+0x44,28))==bytes(u.mem_read(request+0x40,28))
 assert r(node+0x60)==r(node+0x64)==0 and u.mem_read(node+0x68,1)==b'\x01'
 return node
u.mem_write(request+0x14,struct.pack('<4i',1,2,-1,-1));u.mem_write(request+0x24,struct.pack('<4f',.75,.25,0,0))
node=call();assert bytes(u.mem_read(node+8,32))==bytes(u.mem_read(request+0x14,32));assert refs=={21:1,22:1}
print('PASS native D1D5D0: five-argument RET 14h, values, selected variants, weights, queue insertion and retained references')
# Verify capture-after-call sees the random variant the engine selected.
w(request+0x14,-1);selection=0x400b000;selectvt=0x400c000;w(entries+0xc,selection);w(selection,selectvt);w(selectvt+0x10,selector)
node=call();assert struct.unpack('<4i',u.mem_read(node+8,16))==(2,-1,-1,-1);assert refs=={21:2,22:2}
print('PASS native random selector: captured resolved variant is 2, not -1')
for state in (1, 37, -1):
 call(state)
print('PASS native queue preserves nonzero rig state and the -1 sentinel')

# Execute D0F380's field-copy block with distinct descriptor values. This is
# the active-instance mapping used for updates after D405C0 consumes a node.
clip=0x400d000;desc=0x400e000
for offset,value in ((0x18,77),(0x20,0x3fa00000),(0x1c,0x3f400000),
                     (0x28,150),(0x14,2),(0x24,0x3e800000)):
 w(desc+offset,value)
w(stack+0x14,1234)
u.reg_write(UC_X86_REG_ESP,stack);u.reg_write(UC_X86_REG_EDI,desc);u.reg_write(UC_X86_REG_ESI,clip)
u.emu_start(0xD0F440,0xD0F46B,count=100)
assert u.reg_read(UC_X86_REG_EIP)==0xD0F46B
assert r(clip+0x5c)==77 and r(clip+0x64)==2
assert r(clip+0x74)==0x3fa00000 and r(clip+0x78)==0x3f400000
assert r(clip+0x48)==1234 and r(clip+0x50)==150
print('PASS native active clip field mapping: group, variant, speed, weight and separate playback clock')
