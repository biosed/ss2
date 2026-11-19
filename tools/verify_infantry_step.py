"""Trace an accepted soldier step in the licensed DLL, including its real RNG."""
import argparse
import hashlib
from pathlib import Path
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP


def trace(root):
    data = (root / 'n2Game_Dll.dll').read_bytes()
    if hashlib.sha256(data).hexdigest() != 'aa3c36fad90a8fb39bc9aa0b55714c433a9d16df1f6d9490bd883c4abbb27f41':
        raise ValueError('Requires GOG 13718')
    pe = pefile.PE(data=data)
    vm = Uc(UC_ARCH_X86, UC_MODE_32)
    vm.mem_map(0x10000000, (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & ~4095)
    vm.mem_write(0x10000000, pe.get_memory_mapped_image())
    unit, resource, sp, finish = 0x2000000, 0x2001000, 0x200e000, 0x200f000
    vm.mem_map(unit, 0x10000)

    def put(addr, fmt, *values):
        vm.mem_write(addr, struct.pack('<'+fmt, *values))

    def call(address):
        put(sp, 'I', finish)
        vm.reg_write(UC_X86_REG_ESP, sp)
        vm.reg_write(UC_X86_REG_ECX, unit)
        vm.emu_start(address, finish, count=300)
        if vm.reg_read(UC_X86_REG_EIP) != finish:
            raise RuntimeError('Did not return from retail step')

    put(unit+4, 'I', resource)
    put(unit+0x33, 'hh', 34, 110)
    put(0x10102250, 'I', unit)
    put(resource+0x788, '4i', 8, 0, 5, 0)
    digest = 14695981039346656037
    for seed in (0x76321f, 0, 0xffffffff):
        for stance in range(2):
            for slot in range(4):
                for direction in range(8):
                    put(0x10dd4ff0, 'I', seed)
                    put(unit+0xa2, 'B', slot)
                    put(unit+0xe5, 'BB', direction, stance)
                    put(unit+0xec, 'BB', 2, 0)
                    for _ in range(64):
                        call(0x10021810)
                        state, progress = vm.mem_read(unit+0xec, 2)
                        u, v = struct.unpack('<2I', vm.mem_read(0x1010f264, 8))
                        rng, = struct.unpack('<I', vm.mem_read(0x10dd4ff0, 4))
                        for value in (state, progress, u, v, rng):
                            digest = ((digest ^ value) * 1099511628211) & 0xffffffffffffffff
                        if state == 0:
                            break
                        call(0x100200e0)
                    else:
                        raise RuntimeError('Original step exceeded tick limit')
    print(f'Retail accepted-step trace digest 0x{digest:016x}')
    return digest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    trace(parser.parse_args().game)
