"""Compare the opening squad's wreck order against Gold 2.2 infantry callbacks."""
import argparse
import gzip
import hashlib
from pathlib import Path
import re
import struct
import zlib

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP


def verify(game, single_goal=None, expected=0x0489743f, redirect_tick=None):
    image = (game / 'n2Game_Dll.dll').read_bytes()
    if hashlib.sha256(image).hexdigest() != 'aa3c36fad90a8fb39bc9aa0b55714c433a9d16df1f6d9490bd883c4abbb27f41':
        raise ValueError('Requires GOG build 13718')
    pe = pefile.PE(data=image)
    vm = Uc(UC_ARCH_X86, UC_MODE_32)
    vm.mem_map(0x10000000, (pe.OPTIONAL_HEADER.SizeOfImage+4095) & ~4095)
    vm.mem_write(0x10000000, pe.get_memory_mapped_image())
    desc, stack, end = 0x2000000, 0x201e000, 0x201f000
    vm.mem_map(desc, 0x20000)

    def put(address, fmt, *values):
        vm.mem_write(address, struct.pack('<'+fmt, *values))

    def read(address, size):
        return int.from_bytes(vm.mem_read(address, size), 'little')

    def call(address, *args, unit=0):
        put(stack, 'I'*(1+len(args)), end, *args)
        vm.reg_write(UC_X86_REG_ESP, stack)
        vm.reg_write(UC_X86_REG_ECX, unit)
        vm.emu_start(address, end, count=14000000)
        if vm.reg_read(UC_X86_REG_EIP) != end:
            raise RuntimeError(f'Gold callback at {address:#x} did not return')
        return vm.reg_read(UC_X86_REG_EAX)

    # Isolate movement from fog and the full world's object index. Preserve
    # set_tile's coordinate writes. The actual occupancy commit still runs.
    vtable = 0x100ed128
    def skip_world_index(vm, address, size, data):
        sp = vm.reg_read(UC_X86_REG_ESP)
        arg_bytes = 0
        if address == 0x1004a5b0:
            put(vm.reg_read(UC_X86_REG_ECX)+0x33, 'hh', read(sp+4,4), read(sp+8,4))
            arg_bytes = 8
        vm.reg_write(UC_X86_REG_ESP, sp+4+arg_bytes)
        vm.reg_write(UC_X86_REG_EIP, read(sp,4))
    for address in (read(vtable+0xc8,4), 0x1004a5b0):
        vm.hook_add(UC_HOOK_CODE, skip_world_index, begin=address, end=address)

    with gzip.open(game/'maps/single/--- TUTORIAL I ---.SSM','rb') as f:
        mission = f.read((32<<20)+1)
    if len(mission) > 32<<20 or mission[:8] != b'MS2S\x22\x02\0\0':
        raise ValueError('Unexpected Tutorial I mission')
    layers = {b'P-FLAGS': (0x1034239c,4), b'P-OBSTACLES': (0x1080639c,2)}
    spawns = []
    offset = 8
    while offset < len(mission):
        name, size = struct.unpack_from('<12sI', mission, offset)
        offset += 16
        if size > len(mission)-offset:
            raise ValueError('Truncated mission')
        if name.rstrip() == b'I-UNITS':
            if size < 4:
                raise ValueError('Truncated unit list')
            count, = struct.unpack_from('<I', mission, offset)
            pos, limit = offset+4, offset+size
            for _ in range(count):
                if pos+18 > limit:
                    raise ValueError('Truncated unit record')
                u,v,_,owner = struct.unpack_from('<HHBB', mission, pos+12)
                spawns.append((u,v,owner))
                pos += 18 + 10*(mission[pos+10]+mission[pos+11])
            if pos != limit:
                raise ValueError('Incorrect passenger or crew record size')
        elif name.rstrip() in layers:
            addr, stride = layers.pop(name.rstrip())
            if size != 128*128*stride:
                raise ValueError('Unexpected layer dimensions')
            for row in range(128):
                pos = offset+row*128*stride
                vm.mem_write(addr+row*512*stride, mission[pos:pos+128*stride])
        offset += size
    if layers:
        raise ValueError('Missing terrain layers')

    placements = []
    for line in (game/'maps.src/map.020/mis.000/mapunits').read_text().splitlines():
        fields = dict(re.findall(r'(\w+)=(\S+)',line))
        if 'U' in fields:
            placements.append(tuple(int(fields[k]) for k in ('U','V','Owner')))
    if len(placements) != 96:
        raise ValueError('Unexpected opening placements')
    # The loader reads I-UNITS forward and prepends each object to the live list.
    if placements != list(reversed(spawns)):
        raise ValueError('Tutorial source placements differ from the original live-list order')
    put(0x10142384, 'II', 128,128)
    # Run Gold's map-load perimeter pass before issuing orders on the SSM terrain.
    vm.reg_write(UC_X86_REG_ESP, stack)
    vm.emu_start(0x1006b2ec,0x1006b448,count=100000)
    if vm.reg_read(UC_X86_REG_EIP) != 0x1006b448:
        raise RuntimeError('Map perimeter initialization did not finish')
    put(0x10dd4ff0, 'I', 0x76321f)
    put(desc+0x31b, 'H', 0xf0)
    put(desc+0x4e0, 'II', 128,12)
    put(desc+0x788, '4i', 8,0,5,0)
    if redirect_tick is not None:
        put(desc+0x20, 'I', 0x200008)
        formation = 0x3000000
        vm.mem_map(formation, 0x2000)
    squad = []
    for i,(u,v,owner) in enumerate(placements):
        unit = desc+0x2000+i*0x400
        x, y, slot = u//32, v//32, (u//16)%2+2*((v//16)%2)
        put(unit,'IIH',vtable,desc,i+1)
        put(unit+10,'I',unit+0x400 if i+1<len(placements) else 0)
        put(unit+0x33,'hh',x,y)
        put(unit+0xa2,'B',slot)
        put(unit+0x9c,'hhBB',x,y,4,0)
        call(0x10036370,x,y,slot,0,unit=unit+0x9c)
        if owner == 0 and (single_goal is None or (u,v) == (1096,3576)):
            squad.append(unit)
            goal = (x+3,y-39) if single_goal is None else single_goal
            put(unit+0xa4,'Bhh',1,*goal)
            put(unit+0xb0,'I',0x7fffff)
            put(unit+0xbe,'B',255)
    put(0x1010f258,'I',desc+0x2000)
    active = set(squad)
    crc = 0
    ticks = 3200 if single_goal is None else 6300
    if redirect_tick is not None:
        ticks = 5500
    for tick in range(ticks):
        if tick == redirect_tick:
            # Ground pixel (3601,1600) in the second objective's fields.
            call(0x10084500,42,57,unit=formation)
            for u in squad:
                call(0x10084550,u,unit=formation)
            call(0x100845b0,unit=formation)
            for u in squad:
                goal_x, goal_y = formation+0x1000, formation+0x1004
                call(0x10084640,u,goal_x,goal_y,0xffffffff,0,unit=formation)
                x,y = read(goal_x,4), read(goal_y,4)
                if max(x,y) >= 128:
                    raise RuntimeError('Gold assigned a destination outside the tutorial map')
                put(u+0x25,'Ihh',2,x*32+15,y*32+15)
                call(0x100369d0,unit=u)
            active = set(squad)
        for u in squad:
            if u not in active:
                continue
            put(0x10102250,'I',u)
            backoff = read(u+0xe0,1)
            if backoff:
                put(u+0xe0,'B',backoff-1)
            if read(u+0xec,1) == 2:
                call(0x100200e0,unit=u)
                if read(u+0xec,1) == 2:
                    continue
            call(0x10021a50,unit=u)
            if call(0x10036a90,2,unit=u):
                active.remove(u)
            elif read(0x1010f2bd,1) & 1:
                call(0x10021b70,unit=u)
        if any(read(u+0xa9,1)==1 and not read(u+0xe0,1) for u in active):
            call(0x10058570)
        positions = bytearray()
        for u in squad:
            put(0x10102250,'I',u)
            call(0x10021810,unit=u)
            x,y = struct.unpack('<II', vm.mem_read(0x1010f264,8))
            positions += struct.pack('<HH',4096+x-y,(x+y)//2)
        crc = zlib.crc32(positions,crc)
    label = 'Wreck convoy' if single_goal is None else f'Long march to {single_goal}'
    if redirect_tick is not None:
        label = f'Wreck convoy redirected to fields at tick {redirect_tick}'
    print(f'{label}: {len(squad)*ticks} rendered positions, CRC32 {crc:08x}')
    if active or crc != expected:
        raise RuntimeError(f'{label} differs from the native movement regression')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--long-marches', action='store_true', help='Also replay three 6,300-tick single-soldier orders')
    parser.add_argument('--inflight-redirect', action='store_true', help='Also verify five timings for the second squad order to the fields')
    args = parser.parse_args()
    verify(args.game)
    if args.inflight_redirect:
        for tick, checksum in [(350,0x1845fbaa), (1700,0xb462c7a0), (1872,0x3fbc7902),
                               (2108,0xaf716304), (3200,0x99b4fccd)]:
            verify(args.game, expected=checksum, redirect_tick=tick)
    if args.long_marches:
        for goal, checksum in [((34,35),0xd60f5668), ((75,35),0xe3ff723a), ((42,42),0x2ca70725)]:
            verify(args.game,goal,checksum)
