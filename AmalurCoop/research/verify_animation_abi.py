"""Read-only native animation ABI fixture; does not compile, inject, or launch koa.exe."""
import sys, struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import *
import pefile
p=pefile.PE(r'D:\SteamLibrary\steamapps\common\Kingdoms of Amalur Re-Reckoning\koa.exe')
u=Uc(UC_ARCH_X86, UC_MODE_32)
u.mem_map(0x400000,0x2000000); u.mem_write(0x400000,p.get_memory_mapped_image())
u.mem_map(0x4000000,0x400000)
controller=0x4000000; model=0x4001000; active=0x4002000; rig=0x4003000
manager=0x4004000; vtable=0x4005000; state=0x4006000; blend=0x4007000; output=0x4008000
retain=0x4020000; release=0x4020100; stop=0x4030000; stack=0x43ff000
refs={}; observed=[]
def w(a,v):u.mem_write(a,struct.pack('<I',v & 0xffffffff))
def r(a):return struct.unpack('<I',u.mem_read(a,4))[0]
def ret(v=0,n=0):
 sp=u.reg_read(UC_X86_REG_ESP);ra=r(sp)
 u.reg_write(UC_X86_REG_EAX,v);u.reg_write(UC_X86_REG_ESP,sp+4+n);u.reg_write(UC_X86_REG_EIP,ra)
def hook(uc,a,n,_):
 sp=uc.reg_read(UC_X86_REG_ESP);cx=uc.reg_read(UC_X86_REG_ECX)
 if a==0x11bd91a:ret(uc.reg_read(UC_X86_REG_EAX)) # security-cookie checker, no game runtime needed
 elif a in (retain,release):
  h=r(sp+4);refs[h]=refs.get(h,0)+(1 if a==retain else -1);ret(0,4)
 elif a==0xA71E80:ret(rig,8)
 elif a==0xD3EBF0:w(cx+0x60,state);w(cx+0x64,1);ret(0,4)
 elif a==0xCE71D0:
  request=r(sp+4)
  observed.append(bytes(u.mem_read(request,0x70)))
  ret(1,4)
 elif a==0xD1D5D0:
  out=r(sp+20)
  if out:w(out,123)
  ret(0,20)
u.hook_add(UC_HOOK_CODE,hook)
w(manager,vtable);w(vtable+0xa4,retain);w(vtable+0xa8,release)
w(0x19f4d50,manager);w(0x19f4d4c,manager)
w(controller+0xa8,model);w(model+0x1a8,1);w(model+0x1a4,active);u.mem_write(active,b'\x01')
w(rig+0xd8,0x111222)
u.mem_write(blend,struct.pack('<6fB3x',1,2,3,4,5,6,1))
def call(addr,args):
 w(stack,stop)
 for i,v in enumerate(args):w(stack+4+i*4,v)
 u.reg_write(UC_X86_REG_ESP,stack);u.reg_write(UC_X86_REG_ECX,controller)
 u.emu_start(addr,stop,count=50000)
 assert u.reg_read(UC_X86_REG_EIP)==stop,hex(u.reg_read(UC_X86_REG_EIP))
 assert u.reg_read(UC_X86_REG_ESP)==stack+4+len(args)*4, 'ABI stack imbalance'
 return u.reg_read(UC_X86_REG_EAX)&255
args=[0xabcdef,-1,struct.unpack('<I',struct.pack('<f',1.25))[0],1,1,1,0,150,output,21,22,1,blend]
# Native failure path still consumes both by-value handles and all 13 arguments.
w(controller+0xa4,0);refs.update({21:1,22:1})
assert call(0xD3EC40,args)==0 and refs[21]==refs[22]==0
print('PASS D3EC40 failure path: RET 0x34, two owning references consumed')
# Native success path: game allocators/clip lookup mocked, argument marshalling,
# native request construction and handle destructor/epilogue execute unmodified.
w(controller+0xa4,42);w(controller+0x64,0);refs.update({21:1,22:1})
assert call(0xD3EC40,args)==1 and r(output)==123
assert refs[21]==refs[22]==0
request=observed[-1]
assert struct.unpack_from('<I',request,0xc)[0]==0xabcdef
assert struct.unpack_from('<i',request,0x14)[0]==-1
assert struct.unpack_from('<I',request,0x38)[0]==150
assert struct.unpack_from('<f',request,0x3c)[0]==1.25
assert struct.unpack_from('<6f',request,0x40)==(1,2,3,4,5,6)
assert request[0x58]==1 and request[0x6c]==1
print('PASS D3EC40 success: hash, variant, speed, start offset, descriptor, output and refcounts')
# Binder initializes both rig handle and default animation hash.
w(controller+0xa4,0);w(output,42);refs[42]=0
call(0xCB45F0,[output]);assert r(controller+0xa4)==42 and r(controller+0xb0)==0x111222 and refs[42]==1
print('PASS CB45F0: RET 4, retained rig and native default hash')

