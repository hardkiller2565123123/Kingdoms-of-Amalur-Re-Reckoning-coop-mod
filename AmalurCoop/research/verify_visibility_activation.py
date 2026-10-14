"""Execute mapped visibility and animation activation instructions without launching the game."""
import struct, hashlib
from pathlib import Path
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_ECX, UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESI
exe=Path(r'D:/SteamLibrary/steamapps/common/Kingdoms of Amalur Re-Reckoning/koa.exe')
assert hashlib.sha256(exe.read_bytes()).hexdigest()=='16a400f6e8fc10dbe446a9e57717de9fbe975ef17c004f4ca405e2e4e09cb314'
u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(0x400000,0x2000000);u.mem_write(0x400000,pefile.PE(str(exe)).get_memory_mapped_image())
u.mem_map(0x4000000,0x400000)
actor=0x4000000;model=0x4001000;catalogs=0x4002000;active=0x4003000;manager=0x4004000;render_table=0x4005000;handle=0x4006000
stack=0x43ff000;stop=0x4030000
opacity=[];references=[];gate=[];published=[]
def w(a,v):u.mem_write(a,struct.pack('<I',v))
def r(a):return struct.unpack('<I',u.mem_read(a,4))[0]
def ret(v=0,n=0):
 sp=u.reg_read(UC_X86_REG_ESP);ra=r(sp);u.reg_write(UC_X86_REG_EAX,v);u.reg_write(UC_X86_REG_ESP,sp+4+n);u.reg_write(UC_X86_REG_EIP,ra)
def hook(uc,a,n,_):
 sp=u.reg_read(UC_X86_REG_ESP)
 if a==0xC8B8D0:ret(0,4)
 elif a==0xCB1570:opacity.append(struct.unpack('<f',bytes(u.mem_read(sp+4,4)))[0]);ret(0,4)
 elif a==0xCB1660:ret(0,4)
 elif a==0xA6DF80:ret(model,4)
 elif a==0xA6FE10:ret(0x12345678)
 elif a==0xA6C280:ret()
 elif a==0xA97F60:references.append((r(sp+4),r(sp+8),r(sp+12)));ret(0,12)
 elif a in (0xD464FE,0xD4658C):gate.append(a);u.emu_stop()
 elif a==0xC37AD0:ret(0,4)
 elif a==0xD2E0A0:published.append(('actor',r(sp+4)));ret(0,4)
 elif a==0xFB9650:published.append(('spatial',r(sp+8)));ret(0,8)
 elif a in (0xC37B00,0x11BD91A):ret()
u.hook_add(UC_HOOK_CODE,hook)
def call(a,args):
 w(stack,stop)
 for i,x in enumerate(args):w(stack+4+i*4,x)
 u.reg_write(UC_X86_REG_ESP,stack);u.reg_write(UC_X86_REG_ECX,actor)
 u.emu_start(a,stop,count=10000)
 assert u.reg_read(UC_X86_REG_EIP)==stop
 assert u.reg_read(UC_X86_REG_ESP)==stack+4+len(args)*4
w(0x19fdf60,manager);w(manager+4,render_table);w(actor+0x1c0,1);w(actor+0x1bc,0x3f800000)
for bit in (4,0x2000,0x20000,0x40000,0x80000):
 w(actor+0x1d0,bit);call(0xCCC300,[0]);assert opacity[-1]==0
 w(actor+0x1d0,bit & ~0xEA147);call(0xCCC300,[0]);assert opacity[-1]==1
print('PASS native CCC300: imported visibility bits suppress opacity; normalized root renders with opacity 1')
w(actor+0x198,42);w(model+0x190,2);w(model+0x18c,catalogs);w(catalogs,12);w(catalogs+4,13)
w(actor+0x1a8,2);w(actor+0x1a4,active);w(handle,13)
def evaluate_gate():
 u.reg_write(UC_X86_REG_ESI,actor);u.emu_start(0xD464D6,stop,count=1000);return gate[-1]
assert evaluate_gate()==0xD464FE
call(0xCCB840,[handle,0])
assert bytes(u.mem_read(active,2))==b'\0\1'
assert references==[(4,0x12345678,1)]
assert evaluate_gate()==0xD4658C
# Production only calls activation when zero; another packet cannot inflate its count.
if not u.mem_read(active+1,1)[0]:call(0xCCB840,[handle,0])
assert bytes(u.mem_read(active,2))==b'\0\1' and len(references)==1
print('PASS native CCB840 RET 8: correct catalog count/cache reference; D464D6 now enters animation evaluation path')

# The render hook's ABI and matrix ownership, using the normal C7 path.
# Keep the native F116B0 entry/exit and mock only its downstream services.
matrix=0x4007000
w(0x19fec38,manager);w(manager+0xbfc,manager)
u.mem_write(actor+0xf1,b'\0')
assert bytes(u.mem_read(0xF116B0,6))==bytes.fromhex('81ec84000000')
call(0xF116B0,[matrix])
assert published==[('actor',matrix),('spatial',matrix)]
print('PASS native F116B0 RET 4: same matrix reaches actor and spatial publication')
