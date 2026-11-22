#!/usr/bin/env python3
"""Compare native 1x Tutorial I visibility and fog updates against Gold 2.2 x86 routines.

Runs unmodified n2Game_Dll.dll (GOG build 13718) under Unicorn UC_MODE_32 using the
real circ64.dat table, real desc_common.sue sight descriptors, real Tutorial I P-FLAGS
and P-OBSTACLES layers, and Gold's map-load perimeter pass (0x1006b2ec..0x1006b448).
"""
import argparse
import gzip
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import zlib

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP

EXPECTED_DLL_SHA256 = "aa3c36fad90a8fb39bc9aa0b55714c433a9d16df1f6d9490bd883c4abbb27f41"


def read_sue_entry(sue_path: Path, target_name: str) -> bytes:
    raw = sue_path.read_bytes()
    if raw[:4] != b"FZFS":
        raise ValueError(f"Unexpected SUE header in {sue_path}")
    pos = 8 + struct.unpack_from("<I", raw, 4)[0]

    def read_table() -> bytes:
        nonlocal pos
        packed, count, width = struct.unpack_from("<III", raw, pos)
        pos += 12
        out = zlib.decompress(raw[pos:pos + packed])
        pos += packed
        if len(out) != count * width:
            raise ValueError(f"Corrupt SUE table in {sue_path}")
        return out

    items, names, offsets = read_table(), read_table(), read_table()
    want = target_name.lower()
    for off in range(0, len(items), 20):
        name_off, kind, first, size, chunk = struct.unpack_from("<IIIII", items, off)
        nul = names.index(b"\x00", name_off)
        if names[name_off:nul].decode("latin1").lower() != want:
            continue
        if kind == 2:
            return raw[first:first + size]
        out = bytearray()
        for i in range((size + chunk - 1) // chunk):
            s = struct.unpack_from("<I", offsets, 4 * (first + i))[0]
            e = struct.unpack_from("<I", offsets, 4 * (first + i + 1))[0]
            out.extend(zlib.decompress(raw[s:e]))
        if len(out) != size:
            raise ValueError(f"Size mismatch reading {target_name} from {sue_path}")
        return bytes(out)
    raise KeyError(f"{target_name} not found in {sue_path}")


def verify_visibility(game: Path, compare_native: Path | None = None) -> None:
    dll = (game / "n2Game_Dll.dll").read_bytes()
    if hashlib.sha256(dll).hexdigest() != EXPECTED_DLL_SHA256:
        raise ValueError("Oracle requires GOG Gold 2.2 build 13718 n2Game_Dll.dll")

    pe = pefile.PE(data=dll)
    vm = Uc(UC_ARCH_X86, UC_MODE_32)
    vm.mem_map(0x10000000, (pe.OPTIONAL_HEADER.SizeOfImage + 0xffff) & ~0xffff)
    vm.mem_write(0x10000000, pe.get_memory_mapped_image())

    base_mem = 0x2000000
    stack = base_mem + 0x1e000
    end = base_mem + 0x1f000
    circ_buf = base_mem + 0x40000
    cfg = base_mem + 0x100000
    descs_base = base_mem + 0x110000
    vm.mem_map(base_mem, 0x200000)
    vm.mem_write(end, b"\x90" * 16)

    def put(addr: int, fmt: str, *vals: int) -> None:
        vm.mem_write(addr, struct.pack("<" + fmt, *vals))

    def read(addr: int, size: int) -> int:
        return int.from_bytes(vm.mem_read(addr, size), "little")

    def call(addr: int, *args: int, unit: int = 0) -> int:
        put(stack, "I" * (1 + len(args)), end, *args)
        vm.reg_write(UC_X86_REG_ESP, stack)
        vm.reg_write(UC_X86_REG_ECX, unit)
        vm.emu_start(addr, end, count=14000000)
        if vm.reg_read(UC_X86_REG_EIP) != end:
            raise RuntimeError(f"Gold callback at {addr:#x} did not return")
        return vm.reg_read(UC_X86_REG_EAX)

    vtable = 0x100ed128

    def skip_hooks(m, addr: int, sz: int, data) -> None:
        sp = m.reg_read(UC_X86_REG_ESP)
        arg_bytes = 0
        if addr == 0x1004a5b0:
            put(m.reg_read(UC_X86_REG_ECX) + 0x33, "hh", read(sp + 4, 4), read(sp + 8, 4))
            arg_bytes = 8
        m.reg_write(UC_X86_REG_ESP, sp + 4 + arg_bytes)
        m.reg_write(UC_X86_REG_EIP, read(sp, 4))

    for hook_va in (read(vtable + 0xc8, 4), 0x1004a5b0, 0x10069f80):
        vm.hook_add(UC_HOOK_CODE, skip_hooks, begin=hook_va, end=hook_va)

    # 1. Weather table (0x10099430): 11-byte stride at 0x1103e818 + 0x1d.
    weather_pcts = [(0, 100), (1, 70), (2, 70), (3, 50)]
    for kind, pct in weather_pcts:
        put(0x1103e818 + 0x1d + 11 * kind, "I", pct)
    put(0x1106f6e8, "I", cfg)
    put(0x1106f6b0, "I", 0)
    put(0x1106f05c, "I", 0)

    # Test 1: 48-row CSoldier::GetSightRadius (0x100212c0) matrix.
    probe_desc = base_mem
    probe_unit = base_mem + 0x1000
    vm.mem_write(probe_unit, b"\x00" * 0x200)
    put(probe_unit + 4, "I", probe_desc)
    put(probe_unit + 0x1c, "B", 0)
    put(probe_unit + 0xe6, "B", 0)
    for base in (11, 12, 13):
        put(probe_desc + 0xd5, "ii", base, 0)
        for moving in (False, True):
            put(probe_unit + 0x1a, "H", 2 if moving else 0)
            for diff in (1, 2):
                put(cfg + 8, "I", diff)
                for kind, vis_pct in weather_pcts:
                    put(0x1014238c, "I", kind)
                    got = call(0x100212c0, unit=probe_unit)
                    after_move = (base - 1) if (moving and base > 1) else base
                    after_weather = max(1, (after_move * vis_pct) // 100)
                    expected = min(31, (after_weather * (13 if diff == 1 else 10)) // 10)
                    if got != expected:
                        raise RuntimeError(
                            f"0x100212c0 mismatch base={base} moving={moving} diff={diff} "
                            f"weather={kind}: got {got}, expected {expected}"
                        )
    print("Test 1 (48 Gold CSoldier::GetSightRadius 0x100212c0 weather/moving/diff rows): PASSED")

    # 2. Load real CIRC64.DAT, Tutorial I P-FLAGS/P-OBSTACLES, and run Gold perimeter pass.
    circ = read_sue_entry(game / "game_common.sue", "CIRC64.DAT")
    pos = 0
    cur_buf = circ_buf
    for r in range(1, 64):
        cnt, = struct.unpack_from("<H", circ, pos)
        pos += 2
        blob = circ[pos:pos + cnt * 3]
        pos += cnt * 3
        vm.mem_write(cur_buf, blob)
        put(0x10892858 + r * 6, "HI", cnt, cur_buf)
        cur_buf += cnt * 3

    with gzip.open(game / "maps/single/--- TUTORIAL I ---.SSM", "rb") as f:
        mission = f.read((32 << 20) + 1)
    if len(mission) > (32 << 20) or mission[:8] != b"MS2S\x22\x02\x00\x00":
        raise ValueError("Unexpected Tutorial I mission header")
    layers = {b"P-FLAGS": (0x1034239c, 4), b"P-OBSTACLES": (0x1080639c, 2)}
    spawns = []
    off = 8
    while off < len(mission):
        nm, sz = struct.unpack_from("<12sI", mission, off)
        off += 16
        tag = nm.rstrip()
        if tag == b"I-UNITS":
            cnt, = struct.unpack_from("<I", mission, off)
            p = off + 4
            for _ in range(cnt):
                u, v, _, owner = struct.unpack_from("<HHBB", mission, p + 12)
                spawns.append((u, v, owner))
                p += 18 + 10 * (mission[p + 10] + mission[p + 11])
        elif tag in layers:
            addr, stride = layers.pop(tag)
            for row in range(128):
                src = off + row * 128 * stride
                vm.mem_write(addr + row * 512 * stride, mission[src:src + 128 * stride])
        off += sz

    put(0x10142384, "II", 128, 128)
    vm.reg_write(UC_X86_REG_ESP, stack)
    vm.emu_start(0x1006b2ec, 0x1006b448, count=100000)
    if vm.reg_read(UC_X86_REG_EIP) != 0x1006b448:
        raise RuntimeError("Gold map perimeter pass did not finish")
    post_flags = []
    for row in range(128):
        post_flags.extend(struct.unpack("<128I", vm.mem_read(0x1034239c + row * 512 * 4, 512)))

    put(0x10dd4ff0, "I", 0x76321f)
    put(0x1014238c, "I", 0)  # Clear weather
    put(cfg + 8, "I", 2)     # Normal difficulty

    # Default non-player descriptor
    put(base_mem + 0x31b, "H", 0xf0)
    put(base_mem + 0x4e0, "II", 128, 12)
    put(base_mem + 0x788, "4i", 8, 0, 5, 0)

    desc_map: dict[str, int] = {}

    def get_desc(uid: str) -> int:
        if uid not in desc_map:
            d = descs_base + len(desc_map) * 0x1000
            vm.mem_write(d, b"\x00" * 0x1000)
            put(d + 0x31b, "H", 0xf0)
            put(d + 0x4e0, "II", 128, 12)
            put(d + 0x788, "4i", 8, 0, 5, 0)
            txt = read_sue_entry(game / "desc_common.sue", f"UNITS\\{uid}").decode("latin1")
            s, cs = (12, 0), (9, 0)
            for ln in txt.splitlines():
                toks = ln.strip().split()
                if not toks:
                    continue
                if toks[0].lower() == "sight":
                    s = (int(toks[1]), int(toks[2]) - int(toks[1]))
                elif toks[0].lower() == "crouchsight":
                    cs = (int(toks[1]), int(toks[2]) - int(toks[1]))
            put(d + 0xd5, "ii", s[0], s[1])
            put(d + 0x7b8, "ii", cs[0], cs[1])
            desc_map[uid] = d
        return desc_map[uid]

    placements = []
    for line in (game / "maps.src/map.020/mis.000/mapunits").read_text().splitlines():
        fields = dict(re.findall(r"(\w+)=(\S+)", line))
        if "U" in fields:
            placements.append(
                (fields["ID"], int(fields["U"]), int(fields["V"]), int(fields["Owner"]), int(fields["Grp"]))
            )
    if [(u, v, o) for _, u, v, o, _ in placements] != list(reversed(spawns)):
        raise ValueError("Placement order mismatch against I-UNITS")

    squad = []
    grp2 = []
    for i, (uid, u, v, owner, grp) in enumerate(placements):
        unit = base_mem + 0x2000 + i * 0x400
        x, y, slot = u // 32, v // 32, (u // 16) % 2 + 2 * ((v // 16) % 2)
        d = get_desc(uid) if owner == 0 else base_mem
        put(unit, "IIH", vtable, d, i + 1)
        put(unit + 10, "I", unit + 0x400 if i + 1 < len(placements) else 0)
        put(unit + 0x1c, "B", owner)
        put(unit + 0x33, "hh", x, y)
        put(unit + 0xa2, "B", slot)
        put(unit + 0x9c, "hhBB", x, y, 4, 0)
        call(0x10036370, x, y, slot, 0, unit=unit + 0x9c)
        if owner == 0:
            squad.append((i, unit, uid))
            put(unit + 0xa4, "Bhh", 1, x + 3, y - 39)
            put(unit + 0xb0, "I", 0x7fffff)
            put(unit + 0xbe, "B", 255)
        if grp == 2:
            grp2.append((i, uid, x, y))
    put(0x1010f258, "I", base_mem + 0x2000)

    vm.mem_write(0x1064239c, b"\x00" * (128 * 512))

    def refresh_gold_fog() -> None:
        for row in range(128):
            cur = bytearray(vm.mem_read(0x1064239c + row * 512, 128))
            for k in range(128):
                cur[k] &= 0xf0
            vm.mem_write(0x1064239c + row * 512, bytes(cur))
        for _, u, _ in squad:
            tx, ty = struct.unpack("<hh", vm.mem_read(u + 0x33, 4))
            r = call(0x100212c0, unit=u)
            call(0x1006cab0, tx, ty, r, 0)

    def fog_snapshot() -> tuple[int, int, int]:
        plane = bytearray()
        for row in range(128):
            plane.extend(b & 0x11 for b in vm.mem_read(0x1064239c + row * 512, 128))
        return sum(1 for b in plane if b & 1), sum(1 for b in plane if b & 0x10), zlib.crc32(plane)

    refresh_gold_fog()
    prev_state = [
        (*struct.unpack("<hh", vm.mem_read(u + 0x33, 4)), bool(read(u + 0x1a, 1) & 2))
        for _, u, _ in squad
    ]

    checkpoints = {0: fog_snapshot()}
    print(f"GOLD_VIS_CHECKPOINT tick=0 vis={checkpoints[0][0]} exp={checkpoints[0][1]} crc=0x{checkpoints[0][2]:08x}")

    active = {u for _, u, _ in squad}
    first_spot: dict[int, tuple[int, int, int]] = {}
    elev_hits = 0
    pos_crc = 0
    wanted_ticks = {500, 1000, 1200, 1400, 1600, 1800, 2000, 2200, 2600, 3200}

    for tick in range(1, 3201):
        for _, u, _ in squad:
            if u not in active:
                continue
            put(0x10102250, "I", u)
            backoff = read(u + 0xe0, 1)
            if backoff:
                put(u + 0xe0, "B", backoff - 1)
            if read(u + 0xec, 1) == 2:
                call(0x100200e0, unit=u)
                if read(u + 0xec, 1) == 2:
                    continue
            call(0x10021a50, unit=u)
            if call(0x10036a90, 2, unit=u):
                active.remove(u)
            elif read(0x1010f2bd, 1) & 1:
                call(0x10021b70, unit=u)
        if any(read(u + 0xa9, 1) == 1 and not read(u + 0xe0, 1) for u in active):
            call(0x10058570)

        positions = bytearray()
        dirty = False
        for k, (_, u, _) in enumerate(squad):
            put(0x10102250, "I", u)
            call(0x10021810, unit=u)
            px, py = struct.unpack("<II", vm.mem_read(0x1010f264, 8))
            positions += struct.pack("<HH", 4096 + px - py, (px + py) // 2)
            tx, ty = struct.unpack("<hh", vm.mem_read(u + 0x33, 4))
            mov = bool(read(u + 0x1a, 1) & 2)
            cur = (tx, ty, mov)
            if cur != prev_state[k]:
                prev_state[k] = cur
                dirty = True
            if post_flags[ty * 128 + tx] & 0x2000:
                elev_hits += 1
        pos_crc = zlib.crc32(positions, pos_crc)
        if dirty:
            refresh_gold_fog()

        for idx, uid, gx, gy in grp2:
            if idx not in first_spot:
                if vm.mem_read(0x1064239c + gy * 512 + gx, 1)[0] & 1:
                    first_spot[idx] = (tick, gx, gy)

        if tick in wanted_ticks:
            snap = fog_snapshot()
            checkpoints[tick] = snap
            print(f"GOLD_VIS_CHECKPOINT tick={tick} vis={snap[0]} exp={snap[1]} crc=0x{snap[2]:08x}")

    if active or pos_crc != 0x0489743f:
        raise RuntimeError(f"Convoy trajectory drifted from Gold baseline: active={len(active)} pos_crc=0x{pos_crc:08x}")
    for idx, uid, gx, gy in grp2:
        t_spot = first_spot.get(idx, (-1, gx, gy))[0]
        print(f"GOLD_GROUP2_SPOT placement={idx} id={uid} tile=({gx},{gy}) tick={t_spot}")
    print(f"Gold convoy pos_crc=0x{pos_crc:08x} elev_hits={elev_hits} remaining_active={len(active)}")

    if compare_native is not None:
        exe = compare_native / "visibility_parity_test"
        res = subprocess.run([str(exe), str(game)], check=True, text=True, capture_output=True)
        native_ck: dict[int, tuple[int, int, int]] = {}
        native_spot: dict[int, int] = {}
        for ln in res.stdout.splitlines():
            m = re.match(r"NATIVE_VIS_CHECKPOINT tick=(\d+) vis=(\d+) exp=(\d+) crc=(0x[0-9a-fA-F]+)", ln)
            if m:
                native_ck[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4), 16))
            m2 = re.match(r"NATIVE_GROUP2_SPOT placement=(\d+) .* tick=(\d+)", ln)
            if m2:
                native_spot[int(m2.group(1))] = int(m2.group(2))
        for tick, expected in checkpoints.items():
            got = native_ck.get(tick)
            if got != expected:
                raise RuntimeError(f"Checkpoint tick={tick} diverged: gold={expected} native={got}")
        for idx, _, _, _ in grp2:
            g_t = first_spot[idx][0]
            n_t = native_spot.get(idx)
            if n_t != g_t:
                raise RuntimeError(f"Group-2 placement={idx} spot tick diverged: gold={g_t} native={n_t}")
        print(f"verify_visibility --compare-native PASSED ({len(checkpoints)} checkpoints + {len(grp2)} Group-2 first-spot ticks bit-identical)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("game", type=Path)
    ap.add_argument("--compare-native", type=Path, default=None, help="Build dir containing visibility_parity_test")
    args = ap.parse_args()
    verify_visibility(args.game, args.compare_native)
