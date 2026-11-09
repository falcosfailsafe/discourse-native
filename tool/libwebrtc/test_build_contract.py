"""Local contracts for the source staging and release selection (no network)."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CMAKE = ROOT / 'packages/discourse_voice/third_party/flutter_webrtc/third_party/CMakeLists.txt'


class ReleaseSelectionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.vendor = self.root / 'vendor'
        self.vendor.mkdir()
        shutil.copy2(CMAKE, self.vendor / 'CMakeLists.txt')
        (self.root / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.21)\n'
            'project(contract NONE)\nset(CMAKE_SIZEOF_VOID_P 8)\n'
            'set(CMAKE_SYSTEM_NAME Windows)\nadd_subdirectory(vendor)\n')

    def release(self, tag, value):
        directory = self.root / 'releases' / tag
        directory.mkdir(parents=True)
        archive = directory / 'libwebrtc-win-x64-release.zip'
        with zipfile.ZipFile(archive, 'w') as file:
            file.writestr('libwebrtc/include/rtc_peerconnection_factory.h', value)
        return hashlib.sha256(archive.read_bytes()).hexdigest()

    def manifest(self, tag, checksum):
        url = (self.root / 'releases').as_uri()
        (self.vendor / 'libwebrtc_version.ini').write_text(
            f'[libwebrtc]\nbinary_version = stock\ndownload_url = {url}\n'
            f'windows_binary_version = {tag}\nwindows_download_url = {url}\n'
            f'windows_sha256 = {checksum}\n')

    def configure(self, success=True):
        result = subprocess.run(['cmake', '-S', str(self.root), '-B', str(self.root / 'build')],
                                text=True, capture_output=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def header(self):
        return (self.vendor / 'libwebrtc/include/rtc_peerconnection_factory.h').read_text()

    def test_version_change_reextracts_and_hash_repairs_cache(self):
        checksum = self.release('custom.1', 'first')
        self.manifest('custom.1', checksum)
        self.configure()
        self.assertEqual(self.header(), 'first')
        cache = self.vendor / 'downloads/custom.1/libwebrtc-win-x64-release.zip'
        cache.write_bytes(b'interrupted or tampered download')
        self.configure()
        self.assertEqual(hashlib.sha256(cache.read_bytes()).hexdigest(), checksum)
        checksum = self.release('custom.2', 'second')
        self.manifest('custom.2', checksum)
        self.configure()
        self.assertEqual(self.header(), 'second')

    def test_custom_release_requires_checksum(self):
        self.manifest('missing', '')
        result = self.configure(False)
        self.assertIn('windows_sha256', result.stderr)

    def test_bad_checksum_does_not_promote_archive(self):
        self.release('custom.1', 'first')
        self.manifest('custom.1', '0' * 64)
        self.configure(False)
        self.assertFalse((self.vendor / 'libwebrtc').exists())
        self.assertFalse((self.vendor / 'downloads/custom.1/libwebrtc-win-x64-release.zip').exists())

    def test_linux_keeps_stock_release(self):
        text = (self.root / 'CMakeLists.txt').read_text().replace('Windows', 'Linux')
        (self.root / 'CMakeLists.txt').write_text(text)
        directory = self.root / 'releases/stock'
        directory.mkdir(parents=True)
        with zipfile.ZipFile(directory / 'libwebrtc-linux-x64-release.zip', 'w') as file:
            file.writestr('libwebrtc/include/rtc_peerconnection_factory.h', 'linux-stock')
        self.manifest('unpublished-windows', 'a' * 64)
        self.configure()
        self.assertEqual(self.header(), 'linux-stock')


class SourceStagingTest(unittest.TestCase):
    def test_manifest_uses_immutable_source_revisions(self):
        source = json.loads((HERE / 'source.json').read_text())
        for key in ('wrapper_revision', 'webrtc_revision', 'depot_tools_revision'):
            self.assertRegex(source[key], r'^[0-9a-f]{40}$')
        self.assertEqual(source['asset'], 'libwebrtc-win-x64-release.zip')

    def test_stage_copies_overlay_and_core_patches_and_rejects_wrong_lineage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            recipe = root / 'recipe'
            checkout = root / 'checkout'
            recipe.mkdir()
            checkout.mkdir()
            subprocess.run(['git', 'init', '-q', str(checkout)], check=True)
            for name in ('src', 'include'):
                (checkout / name).mkdir()
            for name in ('BUILD.gn', 'include/rtc_peerconnection_factory.h'):
                (checkout / name).write_text('upstream')
            subprocess.run(['git', '-C', str(checkout), 'add', '.'], check=True)
            subprocess.run(['git', '-C', str(checkout), '-c', 'user.name=Contract',
                            '-c', 'user.email=contract@example.invalid',
                            'commit', '-qm', 'baseline'], check=True)
            revision = subprocess.check_output(
                ['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
            (recipe / 'source.json').write_text(json.dumps({'wrapper_revision': revision}))
            (recipe / 'build-windows.ps1').write_text('checked build recipe')
            (recipe / 'windows-build.yml').write_text('checked workflow')
            (recipe / 'overlay/src').mkdir(parents=True)
            (recipe / 'overlay/src/encoder.cc').write_text('overlay')
            (recipe / 'core-patches').mkdir()
            (recipe / 'core-patches/001.patch').write_text('core patch')
            spec = importlib.util.spec_from_file_location('libwebrtc_stage', HERE / 'stage.py')
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            module.HERE = recipe
            module.stage(checkout)
            module.stage(checkout)  # repeatable on the same reviewed checkout
            self.assertEqual((checkout / 'src/encoder.cc').read_text(), 'overlay')
            self.assertEqual((checkout / 'discourse-build/core-patches/001.patch').read_text(), 'core patch')
            self.assertEqual((checkout / '.github/workflows/discourse-windows.yml').read_text(), 'checked workflow')
            # An unrelated source pin must fail before copying anything.
            unrelated = subprocess.check_output(
                ['git', '-C', str(checkout), 'hash-object', '-w', '--stdin'],
                input='unrelated', text=True).strip()
            (recipe / 'source.json').write_text(json.dumps({'wrapper_revision': unrelated}))
            with self.assertRaises(SystemExit):
                module.stage(checkout)


if __name__ == '__main__':
    unittest.main()
