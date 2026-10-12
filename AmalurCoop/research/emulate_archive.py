import sys,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn.x86_const import *
import pefile
p=pefile.PE(r'D:\SteamLibrary\steamapps\common\Kingdoms of Amalur Re-Reckoning\koa.exe')
u=Uc(UC_ARCH_X86,UC_MODE_32)
u.mem_map(0x400000,0x2000000);u.mem_write(0x400000,p.get_memory_mapped_image())
u.mem_map(0x4000000,0x400000)
def w(a,v):u.mem_write(a,struct.pack('<I',v))
def r(a):return struct.unpack('<I',u.mem_read(a,4))[0]
def ret(value=0,n=0):
 sp=u.reg_read(UC_X86_REG_ESP);ra=r(sp);u.reg_write(UC_X86_REG_EAX,value);u.reg_write(UC_X86_REG_ESP,sp+4+n);u.reg_write(UC_X86_REG_EIP,ra)
render=0x4000000;manager=0x4001000;table=0x4002000;actor=0x4003000;model=0x4004000
archive=0x4005000;buffer=0x4010000;chunks=0x4021000;vtable=0x4022000;reader=0x4023000;stop=0x4030000;stack=0x43FF000
w(0x19fdf54,manager);w(manager+0xc4,table);w(manager+0xc8,10);w(table+8,actor);w(table+12,actor)
w(render+0x1c,7);w(render+0x9c,2);w(actor+0x198,42)
u.mem_write(actor+0x124,bytes(range(45)));u.mem_write(actor+0x154,bytes(range(45,90)))
for i,a in enumerate([0x184,0x188,0x18c,0x19c,0x1d0]):w(actor+a,i+100)
w(actor+0x1b4,43)
w(archive+8,buffer);w(archive+0xc,65536);w(archive+0x14,chunks);w(archive+0x1c,256)
def hook(uc,address,size,user):
 sp=uc.reg_read(UC_X86_REG_ESP);cx=uc.reg_read(UC_X86_REG_ECX)
 if address==0xA6FE10:ret(0x12345678 if u.mem_read(cx+2,1)==b'\x14' else 0x87654321)
 elif address==0xA6C280:ret()
 elif address==0xA6DF80:ret(model,4)
 elif address==0xAFCA30:
  dst,src,n=r(sp+4),r(sp+8),r(sp+12);u.mem_write(dst,bytes(u.mem_read(src,n)));ret(dst)
 elif address==0xD0FCD0:
  stream=r(sp+4);offset=r(stream+0x10);raw=struct.pack('<IBI',0x17d60c9,1,0)
  u.mem_write(r(stream+8)+offset,raw);w(stream+0x10,offset+len(raw));ret(0,4)
 elif address==reader:
  dst,n=r(sp+4),r(sp+8);off=r(cx+0x10);assert off+n<=r(cx+0xc)
  u.mem_write(dst,bytes(u.mem_read(r(cx+8)+off,n)));w(cx+0x10,off+n);ret(0,8)
 elif address==0xA70B80:ret(42,4)
 elif address==0xA71E80:ret(model,8)
 elif address==0xA6DEF0:ret(1,4)
 elif address==0xC3CBE0:ret(123,4)
 elif address==0xD2E110:
  output=r(sp+4);w(output,3);ret(output,12)
 elif address==0xC8AD60:ret(actor,4)
 elif address in [0xCE3170,0xC8B230,0xCAFA10,0xCAF9D0]:ret()
 elif address==0xCCC470:ret(0,4)
 elif address==0xC37AD0:ret(cx,4)
 elif address==0xC37B00:ret()
 elif address==0xD2E280:
  # Empty override vector in this fixture; consume its native uint32 count.
  stream=r(sp+4);assert r(r(stream+8)+r(stream+0x10))==0
  w(stream+0x10,r(stream+0x10)+4);ret(0,4)
 elif address==0xF24EF0:
  w(cx+4,0);ret(cx,8) # no Location component for callback fixture
 elif address==0xF26D60:ret() # final renderer update is outside this ABI test
 elif address==0x4024000:ret(0,4) # asset reference-count operation
u.hook_add(UC_HOOK_CODE,hook)
def call(addr,this,args):
 w(stack,stop)
 for i,x in enumerate(args):w(stack+4*(i+1),x)
 u.reg_write(UC_X86_REG_ESP,stack);u.reg_write(UC_X86_REG_ECX,this)
 u.emu_start(addr,stop,count=500000)
 assert u.reg_read(UC_X86_REG_EIP)==stop,hex(u.reg_read(UC_X86_REG_EIP))
 assert u.reg_read(UC_X86_REG_ESP)==stack+4+len(args)*4, 'native stack argument count mismatch'
call(0xF11950,render,[archive,0])
size=r(archive+0x10);raw=bytes(u.mem_read(buffer,size));assert r(archive+0x18)==0
assert raw[:9]==struct.pack('<IBI',0x712123,0,len(raw)-9)
Path(__file__).with_name('native_render_archive.bin').write_bytes(raw)
print('PASS native F11950 -> D1B760 -> D1B300 -> archive writer, bytes',size)
# Remove the transient motion chunk; correct all enclosing lengths exactly as
# CharacterVisualArchive::Normalize does. The real reader must leave it absent.
motion=raw.find(struct.pack('<I',0x17d60c9));assert motion>0
clean=bytearray(raw[:motion]+raw[motion+9:])
for offset in [5,14+5,14+9+1+4+5]:
 struct.pack_into('<I',clean,offset,struct.unpack_from('<I',clean,offset)[0]-9)
Path(__file__).with_name('native_visual_only.bin').write_bytes(clean)
w(archive,vtable);w(vtable+4,reader);w(archive+0x10,0);w(archive+0xc,len(clean));w(archive+0x18,0)
u.mem_write(buffer,bytes(clean));w(render+0x9c,0)
assets=0x4025000;avtable=0x4026000;w(0x19f4d44,assets);w(assets,avtable);w(avtable+0xa4,0x4024000)
call(0xF1DA50,render,[archive])
assert r(archive+0x10)==len(clean),(r(archive+0x10),len(clean))
assert r(archive+0x18)==0
assert r(render+0x9c)==3
assert bytes(u.mem_read(actor+0x124,45))==bytes(range(45))
print('PASS native F1DA50 -> D3E5E0 -> D3E3D0 reader, no motion chunk, new independent render handle')

# Exercise the actual callback epilogue, omitted by the original archive test.
try:
 call(0xF3DBD0,render,[])
except AssertionError as error:
 assert str(error)=='native stack argument count mismatch'
 print('PASS regression reproduced: old zero-argument F3DBD0 call unbalances ESP by 4 bytes')
else:
 raise AssertionError('old signature unexpectedly balanced')
call(0xF3DBD0,render,[0])
print('PASS actual F3DBD0 instructions with corrected one-argument ABI preserve ESP')
