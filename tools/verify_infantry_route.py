"""Check the retail tutorial route, substep and compact formation routines."""
import argparse
import gzip
import hashlib
from pathlib import Path
import re
import struct
import zlib

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP


def run(game, all_zone=False):
    image = (game / 'n2Game_Dll.dll').read_bytes()
    if hashlib.sha256(image).hexdigest() != 'aa3c36fad90a8fb39bc9aa0b55714c433a9d16df1f6d9490bd883c4abbb27f41':
        raise ValueError('Requires GOG build 13718')
    pe = pefile.PE(data=image)
    vm = Uc(UC_ARCH_X86, UC_MODE_32)
    vm.mem_map(0x10000000, (pe.OPTIONAL_HEADER.SizeOfImage+4095) & ~4095)
    vm.mem_write(0x10000000, pe.get_memory_mapped_image())
    unit, desc, stack, end = 0x2000000, 0x2001000, 0x200e000, 0x200f000
    vm.mem_map(unit, 0x10000)

    def put(addr, fmt, *values):
        vm.mem_write(addr, struct.pack('<'+fmt, *values))

    def word(addr):
        return int.from_bytes(vm.mem_read(addr, 4), 'little')

    def call(address, *args, this=unit):
        put(stack, 'I'*(1+len(args)), end, *args)
        vm.reg_write(UC_X86_REG_ESP, stack)
        vm.reg_write(UC_X86_REG_ECX, this)
        vm.emu_start(address, end, count=14000000)
        if vm.reg_read(UC_X86_REG_EIP) != end:
            raise RuntimeError('Retail route did not return')

    def occupy(x, y, present):
        cell = (y//2)*512 + x//2
        shift = 4*(x%2 + 2*(y%2))
        addr = 0x1044239c + 4*cell
        put(addr, 'I', (word(addr) & ~(15<<shift)) | (int(present)<<shift))
        occupied = word(addr)
        mask = sum(1 << slot for slot in range(4) if occupied & (15 << (4*slot)))
        put(0x1070239c+cell, 'B', mask)

    with gzip.open(game/'maps/single/--- TUTORIAL I ---.SSM', 'rb') as file:
        data = file.read((32<<20)+1)
    if len(data) > 32<<20 or data[:8] != b'MS2S\x22\x02\0\0':
        raise ValueError('Unexpected mission')
    layers = {b'P-FLAGS     ': (0x1034239c, 4), b'P-OBSTACLES ': (0x1080639c, 2)}
    zones = None
    offset = 8
    while offset < len(data):
        label, size = struct.unpack_from('<12sI', data, offset)
        offset += 16
        if size > len(data)-offset:
            raise ValueError('Truncated mission')
        if label.rstrip() == b'I-ZONES':
            zones = data[offset:offset+size]
        if label in layers:
            addr, stride = layers.pop(label)
            if size != 128*128*stride:
                raise ValueError('Unexpected terrain dimensions')
            for row in range(128):
                begin = offset+row*128*stride
                vm.mem_write(addr+row*512*stride, data[begin:begin+128*stride])
        offset += size
    if layers:
        raise ValueError('Missing terrain layers')
    squad = []
    for line in (game/'maps.src/map.020/mis.000/mapunits').read_text().splitlines():
        fields = dict(re.findall(r'(\w+)=(\S+)', line))
        if 'U' in fields:
            cell = int(fields['U'])//16, int(fields['V'])//16
            occupy(*cell, True)
            if fields['Owner'] == '0':
                squad.append(cell)

    put(0x10142384, 'II', 128, 128)
    # The XCHNG map loader seals the outer four tiles in the runtime flags.
    vm.reg_write(UC_X86_REG_ESP, stack)
    vm.emu_start(0x1006b2ec,0x1006b448,count=100000)
    if vm.reg_read(UC_X86_REG_EIP) != 0x1006b448:
        raise RuntimeError('Map perimeter initialization did not finish')
    put(unit+4, 'I', desc)
    put(desc+0x31b, 'H', 0xf0)
    # Gold's standard ground-order dispatcher at 0x100866c0 uses these helpers.
    # The selected units stand far from the test clicks. Omit only their per-unit
    # source-footprint hide/restore callbacks and the visual feedback callback.
    ctx, vtable, noop = unit+0x2000, unit+0x3000, unit+0x4000
    out_x, out_y = unit+0x5000, unit+0x5004
    vm.mem_write(noop, bytes([0xc3]))
    put(vtable+0xd0, 'II', noop, noop)
    put(unit, 'I', vtable)
    put(desc+0x20, 'I', 0x200008)  # Infantry base constructor and ground movement.
    for gx, gy in ((37,73), (37,72), (37,74), (34,81)):
        call(0x10084500, gx, gy, this=ctx)
        for x, y in squad:
            put(unit+0x33, 'hh', x//2, y//2)
            call(0x10084550, unit, this=ctx)
        call(0x100845b0, this=ctx)
        cx = sum(x//2 for x,_ in squad)//len(squad)
        cy = sum(y//2 for _,y in squad)//len(squad)
        if word(ctx+36) != 0:
            raise RuntimeError('Unexpected formation mode')
        for x, y in squad:
            put(unit+0x33, 'hh', x//2, y//2)
            put(unit+0xa2, 'B', x%2+2*(y%2))
            call(0x10084640, unit, out_x, out_y, 0xffffffff, 0, this=ctx)
            if (word(out_x),word(out_y)) != (gx+x//2-cx,gy+y//2-cy):
                raise RuntimeError('Group-command destination differs from the compact formation')
    print(f'Retail formation matched all {len(squad)*4} individual destinations')
    def accepted_route(sx, sy, gx, gy):
        occupy(sx, sy, False)
        vm.mem_write(unit+0xa9, bytes(80))
        put(unit+0xb0, 'I', 0x7fffff)
        put(unit+0x33, 'hh', sx//2, sy//2)
        put(unit+0xa2, 'B', sx%2+2*(sy%2))
        call(0x1008bda0, unit, sx//2, sy//2, gx, gy)
        route = vm.mem_read(unit+0xa9, 64)
        tile_dirs = [(route[0x16+i//2] >> (0 if i%2 else 4)) & 15 for i in range(route[0x14])]
        accepted = []
        last = (sx, sy)
        for direction in tile_dirs:
            for _ in range(4):
                call(0x100218d0, direction)
                if not vm.reg_read(UC_X86_REG_EAX):
                    occupy(sx, sy, True)
                    return accepted, last
                accepted.append(word(0x1010f2be) & 255)
                tx, ty = word(0x1010f2c2), word(0x1010f2c6)
                slot = word(0x1010f2ca) & 255
                put(unit+0x33, 'hh', tx, ty)
                put(unit+0xa2, 'B', slot)
                last = (2*tx + slot%2, 2*ty + slot//2)
                if vm.mem_read(0x1010f2bd, 1)[0] & 2:
                    break
            else:
                raise RuntimeError('Tile transition did not terminate')
        occupy(sx, sy, True)
        return accepted, last

    # Original ordinary movement order 2 reaches 0x100369d0 with world coordinates.
    # A new tile replaces the queue. A repeated tile preserves its cursor and data.
    for progress in (0, 1, 129, 255):
        for gx, gy in ((37, 73), (36, 74)):
            put(unit+0x25, 'Ihh', 2, gx*32, gy*32)
            put(unit+0xa4, 'Bhh', 1, 37, 73)
            put(unit+0xa9, 'B', 3)
            put(unit+0xbc, 'BBB', 1, 5, 255)
            put(unit+0xec, 'BB', 2, progress)
            call(0x100369d0)
            if vm.mem_read(unit+0xec, 2) != bytes((2, progress)):
                raise RuntimeError('Retarget interrupted the current RUN substep')
            expected = (3, 1, 5) if (gx, gy) == (37, 73) else (0, 0, 0)
            actual = tuple(vm.mem_read(unit+offset, 1)[0] for offset in (0xa9, 0xbc, 0xbd))
            if actual != expected or tuple(struct.unpack('<hh', vm.mem_read(unit+0xa5, 4))) != (gx, gy):
                raise RuntimeError('Unexpected retarget queue state')
    print('Retail kept the active RUN step for eight same-tile and changed-tile reorders')

    if all_zone:
        if zones is None or len(zones) != 128*128:
            raise ValueError('Missing location grid')
        goals = [(i%128, i//128) for i, value in enumerate(zones) if value & 63 == 2]
        starts = sorted(squad)
        for spread in (False, True):
            positions = [(x, y-60 if spread and i%2 == 0 else y) for i,(x,y) in enumerate(starts)]
            for x,y in starts:
                occupy(x,y,False)
            for x,y in positions:
                occupy(x,y,True)
            crc = 0
            for gx,gy in goals:
                call(0x10084500,gx,gy,this=ctx)
                for x,y in positions:
                    put(unit+0x33,'hh',x//2,y//2)
                    call(0x10084550,unit,this=ctx)
                call(0x100845b0,this=ctx)
                for x,y in positions:
                    put(unit+0x33,'hh',x//2,y//2)
                    put(unit+0xa2,'B',x%2+2*(y%2))
                    occupy(x,y,False)
                    call(0x10084640,unit,out_x,out_y,0xffffffff,0,this=ctx)
                    occupy(x,y,True)
                    crc = zlib.crc32(bytes((word(out_x),word(out_y))),crc)
            if crc != (0x72fc5094 if spread else 0xc843ae03):
                raise RuntimeError('Group formation differs from native regression')
            print(f'Formation spread={spread}, {len(positions)*len(goals)} assignments, CRC32 {crc:08x}', flush=True)
            for x,y in positions:
                occupy(x,y,False)
            for x,y in starts:
                occupy(x,y,True)
        checksum = 0
        for sx, sy in sorted(squad):
            for gx, gy in goals:
                directions, _ = accepted_route(sx, sy, gx, gy)
                checksum = zlib.crc32(bytes(directions) + b'\xff', checksum)
        print(f'Opening occupancy, {len(squad)*len(goals)} routes, CRC32 {checksum:08x}', flush=True)

        if len(squad)*len(goals) != 7366 or checksum != 0xc67b6a1b:
            raise RuntimeError('Opening objective route fingerprint differs from the native regression')

    for tile_x in (34, 35):
        sx, sy = tile_x*2, 223
        accepted, last = accepted_route(sx, sy, tile_x, 81)
        if accepted != [6]*60:
            raise RuntimeError(f'Unexpected route for tile {tile_x}: {accepted}')
        occupy(sx, sy, False)
        occupy(*last, True)
    print('Retail route search and substep acceptance matched all 120 expected directions')


    # Compare the separate live footprint and scheduler footprint on a clear strip.
    vm.mem_write(0x1044239c, bytes(128*512*4))
    vm.mem_write(0x1070239c, bytes(128*512))
    def start_on_strip():
        vm.mem_write(unit+0xa9, bytes(80))
        put(unit+0xb0, 'I', 0x7fffff)
        put(unit+0x33, 'hh', 30, 60)
        put(unit+0xa2, 'B', 1)
        occupy(61, 120, True)
    def search(tx, goal):
        put(0x10dd4ff0, 'I', 0x76321f)
        call(0x1008bda0, unit, tx, 60, goal, 60)
        q = vm.mem_read(unit+0xa9, 64)
        directions = [(q[22+i//2] >> (0 if i%2 else 4)) & 15 for i in range(q[19],q[20])]
        return q[0], int.from_bytes(q[5:7], 'little'), directions, q[54], q[55]
    start_on_strip()
    expected = (3,0,[0,0,0],0,0)
    if search(30,33) != expected:
        raise RuntimeError('Initial convoy queue differs')
    occupy(62,120,True)
    occupy(62,121,True)
    put(0x1070239c+60*512+31, 'B', 0)
    if search(30,33) != (1,2,[0,0,0],0,0):
        raise RuntimeError('Transient blockage did not retain the same queue')
    put(0x1070239c+60*512+31, 'B', 5)
    if search(30,33) != (3,0,[7,1,0],0,0):
        raise RuntimeError('Promoted blocker did not cause the observed detour')
    occupy(62,120,False)
    occupy(62,121,False)
    start_on_strip()
    for slot in range(4):
        occupy(64+(slot&1),120+(slot>>1),True)
    if search(30,32) != (3,0,[0],0,0):
        raise RuntimeError('Unexpected approach to an occupied goal')
    occupy(61,120,False)
    occupy(63,120,True)
    put(unit+0x33, 'hh', 31, 60)
    put(unit+0xbc, 'B', 1)
    for attempt in range(1,5):
        if search(31,32) != (2 if attempt == 4 else 1, 0, [], 41*attempt, 27):
            raise RuntimeError('Occupied goal backoff or terminal state differs')
    print('Retail matched retained queue, detour, four backoffs and terminal blockage')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--all-zone', action='store_true', help='Also fingerprint every starting soldier against every objective tile')
    args = parser.parse_args()
    run(args.game, args.all_zone)
