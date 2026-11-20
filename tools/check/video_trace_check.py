"""Video capture integrity checks using local verified ROMs, no golden images."""
import pathlib
import struct
import subprocess
import sys

program, roms, output = sys.argv[1:]
out = pathlib.Path(output)
out.mkdir(parents=True, exist_ok=True)
magic = b'APBVID1\0'
band = b'B' + struct.pack('<IHHHHi', 10, 0, 383, 0, 0, 0) + bytes(3 * 8192)
palette = b'P' + struct.pack('<I', 10) + bytes(512)
end = b'E' + struct.pack('<II', 1, 1)
cases = {
    'valid': (magic + band + palette + end, 0),
    'truncated': (magic + band + palette, 1),
    'short_page': (magic + band[:100], 1),
    'missing_lines': (magic + b'B' + struct.pack('<IHHHHi', 10, 1, 383, 0, 0, 0), 1),
    'duplicate_palette': (magic + band + palette + palette + end, 1),
    'bad_count': (magic + band + palette + b'E' + struct.pack('<II', 2, 1), 1),
}
for name, (content, expected) in cases.items():
    capture = out / f'{name}.capture'
    capture.write_bytes(content)
    result = subprocess.run([program, roms, str(capture), str(out / name)], capture_output=True, text=True)
    if result.returncode != expected:
        raise RuntimeError(f'{name}: exit {result.returncode}, expected {expected}: {result.stderr}')
png = (out / 'valid-10.png').read_bytes()
assert png[:8] == b'\x89PNG\r\n\x1a\n' and struct.unpack('>II', png[16:24]) == (384, 512)
print('Complete video capture accepted; missing/duplicate/truncated bands rejected')
