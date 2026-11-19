"""Run retail route search and substep acceptance for the two objective-test clicks."""
import argparse
import gzip
import hashlib
from pathlib import Path
import re
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP


def run(game):
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

    def call(address, *args):
        put(stack, 'I'*(1+len(args)), end, *args)
        vm.reg_write(UC_X86_REG_ESP, stack)
        vm.reg_write(UC_X86_REG_ECX, unit)
        vm.emu_start(address, end, count=14000000)
        if vm.reg_read(UC_X86_REG_EIP) != end:
            raise RuntimeError('Retail route did not return')

    def occupy(x, y, present):
        cell = (y//2)*512 + x//2
        shift = 4*(x%2 + 2*(y%2))
        addr = 0x1044239c + 4*cell
        put(addr, 'I', (word(addr) & ~(15<<shift)) | (int(present)<<shift))
        put(0x1070239c+cell, 'B', bool(word(addr) & 0xffff))

    with gzip.open(game/'maps/single/--- TUTORIAL I ---.SSM', 'rb') as file:
        data = file.read((32<<20)+1)
    if len(data) > 32<<20 or data[:8] != b'MS2S\x22\x02\0\0':
        raise ValueError('Unexpected mission')
    layers = {b'P-FLAGS     ': (0x1034239c, 4), b'P-OBSTACLES ': (0x1080639c, 2)}
    offset = 8
    while offset < len(data):
        label, size = struct.unpack_from('<12sI', data, offset)
        offset += 16
        if size > len(data)-offset:
            raise ValueError('Truncated mission')
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
    for line in (game/'maps.src/map.020/mis.000/mapunits').read_text().splitlines():
        fields = dict(re.findall(r'(\w+)=(\S+)', line))
        if 'U' in fields:
            occupy(int(fields['U'])//16, int(fields['V'])//16, True)

    put(0x10142384, 'II', 128, 128)
    put(unit+4, 'I', desc)
    put(desc+0x31b, 'H', 0xf0)
    for tile_x in (34, 35):
        sx, sy = tile_x*2, 223
        occupy(sx, sy, False)
        vm.mem_write(unit+0xa5, bytes(80))
        put(unit+0xac, 'I', 0x7fffff)
        put(unit+0x33, 'hh', sx//2, sy//2)
        put(unit+0xa2, 'B', sx%2+2*(sy%2))
        call(0x10088700, unit, sx//2, sy//2, tile_x, 81)
        route = vm.mem_read(unit+0xa5, 64)
        tile_dirs = [(route[0x16+i//2] >> (0 if i%2 else 4)) & 15 for i in range(route[0x14])]
        accepted = []
        for direction in tile_dirs:
            for _ in range(4):
                call(0x100218d0, direction)
                if not vm.reg_read(UC_X86_REG_EAX):
                    raise RuntimeError('Retail rejected step')
                accepted.append(word(0x1010f2be) & 255)
                tx, ty = word(0x1010f2c2), word(0x1010f2c6)
                slot = word(0x1010f2ca) & 255
                put(unit+0x33, 'hh', tx, ty)
                put(unit+0xa2, 'B', slot)
                if vm.mem_read(0x1010f2bd, 1)[0] & 2:
                    break
            else:
                raise RuntimeError('Tile transition did not terminate')
        if accepted != [6]*60:
            raise RuntimeError(f'Unexpected route for tile {tile_x}: {accepted}')
        occupy(2*tx + slot%2, 2*ty + slot//2, True)
    print('Retail route search and substep acceptance matched all 120 expected directions')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    run(parser.parse_args().game)
