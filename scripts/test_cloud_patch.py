import importlib.util
import os
import pathlib
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('cloud_patch', pathlib.Path(__file__).with_name('cloud_patch.py'))
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class CloudPatchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.previous = pathlib.Path.cwd()
        os.chdir(self.tmp.name)
        subprocess.run(['git', 'init', '-q'], check=True)
        subprocess.run(['git', 'config', 'user.name', 'Test'], check=True)
        subprocess.run(['git', 'config', 'user.email', 'test@example.com'], check=True)
        for f in ('Makefile', 'src/engine.cpp', 'include/minicol/engine.hpp', 'tests/test_engine.cpp'):
            p = pathlib.Path(f)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('initial\n')
        subprocess.run(['git', 'add', '.'], check=True)
        subprocess.run(['git', 'commit', '-qm', 'Fixture'], check=True)
        self.patch = pathlib.Path(self.tmp.name).parent / (pathlib.Path(self.tmp.name).name + '.patch')

    def tearDown(self):
        os.chdir(self.previous)
        self.tmp.cleanup()
        self.patch.unlink(missing_ok=True)

    def test_roundtrip_new_file_and_edit(self):
        pathlib.Path('src/engine.cpp').write_text('changed\n')
        pathlib.Path('tests/new.cpp').write_text('new test\n')
        module.capture(self.patch)
        subprocess.run(['git', 'reset', '--hard', '-q', 'HEAD'], check=True)
        module.apply(self.patch)
        self.assertEqual(pathlib.Path('src/engine.cpp').read_text(), 'changed\n')
        self.assertEqual(pathlib.Path('tests/new.cpp').read_text(), 'new test\n')

    def test_empty_patch(self):
        module.capture(self.patch)
        self.assertEqual(self.patch.read_bytes(), b'')
        module.apply(self.patch)

    def test_control_file_rejected(self):
        pathlib.Path('AGENTS.md').write_text('changed permissions\n')
        with self.assertRaises(ValueError):
            module.capture(self.patch)

    def test_workflow_rejected(self):
        pathlib.Path('.github/workflows').mkdir(parents=True)
        pathlib.Path('.github/workflows/evil.yml').write_text('anything\n')
        with self.assertRaises(ValueError):
            module.capture(self.patch)

    def test_symlink_rejected(self):
        pathlib.Path('src/escape').symlink_to('/etc/passwd')
        with self.assertRaises(ValueError):
            module.capture(self.patch)

    def test_required_file_deletion_rejected(self):
        pathlib.Path('tests/test_engine.cpp').unlink()
        with self.assertRaises(ValueError):
            module.capture(self.patch)

    def test_oversize_patch_rejected(self):
        self.patch.write_bytes(b'x' * (module.MAX_PATCH_BYTES + 1))
        with self.assertRaises(ValueError):
            module.apply(self.patch)

    def test_incoming_control_file_rejected(self):
        pathlib.Path('AGENTS.md').write_text('untrusted\n')
        subprocess.run(['git', 'add', '.'], check=True)
        self.patch.write_bytes(module.git('diff', '--cached', '--binary'))
        subprocess.run(['git', 'reset', '--hard', '-q', 'HEAD'], check=True)
        with self.assertRaises(ValueError):
            module.apply(self.patch)


if __name__ == '__main__':
    unittest.main()
