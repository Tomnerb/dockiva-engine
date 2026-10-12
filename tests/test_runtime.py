import struct
import unittest
from examples.build_hello import build, MESSAGE
from runtime import ELF, Machine, Unsupported, MASK


class RuntimeTests(unittest.TestCase):
    def machine(self, data=None):
        self.output = bytearray()
        def writer(fd, payload):
            self.output.extend(payload)
            return len(payload)
        return Machine(ELF(data if data is not None else build()), writer)

    def patch(self, offset, word):
        data = bytearray(build())
        struct.pack_into('<I', data, offset, word)
        return data

    def test_hello(self):
        self.assertEqual(self.machine().run(), 0)
        self.assertEqual(self.output, MESSAGE)

    def test_custom_payload_and_status(self):
        self.assertEqual(self.machine(build(b'Custom\n', 42)).run(), 42)
        self.assertEqual(self.output, b'Custom\n')

    def test_truncated_inputs(self):
        data = build()
        for length in (0, 6, 63, 90, len(data)-1):
            with self.subTest(length=length), self.assertRaises(Unsupported):
                ELF(data[:length])

    def test_wrong_architecture(self):
        data = bytearray(build())
        struct.pack_into('<H', data, 18, 62)
        with self.assertRaises(Unsupported):
            ELF(data)

    def test_memory_limit(self):
        data = bytearray(build())
        struct.pack_into('<Q', data, 104, 32 * 1024 * 1024)
        with self.assertRaisesRegex(Unsupported, 'memory'):
            ELF(data)

    def test_non_executable_entry(self):
        with self.assertRaisesRegex(Unsupported, 'permission'):
            ELF(self.patch(68, 4))

    def test_bad_pointer_returns_linux_efault(self):
        machine = self.machine()
        machine.reg[:3], machine.reg[8] = [1, 0xdeadbeef, 10], 64
        machine.syscall()
        self.assertEqual(machine.reg[0], (-14) & MASK)

    def test_bad_fd_returns_linux_ebadf(self):
        machine = self.machine()
        machine.reg[0], machine.reg[8] = 7, 64
        machine.syscall()
        self.assertEqual(machine.reg[0], (-9) & MASK)

    def test_unknown_instruction(self):
        with self.assertRaisesRegex(Unsupported, 'Instruction'):
            self.machine(self.patch(0x100, 0xffffffff)).run()

    def test_unknown_syscall(self):
        with self.assertRaisesRegex(Unsupported, 'syscall 999'):
            self.machine(self.patch(0x10c, 0xd2800008 | (999 << 5))).run()

    def test_loop_budget(self):
        with self.assertRaisesRegex(Unsupported, 'budget'):
            self.machine(self.patch(0x100, 0x14000000)).run(10)

    def test_backward_branch(self):
        with self.assertRaisesRegex(Unsupported, 'budget'):
            self.machine(self.patch(0x104, 0x17ffffff)).run(10)


if __name__ == '__main__':
    unittest.main()
