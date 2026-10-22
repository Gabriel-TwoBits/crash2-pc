#!/usr/bin/env python3
"""Replays the native blocks a run traced (src/native.c, CRASH2_NATIVE_TRACE) on Unicorn's MIPS and compares: each block
starts from the registers it started from, its memory holding what the run read there first, and must end with the
same registers and the same memory where it stored.

    .venv/bin/python tools/check_native.py TRACE

Unicorn's MIPS has no load delay slot: a block that reads a register in the instruction after loading it would show
up as a difference. As decomp's tools/equiv.py found, Unicorn 2.1.4 keeps kseg0 at physical addresses (& 0x1FFFFFFF),
faults in kseg2 (so $ra's sentinel is another), and loses a taken branch when a memory hook runs on its delay slot's
store: every page the run touched is mapped and filled first, and no hook watches the stores.
"""
import struct
import sys
from pathlib import Path

from unicorn import UC_ARCH_MIPS, UC_MODE_LITTLE_ENDIAN, UC_MODE_MIPS32, Uc, UcError
from unicorn.mips_const import UC_MIPS_REG_0

TRACE_BEGIN = 0x4E494742
TRACE_FETCH = 1
TRACE_LOAD_WORD = 2
TRACE_LOAD_HALF = 3
TRACE_STORE_WORD = 4
TRACE_STORE_HALF = 5
TRACE_END = 0x444E4542
RECORD = struct.Struct("<3I")
REGISTERS = 32
REGISTER_FILE = struct.Struct(f"<{REGISTERS}I")
RETURN_ADDRESS = 0xFFFFFFF0
# $ra's sentinel on Unicorn, in kseg0, where no block's code is
SENTINEL = 0x80FFF000
KSEG0 = 0x80000000
KSEG2 = 0xC0000000
PHYSICAL_MASK = 0x1FFFFFFF
RA = 31
PC = 21
# Each step's instruction and its delay slot at most, with room to spare
STEP_LIMIT_FACTOR = 2
STEP_LIMIT_SPARE = 16
PAGE_BYTES = 0x1000
PAGE_MASK = ~(PAGE_BYTES - 1) & 0xFFFFFFFF
WORD_BYTES = 4
HALF_BYTES = 2
# $k0 and $k1 belong to the kernel; neither side writes them
KERNEL_REGISTERS = (26, 27)
SHOWN = 10


def blocks(data):
    """Each traced block: (registers on entry, events, steps, registers at the end)"""
    position = 0
    while position + RECORD.size + REGISTER_FILE.size <= len(data):
        kind, address, value = RECORD.unpack_from(data, position)
        position += RECORD.size
        if kind != TRACE_BEGIN:
            sys.exit(f"trace: a block's start expected at byte {position - RECORD.size}")
        else:
            start = REGISTER_FILE.unpack_from(data, position)
            position += REGISTER_FILE.size
        events = []
        while True:
            if position + RECORD.size > len(data):
                return
            else:
                kind, address, value = RECORD.unpack_from(data, position)
                position += RECORD.size
            if kind == TRACE_END and position + REGISTER_FILE.size > len(data):
                # A run that stopped mid-block
                return
            elif kind == TRACE_END:
                end = REGISTER_FILE.unpack_from(data, position)
                position += REGISTER_FILE.size
                yield start, events, address, end
                break
            else:
                events.append((kind, address, value))


def initial_memory(events):
    """{address: byte} the block found in memory: its fetches, and its loads before it stored there"""
    memory = {}
    stored = set()
    for kind, address, value in events:
        if kind in (TRACE_FETCH, TRACE_LOAD_WORD, TRACE_LOAD_HALF):
            size = HALF_BYTES if kind == TRACE_LOAD_HALF else WORD_BYTES
            for index, byte in enumerate(value.to_bytes(WORD_BYTES, "little")[:size]):
                if address + index not in stored:
                    memory.setdefault(address + index, byte)
                else:
                    continue
        elif kind in (TRACE_STORE_WORD, TRACE_STORE_HALF):
            size = HALF_BYTES if kind == TRACE_STORE_HALF else WORD_BYTES
            stored.update(range(address, address + size))
        else:
            sys.exit(f"trace: unknown event {kind}")
    return memory


def physical(address):
    """Where Unicorn keeps a virtual address"""
    if KSEG0 <= address < KSEG2:
        return address & PHYSICAL_MASK
    else:
        return address


def expected_memory(events):
    """{address: byte} the block's stores leave"""
    memory = {}
    for kind, address, value in events:
        if kind in (TRACE_STORE_WORD, TRACE_STORE_HALF):
            size = HALF_BYTES if kind == TRACE_STORE_HALF else WORD_BYTES
            for index, byte in enumerate(value.to_bytes(WORD_BYTES, "little")[:size]):
                memory[address + index] = byte
        else:
            continue
    return memory


def replay(start, events, steps):
    """The block on Unicorn: its registers at the end, and its bytes at the run's stored addresses"""
    memory = initial_memory(events)
    pages = {physical(address) & PAGE_MASK for _, address, _ in events}
    emulator = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
    by_physical = {physical(address): byte for address, byte in memory.items()}
    for page in sorted(pages):
        emulator.mem_map(page, PAGE_BYTES)
        emulator.mem_write(page, bytes(by_physical.get(page + offset, 0) for offset in range(PAGE_BYTES)))
    for register in range(REGISTERS):
        emulator.reg_write(UC_MIPS_REG_0 + register, start[register])
    emulator.reg_write(UC_MIPS_REG_0 + RA, SENTINEL)
    emulator.emu_start(start[PC], SENTINEL, count=steps * STEP_LIMIT_FACTOR + STEP_LIMIT_SPARE)
    end = [emulator.reg_read(UC_MIPS_REG_0 + register) & 0xFFFFFFFF for register in range(REGISTERS)]
    if end[RA] == SENTINEL:
        end[RA] = RETURN_ADDRESS
    else:
        pass
    stored = {address: emulator.mem_read(physical(address), 1)[0] for address in expected_memory(events)}
    return end, stored


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    else:
        data = Path(sys.argv[1]).read_bytes()
    checked = 0
    differing = []
    for start, events, steps, end in blocks(data):
        checked += 1
        try:
            emulated, stored = replay(start, events, steps)
        except UcError as error:
            differing.append(f"block at {start[PC]:#010x}: Unicorn stopped ({error})")
            continue
        registers = [index for index in range(REGISTERS)
                     if index not in KERNEL_REGISTERS and emulated[index] != end[index]]
        memory = stored == expected_memory(events)
        if registers or not memory:
            names = ", ".join(f"${index} {end[index]:#x} (Unicorn {emulated[index]:#x})" for index in registers)
            memory_note = "" if memory else "; the stored memory differs"
            differing.append(f"block at {start[PC]:#010x}, {steps} steps: {names or 'registers agree'}{memory_note}")
        else:
            continue
    print(f"{checked} blocks replayed, {checked - len(differing)} agree")
    for line in differing[:SHOWN]:
        print("  " + line)
    if differing:
        sys.exit(1)
    else:
        return


if __name__ == "__main__":
    main()
