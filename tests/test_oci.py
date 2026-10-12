import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import oci
from scripts.build_m6 import tar_bytes, write_layout

class OCITests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.elf = (oci.ROOT / 'examples/m5/server.elf').read_bytes()

    def entry(self, name='app/server', data=None, kind=tarfile.REGTYPE, mode=0o755):
        return (name, self.elf if data is None else data, kind, mode)

    def layout(self, extra=(), **kwargs):
        return write_layout(self.root, [tar_bytes([self.entry(), *extra])], **kwargs)

    def test_gzip_extract_and_command(self):
        self.layout(compressed=True, settings=dict(Entrypoint=['/app/server'], Cmd=['auto'], Env=['MODE=oci']))
        image = oci.Image(self.root, 'demo')
        self.assertEqual(image.command()[0], ['/app/server', 'auto'])
        self.assertEqual(image.command([])[0], ['/app/server'])
        self.assertEqual(image.env, ['MODE=oci'])
        with tempfile.TemporaryDirectory() as target:
            image.extract(target)
            path = Path(target) / 'app/server'
            self.assertEqual(path.read_bytes(), self.elf)
            self.assertEqual(path.stat().st_mode & 0o777, 0o700)

    def test_blob_digest(self):
        manifest = self.layout()
        path = self.root / 'blobs/sha256' / manifest['layers'][0]['digest'][7:]
        raw = bytearray(path.read_bytes()); raw[0] ^= 1; path.write_bytes(raw)
        with self.assertRaisesRegex(oci.ImageError, 'digest mismatch'):
            oci.Image(self.root)

    def test_diff_id(self):
        self.layout(compressed=True, diffs=['sha256:' + '0'*64])
        with self.assertRaisesRegex(oci.ImageError, 'DiffID'):
            oci.Image(self.root)

    def test_unsafe_paths(self):
        for name in ('../escape', '/escape', 'app/../../escape', 'a\\b', 'a:b', 'a/\x01b'):
            with self.subTest(name=name), self.assertRaises(oci.ImageError):
                oci.layer_apply({}, tar_bytes([self.entry(name)]), [0])

    def test_links_and_special_files(self):
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE, tarfile.BLKTYPE):
            with self.subTest(kind=kind), self.assertRaisesRegex(oci.ImageError, 'unsupported'):
                oci.layer_apply({}, tar_bytes([self.entry('unsafe', b'', kind)]), [0])

    def test_duplicate_and_case_collision(self):
        for name in ('app/server', './app/server', 'App/server'):
            with self.subTest(name=name):
                self.layout([self.entry(name)])
                with self.assertRaises(oci.ImageError):
                    oci.Image(self.root)

    def test_whiteout_order(self):
        tree = {}; budget = [0]
        oci.layer_apply(tree, tar_bytes([self.entry('a', b'old'), self.entry('b', b'delete')]), budget)
        oci.layer_apply(tree, tar_bytes([self.entry('a', b'new'), self.entry('.wh.a', b''), self.entry('.wh.b', b'')]), budget)
        self.assertEqual(tree, {'a': ('file', b'new', True)})

    def test_opaque_directory(self):
        tree = {}; budget = [0]
        oci.layer_apply(tree, tar_bytes([self.entry('dir/old', b'old'), self.entry('keep', b'keep')]), budget)
        oci.layer_apply(tree, tar_bytes([self.entry('dir/new', b'new'), self.entry('dir/.wh..wh..opq', b'')]), budget)
        self.assertNotIn('dir/old', tree)
        self.assertEqual(tree['dir/new'][1], b'new')
        self.assertIn('keep', tree)

    def test_file_directory_replacement(self):
        tree = {}; budget = [0]
        oci.layer_apply(tree, tar_bytes([self.entry('a/b', b'b')]), budget)
        oci.layer_apply(tree, tar_bytes([self.entry('a', b'a')]), budget)
        self.assertEqual(tree, {'a': ('file', b'a', True)})
        oci.layer_apply(tree, tar_bytes([self.entry('a', b'', tarfile.DIRTYPE), self.entry('a/c', b'c')]), budget)
        self.assertEqual(tree['a/c'][1], b'c')

    def test_invalid_whiteout(self):
        with self.assertRaisesRegex(oci.ImageError, 'Whiteouts'):
            oci.layer_apply({}, tar_bytes([self.entry('.wh.a', b'bad')]), [0])

    def test_expansion_limit(self):
        self.layout(compressed=True)
        with patch.object(oci, 'MAX_EXPANDED', 512), self.assertRaisesRegex(oci.ImageError, 'Expanded'):
            oci.Image(self.root)

    def test_member_limit(self):
        with patch.object(oci, 'MAX_MEMBERS', 1), self.assertRaisesRegex(oci.ImageError, 'Too many'):
            oci.layer_apply({}, tar_bytes([self.entry('a'), self.entry('b')]), [0])

    def test_large_file_boundary(self):
        data = b'x' * oci.MAX_BLOB
        tree = {}
        oci.layer_apply(tree, tar_bytes([self.entry('large', data)]), [0])
        self.assertEqual(len(tree['large'][1]), 32 * 1024 * 1024)
        header = tarfile.TarInfo('too-large')
        header.size = oci.MAX_BLOB + 1
        with self.assertRaisesRegex(oci.ImageError, 'Layer file exceeds'):
            oci.layer_apply({}, header.tobuf() + b'\0' * 1024, [0])

    def test_layout_symlink(self):
        manifest = self.layout()
        path = self.root / 'blobs/sha256' / manifest['layers'][0]['digest'][7:]
        target = self.root / 'target'; path.rename(target); path.symlink_to(target)
        with self.assertRaisesRegex(oci.ImageError, 'Symlink'):
            oci.Image(self.root)

    def test_config_rejections(self):
        for value in ({'WorkingDir': '/app'}, {'User': '0'}, {'Env': ['BAD']},
                      {'Env': ['A=1', 'A=2']}, {'Entrypoint': ['server']},
                      {'Entrypoint': ['/missing']}, {'Cmd': [3]}):
            with self.subTest(value=value):
                self.layout(settings={'Entrypoint': ['/app/server'], **value})
                with self.assertRaises(oci.ImageError):
                    oci.Image(self.root)

    def test_wrong_platform(self):
        self.layout(architecture='amd64')
        with self.assertRaisesRegex(oci.ImageError, 'platform'):
            oci.Image(self.root)

    def test_reference_selection(self):
        self.layout()
        with self.assertRaisesRegex(oci.ImageError, 'Select exactly'):
            oci.Image(self.root, 'missing')

    def test_duplicate_json_keys(self):
        with self.assertRaisesRegex(oci.ImageError, 'Duplicate'):
            oci.json_object(b'{"a":1,"a":2}')

    def test_nonexecutable_entrypoint(self):
        write_layout(self.root, [tar_bytes([self.entry(mode=0o644)])])
        with self.assertRaisesRegex(oci.ImageError, 'not executable'):
            oci.Image(self.root)

if __name__ == '__main__':
    unittest.main()
