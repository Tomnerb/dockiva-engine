"""Build a real minimal Linux AArch64 ELF without a cross compiler."""
from pathlib import Path
import struct

MESSAGE = b'Hello from ARM64 Linux, without a Linux VM!\n'


def build(message=MESSAGE, status=0):
    if len(message) > 65535 or not 0 <= status <= 255:
        raise ValueError('Fixture message/status out of range')
    # Entry at file offset 0x100; message follows eight 4-byte instructions.
    words = [0xd2800020, 0x100000e1, 0xd2800002 | (len(message) << 5),
             0xd2800808, 0xd4000001, 0xd2800000 | (status << 5),
             0xd2800ba8, 0xd4000001]
    code = struct.pack('<8I', *words) + message
    size = 0x100 + len(code)
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    header = ident + struct.pack('<HHIQQQIHHHHHH', 2, 183, 1, 0x400100,
                                64, 0, 0, 64, 56, 1, 0, 0, 0)
    ph = struct.pack('<IIQQQQQQ', 1, 5, 0, 0x400000, 0x400000, size, size, 0x1000)
    return (header + ph).ljust(0x100, b'\0') + code


if __name__ == '__main__':
    path = Path(__file__).with_name('hello-linux-arm64.elf')
    path.write_bytes(build())
    print(path)
