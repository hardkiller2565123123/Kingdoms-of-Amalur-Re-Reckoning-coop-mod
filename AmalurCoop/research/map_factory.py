import pefile,capstone,json,struct
from pathlib import Path
p=pefile.PE(r'D:\SteamLibrary\steamapps\common\Kingdoms of Amalur Re-Reckoning\koa.exe');base=0x400000
c=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);c.detail=True
vt={int(r['vtable'],16):r for r in json.load(open(Path(__file__).with_name('component_vtables.json'))) if 'Blueprint' not in r['name']}
def instructions(a,size=0x800):
 for i in c.disasm(p.get_data(a-base,size),a):
  if i.mnemonic in ['ret','int3']:break
  yield i
def trace(a,depth=0):
 out=[]
 for i in instructions(a):
  if i.mnemonic=='mov' and len(i.operands)==2 and i.operands[1].type==capstone.x86.X86_OP_IMM and i.operands[1].imm in vt:
   out.append(vt[i.operands[1].imm])
  if i.mnemonic=='call' and depth<2 and i.operands[0].type==capstone.x86.X86_OP_IMM and 0xEF0000<i.operands[0].imm<0xFB0000:
   out+=trace(i.operands[0].imm,depth+1)
 return out
rs=[]
for idx in range(43):
 a=struct.unpack('<I',p.get_data(0xFA61B8-base+4*idx,4))[0]
 ins=list(instructions(a,0x80)); alloc=next((i.operands[0].imm for i in ins if i.mnemonic=='push' and i.operands[0].type==capstone.x86.X86_OP_IMM),0)
 names=trace(a)
 r=dict(id=idx,branch=hex(a),size=hex(alloc),types=[x['name'] for x in names],vtables=[x['vtable'] for x in names]);rs.append(r)
 print(idx,r['size'],r['types'],r['vtables'])
Path(__file__).with_name('factory_map.json').write_text(json.dumps(rs,indent=2))
