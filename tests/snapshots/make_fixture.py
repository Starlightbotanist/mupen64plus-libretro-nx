"""Generate an original N64 RDP/RSP history fixture; no game or IPL firmware."""
import argparse
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--shaded', action='store_true', help='retain subpixel shaded triangles for GPU tests')
args = parser.parse_args()
code, labels, branches = [], {}, []

def emit(word): code.append(word & 0xffffffff)
def li(reg, value):
    emit(0x3c000000 | reg << 16 | (value >> 16 & 0xffff))
    emit(0x34000000 | reg << 21 | reg << 16 | (value & 0xffff))
def write(address, value):
    li(8, address); li(9, value); emit(0xad090000)
def mark(name):
    assert name not in labels
    labels[name] = len(code)
def branch(reg, name, nonzero=False):
    branches.append((len(code), name))
    emit((0x14000000 if nonzero else 0x10000000) | reg << 21)
    emit(0)
def poll(address, mask, nonzero, name):
    li(8, address); li(10, mask); mark(name)
    emit(0x8d090000); emit(0x012a4824)
    branch(9, name, nonzero)
def rsp(words, name):
    for index, word in enumerate(words):write(0xa4001000 + index * 4, word)
    write(0xa4080000, 0)
    write(0xa4040010, 0x10d)  # clear halt/broke/interrupt, enable break interrupt
    poll(0xa4040010, 1, False, name)
def submit(words, name):
    for index, word in enumerate(words):write(0xa0003000 + index * 4, word)
    write(0xa4300000, 0x800)  # clear DP interrupt
    write(0xa410000c, 1)      # clear XBUS: commands reside in RDRAM
    write(0xa4100000, 0x3000)
    write(0xa4100004, 0x3000 + len(words) * 4)
    poll(0xa4300008, 0x20, False, name)

# The original IPL stub copies the guest from SP DMEM to uncached RDRAM.
write(0xa0080004,1)
li(8, 0x34000000); emit(0x40886000); emit(0)  # CP0 status, interrupts off
for offset, value in [(4,0x200000),(8,320),(12,512),(20,0x03e52239),
                      (24,525),(28,0x0c15),(32,0x0c150c15),(36,0x006c02ec),
                      (40,0x002501ff),(44,0x000e0204),(48,0x200),(52,0x400),(0,0x3002)]:
    write(0xa4400000 + offset, value)
initial = [0xff10013f,0x200000,0xed000000,0x005003c0,0xef300000,0,
           0xf7000000,0xf801f801,0xf64fc3bc,0,
           0xe7000000,0,0xf7000000,0x07c107c1,0xf64fc0fc,0,
           0xe7000000,0,0xf7000000,0x003f003f,0xf60fc3bc,0x00000100,
           0xe7000000,0,0xf7000000,0x40004000,0xf6004000,0,
           0xe9000000,0]
submit(initial, 'initial_rdp')
if args.shaded:
    # Unlike fill rectangles, triangles populate ParaLLEl-RDP's enhanced buffers.
    # Pack RGBA 16.16 integer/fraction halves in the RDP attribute order.
    shade = [(64 << 16) | 96, (160 << 16) | 255, 5 << 16, 0,
             0, 0, 16384 << 16, 0] + [0] * 8
    triangle = [0xe7000000, 0, 0xef0000f0, 0, 0xfc000000, 0x00020904,
                0xcc800000 | 529, (529 << 16) | 401,
                0x003a4000, 0, 0x00324000, 0, 0x00324000, 0x00004000,
                *shade, 0xe9000000, 0]
    assert len(triangle) == 32
    for index, word in enumerate(triangle):write(0xa0004000 + index * 4, word)
write(0xa0080004,2)
rsp([0x24101000,0x48900000,0x48d00000,0x0000000d], 'initial_rsp')
write(0xa0080004,3)
li(21,0)
mark('frame')
li(8,0xa4400010); li(10,480)
mark('late_scanline'); emit(0x8d090000); emit(0x012a582b); branch(11,'late_scanline',True)
mark('early_scanline'); emit(0x8d090000); emit(0x012a582b); branch(11,'early_scanline',False)
write(0xa0080004,4)

