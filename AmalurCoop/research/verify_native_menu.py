"""Source-only menu checks using Lua 5.1 and a bounded HKS chunk reader."""
from pathlib import Path
import sys,struct,re

from lupa.lua51 import LuaRuntime
r=Path(__file__).resolve().parents[1]
source=(r/'MainMenuScript.h').read_text().split('R"AmalurLua(',1)[1].split(')AmalurLua"',1)[0]
lua=LuaRuntime(unpack_returned_tuples=True)
lua.execute('''
commands = {}; updates = 0; changed = {}; current_status = "Offline"
function print(value)
  commands[#commands+1] = value
  if value == "__AmalurCoop:host" then return "Starting host..." end
  if value == "__AmalurCoop:join" then return "Connecting..." end
  if value == "__AmalurCoop:status" then return current_status end
end
main_menu = {
 get_button_tree_info = function(arg) return { title=arg, children={{id_string=657596, title="Continue"}} } end,
 on_update_event = function(...) updates=updates+1 end,
 m_button_tree = { change_button_title=function(self,id,text) changed[id]=text end }
}
''')
lua.globals().fixture_source = source
lua.execute('setmetatable(main_menu, {__index=_G}); setfenv(assert(loadstring(fixture_source)), main_menu)(); assert(_G.amalur_coop_menu and rawget(main_menu, "amalur_coop_menu") == nil)')
lua.execute('''
assert(commands[1] == "__AmalurCoop:ready")
local root = main_menu.get_button_tree_info("Original")
assert(root.title == "Original" and #root.children == 2)
assert(root.children[1].title == "Continue")
assert(root.children[2].module == "amalur_coop_menu")
local mp = _G[root.children[2].module].get_button_tree_info()
assert(mp.title == "Multiplayer" and mp.children[1].title == "Host" and mp.children[2].title == "Join")
mp.children[1].override(mp.children[1]); assert(commands[2] == "__AmalurCoop:host" and changed[0x414304] == "Starting host...")
mp.children[2].override(mp.children[2]); assert(commands[3] == "__AmalurCoop:join" and changed[0x414304] == "Connecting...")
current_status = "Connected to host"
for i=1,60 do main_menu.on_update_event() end
assert(updates == 60 and changed[0x414304] == current_status)
assert(#main_menu.get_button_tree_info().children == 2)
''')
lua.globals().fixture_source = source
lua.execute('setmetatable(main_menu, {__index=_G}); setfenv(assert(loadstring(fixture_source)), main_menu)(); assert(_G.amalur_coop_menu and rawget(main_menu, "amalur_coop_menu") == nil)');lua.execute('assert(#commands == 4 and #main_menu.get_button_tree_info().children == 2)')
print('PASS Lua 5.1 menu: preserves original entries, native submenu/Host/Join callbacks, live status, original update handler and reload idempotency')
b=Path(sys.argv[1]).read_bytes();b=b[b.index(b'\x1bLua'):]
h=2166136261
for x in b:h=((h^x)*16777619)&0xffffffff
profiles = {(56302,0x1898df81):(713,304,8013,66), (56770,0x83744cb7):(719,307,8125,67)}
nc,nk,kend,nchildren = profiles[(len(b),h)]
ret = 271 + (nc-1)*4
kstart = ret+8
be=lambda v:struct.pack('>I',v)
def st(s):s=s.encode()+b'\0';return b'\x04'+be(len(s))+s
abc=lambda op,a,b,c:be((op<<25)|(b<<17)|(c<<8)|a)
abx=lambda op,a,bx:be((op<<25)|(bx<<8)|a)
bootstrap=b''.join([abx(85,0,nk+4),abx(70,20,nk+5),abx(85,1,nk),abx(70,20,nk+1),abx(25,2,nk+3),abc(2,1,2,2),abc(2,0,2,1),abc(9,0,1,0)])
patched=b[:267]+be(nc+7)+b[271:ret]+bootstrap+be(nk+7)+b[kstart:kend]+st('loadstring')+b'\0\0'+st(source)+st('pcall')+b'\0\0'+b[kend:]
class Reader:
 def __init__(self,data):self.data=data;self.pos=238
 def u(self):v=struct.unpack_from('>I',self.data,self.pos)[0];self.pos+=4;return v
 def byte(self):v=self.data[self.pos];self.pos+=1;return v
 def string(self):n=self.u();v=self.data[self.pos:self.pos+n];self.pos+=n;return v
 def function(self):
  src=self.string();name=self.string();first,last,nups,params=self.u(),self.u(),self.u(),self.u();var=self.byte();stack=self.u();code=[self.u() for _ in range(self.u())];k=[]
  for _ in range(self.u()):
   tag=self.byte()
   if tag==0:v=None
   elif tag==1:v=self.byte()
   elif tag==3:v=self.u()
   elif tag==4:v=self.string()
   else:raise AssertionError(tag)
   k.append(v)
  children=[self.function() for _ in range(self.u())]
  lines=[self.u() for _ in range(self.u())];locals=[(self.string(),self.u(),self.u()) for _ in range(self.u())];ups=[self.string() for _ in range(self.u())]
  return (code,k,children)
a=Reader(b);original=a.function();z=Reader(patched);result=z.function()
assert original[0][:-1]==result[0][:-8] and original[1]==result[1][:nk] and original[2]==result[2]
assert b[a.pos:]==patched[z.pos:]
assert result[1][nk]==b'loadstring\0' and result[1][nk+3]==source.encode()+b'\0' and result[1][nk+4]==b'pcall\0'
# Evaluate the eight inserted bootstrap instructions against stub loadstring/pcall.
regs={};calls=[];pc=nc-1
while pc<len(result[0]):
 ins=result[0][pc];op=ins>>25;ra=ins&255;rb=(ins>>17)&255;rc=(ins>>8)&511;bx=(ins>>8)&0x1ffff
 if op==85:regs[ra]=result[1][bx].rstrip(b'\0').decode()
 elif op==25:regs[ra]=result[1][bx].rstrip(b'\0').decode()
 elif op==2:
  calls.append((regs[ra],regs.get(ra+1)))
  if regs[ra]=='loadstring':regs[ra]='compiled menu patch'
 elif op==9:break
 else:assert op==70
 pc+=1
assert calls==[('loadstring',source),('pcall','compiled menu patch')]
assert len(result[2]) == nchildren
print(f'PASS verified HKS chunk patch: all {nchildren} original child functions preserved, constants/code counts valid, bootstrap calls loadstring then pcall')
