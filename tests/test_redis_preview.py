"""The prepared Redis tag alone opts into the bundled execution profile."""
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch
import daemon


class RedisPreviewTests(unittest.TestCase):
    def setUp(self):
        self.tmp = TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        profile = self.root / 'examples/redis-preview/profile.json'
        profile.parent.mkdir(parents=True)
        profile.write_text(json.dumps({'digest': 'sha256:pinned'}))
        self.backend = daemon.Backend.__new__(daemon.Backend)
        self.backend.cache = self.root / 'images'
        self.backend.state = self.root / 'containers'

    def create(self, image, digest='sha256:pinned', readonly=False):
        with patch.object(daemon, 'ROOT', self.root), \
             patch.object(self.backend, 'image', return_value={'Id': digest}), \
             patch.object(daemon.registry, 'resolve', return_value=self.root), \
             patch.object(self.backend, 'cli') as cli, \
             patch.object(daemon.containers, 'read', return_value={}), \
             patch.object(daemon.containers, 'save'):
            self.backend.create('test', {'Image': image, 'HostConfig': {'ReadonlyRootfs': readonly}})
            return cli.call_args.args

    def test_pinned_redis_uses_native_backend(self):
        args = self.create('dockiva/redis:preview')
        self.assertEqual(args[args.index('--cpu-backend') + 1], 'hvf')
        for flag in ('--dynamic', '--process-tree', '--scheduled-network'):
            self.assertIn(flag, args)

    def test_other_images_keep_default_backend(self):
        self.assertNotIn('--cpu-backend', self.create('dockiva/shell:demo', readonly=True))

    def test_changed_redis_tag_rejected(self):
        with self.assertRaisesRegex(daemon.APIError, 'does not match'):
            self.create('dockiva/redis:preview', digest='sha256:other')

    def test_readonly_redis_rejected(self):
        with self.assertRaisesRegex(daemon.APIError, 'private writable'):
            self.create('dockiva/redis:preview', readonly=True)
