"""Regression tests for the published ESP images and wrong-board rejection."""
import io
import json
from contextlib import redirect_stdout
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from zipfile import ZipFile

import package
import t3s3


class PackageTest(unittest.TestCase):
    def setUp(self):
        tmp = TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.t3 = self.root / 't3'
        self.deck = self.root / 'deck'
        for build, stem, size, board in [(self.t3, 't3s3', '4MB', 'OFF'), (self.deck, 'tdeck-plus', '16MB', 'ON')]:
            build.mkdir()
            with ZipFile(package.ROOT / 'firmware' / f'{stem}-adsb.zip') as archive:
                for name in package.FLASH_FILES.values():
                    (build / name).write_bytes(archive.read(name))
            config = {'flash_files': {hex(k): v for k, v in package.FLASH_FILES.items()},
                      'extra_esptool_args': {'chip': 'esp32s3'},
                      'flash_settings': {'flash_size': size, 'flash_mode': 'dio', 'flash_freq': '80m'}}
            (build / 'flasher_args.json').write_text(json.dumps(config))
            (build / 'CMakeCache.txt').write_text(f'ADS_B_TDECK_PLUS:BOOL={board}\nADS_B_TDECK_REPLAY:FILEPATH=\n')

    def test_shipped_boards_and_wrong_builds(self):
        out = self.root / 'test.zip'
        for build, tdeck in [(self.t3, False), (self.deck, True)]:
            package.esp_bundle(build, out, tdeck=tdeck)
            with ZipFile(out) as archive:
                self.assertEqual(archive.read('receiver.bin'), (build / 'receiver.bin').read_bytes())
                self.assertEqual('install.py' in archive.namelist(), not tdeck)
            out.unlink()
            with self.assertRaisesRegex(ValueError, 'release build'):
                package.esp_bundle(build, out, tdeck=not tdeck)
            self.assertFalse(out.exists())

    def test_foreign_images_despite_matching_metadata(self):
        for name in ('partitions.bin', 'receiver.bin', 'bootloader.bin'):
            with self.subTest(name=name):
                target = self.t3 / name
                original = target.read_bytes()
                target.write_bytes((self.deck / name).read_bytes())
                with self.assertRaises(ValueError):
                    package.esp_bundle(self.t3, self.root / 'test.zip', tdeck=False)
                target.write_bytes(original)

    def test_t3_installer_stops_on_larger_or_unknown_flash(self):
        with patch.object(t3s3, 'HERE', self.t3), patch.object(t3s3.subprocess, 'run') as run:
            for report in ('Detected flash size: 16MB', 'Detected flash size: Unknown', ''):
                run.reset_mock()
                run.return_value.stdout = report
                with redirect_stdout(io.StringIO()), self.assertRaisesRegex(SystemExit, '4 MB flash'):
                    t3s3.run(SimpleNamespace(port='test'))
                run.assert_called_once()
                self.assertEqual(run.call_args.args[0][-1], 'flash_id')


if __name__ == '__main__':
    unittest.main()