# Consume the retained RDP fill-color register before replacing it. Restoring
# RDRAM alone cannot repair a missing private renderer restore.
submit([0xe7000000,0,0xef300000,0,0xf6004000,0,0xe9000000,0], 'read_retained_rdp')
li(19,0xa0100040)
emit(0x26aa0001); emit(0x314a0001); emit(0x000a5100); emit(0x026a9821)
li(8,0xa0200000); emit(0x8d090000); emit(0xae690000)
rsp([0xac100000,0x48080000,0x48490000,0xac080004,0xac090008,0x0000000d], 'read_rsp')
li(9,0xa4000000)
for index in range(3):emit(0x8d280000 | index * 4); emit(0xae680000 | (index + 1) * 4)

# Replace private scalar/vector/flag state only after its old value is read.
write(0xa4001000,0)
li(8,0xa4001000); emit(0x32a9001f); emit(0x35291000)
li(10,0x24100000); emit(0x012a4825); emit(0xad090000)
for index, word in enumerate([0x48900000,0x48d00000,0x0000000d],1):write(0xa4001000 + index * 4,word)
write(0xa4080000,0); write(0xa4040010,0x10d)
poll(0xa4040010,1,False,'write_rsp')

words=[0xe7000000,0,0xf7000000,0,
       0xf6004000,0,0xe7000000,0,0xf60fc0fc,0x00080080,0xe9000000,0]
for index, word in enumerate(words):write(0xa0003000 + index * 4,word)
li(8,0xa000300c); emit(0x32a9001f); emit(0x35294000)
emit(0x00095400); emit(0x012a4825); emit(0xad090000)
# Two-pixel columns revisit after sixteen frames with a different color.
# Speculative draws therefore alter pixels outside the next canonical column.
emit(0x32a9000f); emit(0x00094bc0)  # (frame & 15) << 15
for offset,base in [(0x3020,0xf60840fc),(0x3024,0x00080080)]:
    li(8,0xa0000000+offset); li(10,base); emit(0x012a5025); emit(0xad0a0000)
write(0xa4300000,0x800); write(0xa4100000,0x3000); write(0xa4100004,0x3030)
poll(0xa4300008,0x20,False,'frame_rdp')
if args.shaded:
    # Revisit sixteen separate triangles, changing their shade on each visit.
    # A canonical draw cannot repair pixels changed by a discarded future.
    emit(0x32a9000f); emit(0x00094cc0)  # (frame & 15) << 19
    for index, base in [(8,0x003a4000),(10,0x00324000),(12,0x00324000)]:
        li(8,0xa0004000+index*4); li(10,base); emit(0x012a5021); emit(0xad0a0000)
    li(8,0xa0004038); emit(0x32a9001f); emit(0x00094c00)
    li(10,(64 << 16) | 96); emit(0x012a4825); emit(0xad090000)
    write(0xa4300000,0x800); write(0xa4100000,0x4000); write(0xa4100004,0x4080)
    poll(0xa4300008,0x20,False,'frame_shaded')
emit(0x26b50001); li(8,0xa0080000); emit(0xad150000)  # publish complete record
branch(0,'frame')

for at, name in branches:
    offset=labels[name]-at-1
    assert -32768<=offset<=32767
    code[at]|=offset & 0xffff
guest=struct.pack('>'+str(len(code))+'I',*code)
assert len(guest)<=0xf00
boot=[]
def boot_li(reg,value):boot.extend([0x3c000000|reg<<16|(value>>16&0xffff),0x34000000|reg<<21|reg<<16|(value&0xffff)])
boot_li(8,0xa4000100); boot_li(9,0xa0001000); boot_li(10,len(guest))
boot.extend([0x8d0b0000,0xad2b0000,0x25080004,0x25290004,0x254afffc,0x1540fffa,0])
boot_li(8,0xa0001000); boot.extend([0x01000008,0])
rom=bytearray(8192)
struct.pack_into('>III',rom,0,0x80371240,0x0000000f,0x80001000)
rom[0x20:0x34]=b'SNAPSHOT HISTORY'.ljust(20, b' ')
rom[0x3e]=ord('E')
rom[0x40:0x40+len(boot)*4]=struct.pack('>'+str(len(boot))+'I',*boot)
rom[0x100:0x100+len(guest)]=guest
with args.output.open('xb') as target:target.write(rom)
print(args.output, len(code), 'guest instructions')
