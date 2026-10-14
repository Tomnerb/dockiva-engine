#!/usr/bin/env python3
"""Bounded ARM64 Linux user-mode interpreter, not a sandbox."""
import argparse
import os
from pathlib import Path
import struct
import sys

MASK = (1 << 64) - 1
LIMIT = 16 * 1024 * 1024


class Unsupported(Exception):
    pass


class ELF:
    def __init__(self, data):
        if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01':
            raise Unsupported('Expected little-endian ELF64 version 1')
        h = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
        kind, machine, version, self.entry, phoff = h[:5]
        if (kind, machine, version) != (2, 183, 1):
            raise Unsupported('Only ARM64 ET_EXEC version 1 is supported')
        if h[7] != 64 or h[8] != 56 or not 0 < h[9] <= 128:
            raise Unsupported('Invalid ELF/program header size or count')
        if phoff < 64 or phoff + h[8] * h[9] > len(data):
            raise Unsupported('Program headers outside file')
        self.segments = []
        total = 0
        for i in range(h[9]):
            typ, flags, off, addr, _, size, mem, align = struct.unpack_from(
                '<IIQQQQQQ', data, phoff + i * 56)
            if typ in (2, 3):
                raise Unsupported('Dynamic linking is not implemented')
            if typ != 1:
                continue
            if size > mem or off + size > len(data) or addr + mem > MASK:
                raise Unsupported('Invalid load segment bounds')
            if align > 1 and (align & (align - 1) or addr % align != off % align):
                raise Unsupported('Invalid segment alignment')
            total += mem
            if total > LIMIT:
                raise Unsupported('Guest memory exceeds 16 MiB limit')
            if any(addr < a + len(b) and a < addr + mem for a, b, _ in self.segments):
                raise Unsupported('Overlapping load segments are unsupported')
            self.segments.append((addr, data[off:off + size] + bytes(mem - size), flags))
        self.read(self.entry, 4, executable=True)

    def read(self, addr, count, executable=False):
        if count < 0 or count > LIMIT:
            raise Unsupported('Invalid guest read size')
        for start, data, flags in self.segments:
            if start <= addr and addr + count <= start + len(data):
                if not flags & (1 if executable else 4):
                    raise Unsupported('Guest segment permission denied')
                return data[addr - start:addr - start + count]
        raise Unsupported(f'Unmapped guest address 0x{addr:x}')


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class Machine:
    def __init__(self, elf, writer=os.write, trace=False):
        self.elf, self.writer, self.trace = elf, writer, trace
        self.reg = [0] * 32
        self.pc = elf.entry

    def syscall(self):
        number = self.reg[8]
        if self.trace:
            print(f'syscall {number}: x0..2={self.reg[:3]}', file=sys.stderr)
        if number in (93, 94):
            return self.reg[0] & 255
        if number != 64:
            raise Unsupported(f'Linux syscall {number} is not implemented')
        fd, addr, count = self.reg[:3]
        if fd not in (1, 2):
            self.reg[0] = (-9) & MASK  # Linux EBADF
            return None
        try:
            payload = self.elf.read(addr, count) if count else b''
        except Unsupported:
            self.reg[0] = (-14) & MASK  # Linux EFAULT
            return None
        try:
            self.reg[0] = self.writer(fd, payload)
        except OSError:
            self.reg[0] = (-5) & MASK  # Prototype collapses host errors to Linux EIO
        return None

    def run(self, max_steps=100000):
        for _ in range(max_steps):
            if self.pc % 4:
                raise Unsupported('Unaligned instruction address')
            ins = int.from_bytes(self.elf.read(self.pc, 4, executable=True), 'little')
            oldpc = self.pc
            self.pc = (self.pc + 4) & MASK
            rd = ins & 31
            if ins & 0x7f800000 == 0x52800000:  # MOVZ Wd/Xd
                width = 64 if ins >> 31 else 32
                shift = ((ins >> 21) & 3) * 16
                if shift >= width:
                    raise Unsupported('Reserved MOVZ encoding')
                self.reg[rd] = ((ins >> 5) & 65535) << shift
            elif ins & 0x9f000000 == 0x10000000:  # ADR
                imm = (((ins >> 5) & 0x7ffff) << 2) | ((ins >> 29) & 3)
                self.reg[rd] = (oldpc + signed(imm, 21)) & MASK
            elif ins & 0xfc000000 == 0x14000000:  # B immediate
                self.pc = (oldpc + (signed(ins & 0x3ffffff, 26) << 2)) & MASK
            elif ins == 0xd503201f:  # NOP
                pass
            elif ins == 0xd4000001:  # SVC #0
                status = self.syscall()
                if status is not None:
                    return status
            else:
                raise Unsupported(f'Instruction 0x{ins:08x} at 0x{oldpc:x} unsupported')
            self.reg[31] = 0  # XZR for the supported instructions
        raise Unsupported(f'Instruction budget exhausted ({max_steps})')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['inspect', 'run'])
    parser.add_argument('elf', type=Path)
    parser.add_argument('--trace', action='store_true')
    parser.add_argument('--max-steps', type=int, default=100000)
    args = parser.parse_args()
    try:
        with args.elf.open('rb') as stream:
            data = stream.read(LIMIT + 1)
        if len(data) > LIMIT:
            raise Unsupported('Input file exceeds 16 MiB limit')
        elf = ELF(data)
        if args.command == 'inspect':
            print(f'ARM64 ELF64 ET_EXEC; entry=0x{elf.entry:x}; segments={len(elf.segments)}')
            return 0
        return Machine(elf, trace=args.trace).run(args.max_steps)
    except (Unsupported, OSError, struct.error) as exc:
        print(f'dockiva: {exc}', file=sys.stderr)
        return 125


if __name__ == '__main__':
    sys.exit(main())
