#!/usr/bin/env python3
"""Package completed T3-S3, T-Deck Plus and T-Echo release builds."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
FLASH_FILES = {0: 'bootloader.bin', 0x8000: 'partitions.bin', 0x10000: 'receiver.bin'}
# Pin the entire on-device partition table used by each install procedure. The
# T3-S3 layout reserves the packet journal at 0x100000; T-Deck uses a 1 MiB app.
T3_TABLE = '095d947e658e8d9ef99b448060ffd5b02d2af61cf75788dec5e458aa1970717c'
TDECK_TABLE = '7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820'


def esp_bundle(build, destination, *, tdeck):
    label = 'T-Deck Plus' if tdeck else 'T3-S3'
    flash_size = '16MB' if tdeck else '4MB'
    cache = {line.upper() for line in (build / 'CMakeCache.txt').read_text().splitlines()}
    board_values = ('ON', '1', 'YES', 'TRUE', 'Y') if tdeck else ('OFF', '0', 'NO', 'FALSE', 'N')
    if not any(f'ADS_B_TDECK_PLUS:BOOL={value}' in cache for value in board_values):
        raise ValueError(f'{build} is not a {label} release build (check ADS_B_TDECK_PLUS)')
    if tdeck and 'ADS_B_TDECK_REPLAY:FILEPATH=' not in cache:
        raise ValueError('T-Deck release requires a live build without an embedded replay fixture')
    config = json.loads((build / 'flasher_args.json').read_text())
    expected = {'flash_size': flash_size, 'flash_mode': 'dio', 'flash_freq': '80m'}
    if config['extra_esptool_args']['chip'] != 'esp32s3' or config['flash_settings'] != expected:
        raise ValueError(f'{label} installer expects an ESP32-S3 with {flash_size} DIO flash at 80 MHz')
    layout = {int(k, 0): build / path for k, path in config['flash_files'].items()}
    if layout.keys() != FLASH_FILES.keys():
        raise ValueError(f'{label} flash offsets changed; check the installation procedure')
    files = {name: layout[offset] for offset, name in FLASH_FILES.items()}
    # ESP image header: magic 0xe9, DIO (2), size + 80 MHz, ESP32-S3 chip ID (9).
    size_frequency = 0x4f if tdeck else 0x2f
    for name in ('bootloader.bin', 'receiver.bin'):
        with files[name].open('rb') as image:
            header = image.read(24)
        if (len(header) != 24 or header[0] != 0xe9 or header[2:4] != bytes((2, size_frequency))
                or int.from_bytes(header[12:14], 'little') != 9):
            raise ValueError(f'{name} is not an ESP32-S3 {flash_size} DIO/80 MHz image for {label}')
    table_hash = hashlib.sha256(files['partitions.bin'].read_bytes()).hexdigest()
    if table_hash != (TDECK_TABLE if tdeck else T3_TABLE):
        raise ValueError(f'{label} partition table changed; check the installation procedure')
    files['NOTICES.txt'] = ROOT / 'firmware/NOTICES.txt'
    files['LICENSE.txt'] = ROOT / 'LICENSE'
    if tdeck:
        files['README.txt'] = ROOT / 'install/T-Deck-Plus.txt'
        files['FONT-LICENSE.txt'] = ROOT / 'main/tdeck-font-OFL.txt'
    else:
        files['README.txt'] = ROOT / 'install/T3-S3.txt'
        files['install.py'] = ROOT / 'install/t3s3.py'
    with ZipFile(destination, 'w') as archive:
        for name, path in sorted(files.items()):
            entry = ZipInfo(name)
            entry.compress_type = ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, path.read_bytes())


def package(t3, tdeck, techo, out):
    out.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=out) as work:
        stage = Path(work)
        t3_zip = stage / 't3s3-adsb.zip'
        deck_zip = stage / 'tdeck-plus-adsb.zip'
        uf2 = stage / 'techo-adsb.uf2'
        esp_bundle(t3, t3_zip, tdeck=False)
        esp_bundle(tdeck, deck_zip, tdeck=True)
        options = json.loads((techo / 'obj/build.options.json').read_text())
        if options['fqbn'] != 'adafruit:nrf52:pca10056':
            raise ValueError('Expected the T-Echo PCA10056 build')
        # Reuse the application-only converter, which also checks HEX addresses and CRCs.
        subprocess.run([sys.executable, ROOT / 'techo/uf2.py', techo / 'obj/techo.ino.hex', uf2], check=True)
        if uf2.read_bytes() != (techo / uf2.name).read_bytes():
            raise ValueError('T-Echo UF2 does not match the selected build HEX')
        downloads = (t3_zip, deck_zip, uf2)
        manifest = stage / 'SHA256SUMS'
        manifest.write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in downloads))
        for path in (*downloads, manifest):
            path.replace(out / path.name)


if __name__ == '__main__':
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--t3-build', type=Path, default=ROOT / 'build')
    cli.add_argument('--tdeck-build', type=Path, default=ROOT / 'build-tdeck-plus')
    cli.add_argument('--techo-build', type=Path, default=ROOT / 'build-techo')
    cli.add_argument('--output', type=Path, default=ROOT / 'firmware')
    args = cli.parse_args()
    try:
        package(args.t3_build, args.tdeck_build, args.techo_build, args.output)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        cli.exit(1, f'Could not package firmware: {exc}\n')
