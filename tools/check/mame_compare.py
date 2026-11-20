#!/usr/bin/env python3
"""Compare the native player with standalone MAME 0.264, not shared models.

Requires numpy, scipy and Pillow. All game-derived output stays under --out.
Exit 0 means exact images and correlated audio; 1 means a reported difference;
2 means setup/capture failure. Offsets are explicit, never fitted per picture.
"""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import wave
import zipfile

import numpy as np
from PIL import Image
from scipy import signal


def run(command, log, env=None):
    with log.open('w', encoding='utf-8') as output:
        subprocess.run([str(x) for x in command], stdout=output, stderr=subprocess.STDOUT,
                       env=env, check=True, timeout=300)


def lua_string(value):
    # Lua accepts the same escapes used here for filenames, including Windows.
    return json.dumps(str(value), ensure_ascii=False)


def compare_audio(native, reference, start, end):
    def read(path):
        with wave.open(str(path), 'rb') as w:
            if w.getsampwidth() != 2 or w.getnchannels() != 2:
                raise ValueError(f'{path}: expected 16-bit stereo WAV')
            return np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, 2).astype(float), w.getframerate()
    a, rate = read(native)
    b, other_rate = read(reference)
    if other_rate != rate:
        gcd = math.gcd(rate, other_rate)
        b = signal.resample_poly(b, rate // gcd, other_rate // gcd, axis=0)
    lo, hi = int(start * rate), int(end * rate)
    margin = rate // 4
    if lo < margin or hi + margin >= min(len(a), len(b)) or hi <= lo:
        raise ValueError('Audio window does not fit captured WAVs')
    # One shared lag for stereo, bounded to +/-250 ms. Correlate the coin
    # window, not silent startup. Report lag rather than hiding alignment.
    x, search = a[lo:hi].mean(axis=1), b[lo-margin:hi+margin].mean(axis=1)
    if np.std(x) < 1 or np.std(search) < 1:
        raise ValueError('Audio window is silent; no valid sound comparison')
    correlations = signal.correlate(search, x, mode='valid', method='fft')
    shift = int(np.argmax(correlations)) - margin
    results = []
    for ch in range(2):
        x, y = a[lo:hi, ch], b[lo+shift:hi+shift, ch]
        xrms, yrms = float(np.sqrt(np.mean(x*x))), float(np.sqrt(np.mean(y*y)))
        if min(xrms, yrms) < 1:
            raise ValueError(f'Audio channel {ch} is silent')
        frequencies, px = signal.welch(x, rate, nperseg=8192)
        _, py = signal.welch(y, rate, nperseg=8192)
        bands = []
        for low, high in ((50, 100), (100, 250), (250, 500), (500, 1000), (1000, 2000), (2000, 5000)):
            mask = (frequencies >= low) & (frequencies < high)
            nx, ny = float(px[mask].sum()), float(py[mask].sum())
            if nx > 0 and ny > 0:
                bands.append({'hz': [low, high], 'level_difference_db': float(10 * np.log10(nx / ny))})
        results.append({'correlation': float(np.corrcoef(x, y)[0, 1]),
                        'native_rms': xrms, 'mame_rms': yrms, 'bands': bands,
                        'level_difference_db': float(20 * np.log10(xrms / yrms))})
    passed = all(c['correlation'] >= 0.98 and abs(c['level_difference_db']) <= 1 for c in results)
    return {'window_seconds': [start, end], 'mame_lag_samples': shift,
            'sample_rate': rate, 'channels': results, 'passed': passed}


def compare_traces(out):
    result = {}
    commands = {}
    for engine in ('native', 'mame'):
        commands[engine] = []
        coins, random_reads, first_ym = [], [], []
        last_coin, ym_count = 0x40, 0
        with (out / f'{engine}-io.trace').open() as trace:
            for line in trace:
                kind, address, value, time = line.split()
                address, value, time = int(address, 16) & 0x7f, int(value, 16), int(time)
                if kind == 'r' and address == 0x60:
                    commands[engine].append({'value': value, 'main_cycle': time})
                if kind == 'r' and address == 0x40 and (value & 0x40) != last_coin:
                    last_coin = value & 0x40
                    if len(coins) < 8:
                        coins.append({'active': not bool(last_coin), 'main_cycle': time})
                if kind == 'r' and address in (0x0a, 0x3a) and len(random_reads) < 8:
                    random_reads.append({'chip': address // 0x30, 'value': value, 'main_cycle': time})
                if kind == 'w' and address in (0x50, 0x51):
                    ym_count += 1
                    if len(first_ym) < 8:
                        first_ym.append({'data': bool(address & 1), 'value': value, 'main_cycle': time})
        if not commands[engine] or not (out / f'{engine}-input.trace').stat().st_size:
            raise ValueError(f'{engine}: bus trace missing or empty')
        result[engine] = {'sound_command_reads': len(commands[engine]), 'first_commands': commands[engine][:8],
                          'coin_edges': coins, 'first_random_reads': random_reads,
                          'ym_writes': ym_count, 'first_ym_writes': first_ym}
    result['first_command_value_difference'] = None
    for i, (a, b) in enumerate(zip(commands['native'], commands['mame'])):
        if a['value'] != b['value']:
            result['first_command_value_difference'] = {'index': i, 'native': a, 'mame': b}
            break
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mame', type=Path, required=True)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--rom-dir', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--frames', default='600,1500,2100,3000,4500', help='MAME frame numbers')
    p.add_argument('--boot-offset', type=int, default=4, help='Native boot finishes this many frames earlier')
    p.add_argument('--audio-window', default='14.5:20', help='Native WAV seconds spanning two coin sounds')
    args = p.parse_args()
    mame, native, roms, out = [x.resolve() for x in (args.mame, args.native, args.rom_dir, args.out)]
    frames = sorted(set(int(n) for n in args.frames.split(',')))
    if not frames or min(frames) < max(1, args.boot_offset) or max(frames) < 1300:
        raise ValueError('Frames must include at least 1300 frames to capture the audio window')
    if out.exists():
        raise ValueError('Choose a new --out directory; old captures must not be reused')
    out.mkdir(parents=True)
    version = subprocess.check_output([str(mame), '-version'], text=True).strip()
    if not version.startswith('0.264 '):
        raise ValueError(f'Expected MAME 0.264 (Lua API/baseline pinned), got {version}')
    (out / 'roms').mkdir()
    with zipfile.ZipFile(out / 'roms' / 'apb.zip', 'w', zipfile.ZIP_STORED) as archive:
        files = sorted(f for f in roms.iterdir() if f.is_file())
        if not files:
            raise ValueError('No extracted ROMs in --rom-dir')
        for file in files:
            archive.write(file, file.name)
    for name in ('cfg', 'nvram', 'snapshot', 'state', 'diff'):
        (out / name).mkdir()
    wanted = ','.join(f'[{n}]=true' for n in frames)
    # Apply controls at vblank for the following frame. Native scripts do
    # likewise. Keep the four-frame boot lead explicit in input timing too.
    offset = args.boot_offset
    lua = f'''local m=manager.machine
local s=m.screens[':screen']
local wanted={{{wanted}}}
local coin=m.ioport.ports[':1840'].fields['Coin 1']
local siren=m.ioport.ports[':1800'].fields['P1 Button 3']
local pedal=m.ioport.ports[':ADC1'].fields['P1 Pedal 1']
assert(coin and siren and pedal)
-- Keep taps alive until exit; no debugger or modified MAME required.
local trace=assert(io.open({lua_string(out / 'mame-io.trace')},'w'))
local inputs=assert(io.open({lua_string(out / 'mame-input.trace')},'w'))
apb_taps={{}}
local function at() return math.floor(m.time:as_double()*10000000+0.5) end
local sound=m.devices[':audiocpu'].spaces['program']
apb_taps[1]=sound:install_read_tap(0x1800,0x187f,'apb-sound-read',function(a,d,mask)
 trace:write(string.format('r %04X %02X %d\\n',a,d,at()))
end)
apb_taps[2]=sound:install_write_tap(0x1800,0x187f,'apb-sound-write',function(a,d,mask)
 trace:write(string.format('w %04X %02X %d\\n',a,d,at()))
end)
local main=m.devices[':maincpu'].spaces['program']
local function mainread(a,d,mask)
 inputs:write(string.format('r %04X %04X %d\\n',a,d,at()))
end
apb_taps[3]=main:install_read_tap(0x1400,0x147f,'apb-adc',mainread)
apb_taps[4]=main:install_read_tap(0x1800,0x1bff,'apb-controls',mainread)
local function controls(frame)
 local n=frame-{offset}
 coin:set_value(((n>=900 and n<903) or (n>=930 and n<933)) and 1 or 0)
 siren:set_value(((n>=3100 and n<3110) or (n>=4000 and n<4010)) and 1 or 0)
 pedal:set_value(63)
end
controls(0)
emu.register_frame_done(function()
 local n=s:frame_number()
 if wanted[n] then
  local pixels,w,h=s:pixels()
  assert(w==512 and h==384 and #pixels==512*384*4)
  local f=assert(io.open({lua_string(out)}..'/mame-'..n..'.rgb32','wb'))
  f:write(pixels);f:close()
 end
 controls(n+1)
 if n>={max(frames)+5} then trace:close(); inputs:close(); m:exit() end
end,'frame')
'''
    script = out / 'capture.lua'
    script.write_text(lua, encoding='utf-8')
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    command = [mame, 'apb', '-rompath', out / 'roms', '-video', 'none', '-sound', 'none',
               '-nothrottle', '-skip_gameinfo', '-autoboot_delay', '0', '-autoboot_script', script,
               '-samplerate', '55930', '-wavwrite', out / 'mame.wav',
               '-seconds_to_run', str(math.ceil((max(frames)+10) * .01664) + 5)]
    for name in ('cfg', 'nvram', 'snapshot', 'state', 'diff'):
        command += [f'-{name}_directory', out / name]
    run(command, out / 'mame.log', env)
    command = [native, roms, '--frames', str(max(frames)+2), '--pedal', '63',
               '--coin', '900', '--coin', '930', '--press', '8:3100', '--press', '8:4000',
               '--wav', out / 'native.wav', '--audio-threaded', '1',
               '--io-trace', out / 'native-io.trace', '--input-trace', out / 'native-input.trace']
    for frame in frames:
        command += ['--frame-out', f'{frame-offset}:{out}/native-{frame}.png']
    run(command, out / 'native.log')
    result = {'mame_version': version, 'boot_offset': offset, 'scenario': 'two coins, start and acknowledge briefing; released pedal, stationary wheel', 'images': []}
    for frame in frames:
        # pixels() is host-endian 0x00RRGGBB before cabinet orientation.
        raw = np.fromfile(out / f'mame-{frame}.rgb32', dtype=np.uint32)
        if raw.size != 512 * 384:
            raise ValueError(f'Incomplete MAME frame {frame}')
        raw = raw.reshape(384, 512)
        rgb = np.rot90(np.stack([(raw >> 16) & 255, (raw >> 8) & 255, raw & 255], axis=2), 1).astype(np.uint8)
        actual = np.array(Image.open(out / f'native-{frame}.png').convert('RGB'))
        mismatch = np.any(actual != rgb, axis=2)
        count = int(np.count_nonzero(mismatch))
        Image.fromarray(rgb).save(out / f'mame-{frame}.png')
        if count:
            Image.fromarray((mismatch * 255).astype(np.uint8)).save(out / f'diff-{frame}.png')
        result['images'].append({'mame_frame': frame, 'native_frame': frame-offset,
                                 'different_pixels': count, 'total_pixels': int(mismatch.size)})
    start, end = map(float, args.audio_window.split(':'))
    result['traces'] = compare_traces(out)
    result['audio'] = compare_audio(out / 'native.wav', out / 'mame.wav', start, end)
    result['passed'] = not any(f['different_pixels'] for f in result['images']) and result['audio']['passed']
    (out / 'report.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        print(f'mame_compare: {e}', file=sys.stderr)
        sys.exit(2)
