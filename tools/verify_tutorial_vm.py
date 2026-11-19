"""Observe Tutorial I's opening callbacks in the unmodified Gold 13718 x86 interpreter.

Optional local tooling: python -m pip install pefile unicorn
Pass the private installation directory. No Windows runtime or desktop is used.
"""
import argparse
import gzip
import hashlib
from pathlib import Path
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EIP

BASE = 0x10000000
STACK = 0x02000000
CODE = 0x03000000
PHRASES = CODE + 0x10000
RETURN = CODE + 0x1F000


def scripts(path):
    with gzip.open(path, 'rb') as stream:
        data = stream.read((32 << 20) + 1)
    if len(data) > 32 << 20 or data[:8] != b'MS2S\x22\x02\x00\x00':
        raise ValueError('Unexpected mission')
    i = 8
    while i + 16 <= len(data):
        name = data[i:i+12]
        size, = struct.unpack_from('<I', data, i+12)
        i += 16
        if size > len(data) - i:
            raise ValueError('Truncated mission')
        if name == b'I-SCRIPTS   ':
            section = memoryview(data)[i:i+size]
            count, = struct.unpack_from('<I', section)
            j = 4
            records = []
            for _ in range(count):
                size, = struct.unpack_from('<I', section, j)
                j += 4
                if size < 8 or size > len(section)-j or section[j:j+4] != b'CAOF':
                    raise ValueError('Truncated trigger')
                records.append(bytes(section[j+4:j+size]))
                j += size
            return records
        i += size
    raise ValueError('Missing scripts')


def main(root):
    dll = (root/'n2Game_Dll.dll').read_bytes()
    if hashlib.sha256(dll).hexdigest() != 'aa3c36fad90a8fb39bc9aa0b55714c433a9d16df1f6d9490bd883c4abbb27f41':
        raise ValueError('Oracle addresses require GOG build 13718')
    pe = pefile.PE(data=dll)
    vm = Uc(UC_ARCH_X86, UC_MODE_32)
    image = pe.get_memory_mapped_image()
    vm.mem_map(BASE, (pe.OPTIONAL_HEADER.SizeOfImage + 4095) & ~4095)
    vm.mem_write(BASE, image)
    vm.mem_map(STACK, 0x10000)
    vm.mem_map(CODE, 0x20000)

    def write(addr, *values):
        vm.mem_write(addr, struct.pack('<' + 'I'*len(values), *values))

    def word(addr):
        return int.from_bytes(vm.mem_read(addr, 4), 'little')

    events = []
    tick = 0

    def callback(machine, addr, size, unused):
        # Intercept only presentation calls. The evaluator, conditions, stack,
        # variables, timer writes and trigger retirement execute original bytes.
        sp = machine.reg_read(UC_X86_REG_ESP)
        if addr in (BASE + 0x6de20, BASE + 0x6dae0):
            phrase = (machine.reg_read(UC_X86_REG_ECX) - PHRASES) // 17
            events.append((tick, phrase))
            machine.reg_write(UC_X86_REG_ESP, sp + 12)
        else:
            machine.reg_write(UC_X86_REG_ESP, sp + 4)
        machine.reg_write(UC_X86_REG_EIP, word(sp))

    for address in [BASE + 0x6de20, BASE + 0x6dae0, BASE + 0x9d3d0]:
        vm.hook_add(UC_HOOK_CODE, callback, begin=address, end=address)
    write(0x10892f1c, PHRASES)
    records = scripts(root/'maps/single/--- TUTORIAL I ---.SSM')
    active = {i: records[i] for i in (0, 1, 3, 4, 5, 6, 7, 8, 10, 11)}
    for tick in [0, *range(3, 840, 64)]:
        write(0x1106f67c, tick)
        # Controlled player 0 occupancy, location 2. One entrant is insufficient.
        units = 0 if tick < 579 else 1 if tick < 643 else 2
        vm.mem_write(0x110395a8 + 2*2, struct.pack('<H', units))
        for i, record in list(active.items()):
            vm.mem_write(CODE, record)
            sp = STACK + 0xf000
            write(sp, RETURN, CODE)
            vm.reg_write(UC_X86_REG_ESP, sp)
            vm.emu_start(BASE + 0x91b40, RETURN, count=20000)
            if vm.reg_read(UC_X86_REG_EIP) != RETURN:
                raise RuntimeError('Interpreter exceeded instruction budget')
            if vm.reg_read(UC_X86_REG_EAX) == 0:
                del active[i]
    expected = [(0, 19), (67, 0), (131, 28), (195, 1), (259, 2), (387, 20), (451, 3), (515, 29), (643, 4), (835, 5)]
    if events != expected or active:
        raise RuntimeError(f'Observed {events}, remaining triggers {list(active)}')
    print('Original VM (mission tick, phrase index):', events)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    main(parser.parse_args().game)
