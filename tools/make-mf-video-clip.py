#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make the Media Foundation probe's clip and what the host decodes from it (tests/mf-video-probe.c).

The clip is a generated test pattern, never game data: 1920x1080 at 60 frames a second for 5 s, H.264 High (x264,
B-frames, CABAC) at 12 Mbit/s with moving noise so decoding costs what a game's intro does, and a 48 kHz stereo AAC
tone (440 and 660 Hz at half scale). The host's FFmpeg then decodes it, and the probe must agree:

  .deps/probes/mf-video.mp4   the clip (staged as C:\\mf-video.mp4)
  .deps/probes/mf-video.txt   width height frames luma-crc32 chroma-crc32 sample-frame mean-b mean-g mean-r
                              audio-frames rms/1000

The NV12 frames are compared bit for bit (CRC-32 of the visible luma rows, and apart of the interleaved chroma rows,
in presentation order): H.264 decoding is exact, and NV12 is the decoder's own 4:2:0 with its chroma interleaved.
The RGB32 frame's mean colour and the audio are compared within tolerances, since colour conversion and AAC decoding
round differently between builds.

--expect CLIP writes the same expectations for an existing clip (CLIP with .txt for its extension), whatever its
container and codecs: a clip without audio expects 0 audio frames, and the probe then skips its audio pass.

  tools/make-mf-video-clip.py [--out DIR] [--expect CLIP]
"""
from pathlib import Path
import argparse
import array
import math
import shutil
import subprocess
import zlib

import json

WIDTH, HEIGHT, RATE, SECONDS, SAMPLE_FRAME = 1920, 1080, 60, 5, 150


def decode(clip, *args, size=1 << 22):
    """Stream the host FFmpeg's decode of the clip's first video or audio track, in pieces of `size` bytes."""
    command = ['ffmpeg', '-v', 'error', '-nostdin', '-i', str(clip), *args, '-']
    process = subprocess.Popen(command, stdout=subprocess.PIPE)
    while chunk := process.stdout.read(size):
        yield chunk
    if process.wait():
        raise SystemExit(f'make-mf-video-clip: {" ".join(command)} failed')


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--out', type=Path, default=root / '.deps/probes')
    parser.add_argument('--expect', type=Path, help='only write the expectations of an existing clip')
    args = parser.parse_args()
    if args.expect:
        write_expectations(args.expect, args.expect.with_suffix('.txt'))
        return
    if not shutil.which('ffmpeg'):
        raise SystemExit('make-mf-video-clip: the host has no ffmpeg (with libx264 and aac) to make the clip')
    args.out.mkdir(parents=True, exist_ok=True)
    clip = args.out / 'mf-video.mp4'
    subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', '-y', '-filter_threads', '1',
                    '-f', 'lavfi', '-i', f'testsrc2=size={WIDTH}x{HEIGHT}:rate={RATE}:duration={SECONDS},'
                                         'noise=alls=12:allf=t:all_seed=5',
                    '-f', 'lavfi', '-i', f'aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*660*t):s=48000:d={SECONDS}',
                    '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-profile:v', 'high', '-preset', 'medium',
                    '-b:v', '12M', '-g', str(RATE),
                    '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', '-map_metadata', '-1',
                    str(clip)], check=True)
    write_expectations(clip, args.out / 'mf-video.txt')


def write_expectations(clip, out):
    """What the host's FFmpeg decodes from the clip, in the probe's order (see the module's docstring)."""
    probe = json.loads(subprocess.run(['ffprobe', '-v', 'error', '-of', 'json', '-show_streams', str(clip)],
                                      check=True, capture_output=True, text=True).stdout)['streams']
    video = next(s for s in probe if s['codec_type'] == 'video')
    width, height = video['width'], video['height']
    has_audio = any(s['codec_type'] == 'audio' for s in probe)
    luma, frames, crc_luma, crc_chroma = width * height, 0, 0, 0
    for frame in decode(clip, '-map', '0:v:0', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-pix_fmt', 'nv12',
                        size=luma * 3 // 2):
        if len(frame) != luma * 3 // 2:
            raise SystemExit('make-mf-video-clip: the host decode ended inside a frame')
        crc_luma = zlib.crc32(frame[:luma], crc_luma)
        crc_chroma = zlib.crc32(frame[luma:], crc_chroma)
        frames += 1
    bgra = b''.join(decode(clip, '-map', '0:v:0', '-vf', f'select=eq(n\\,{SAMPLE_FRAME})', '-frames:v', '1',
                           '-f', 'rawvideo', '-pix_fmt', 'bgra'))
    means = [round(sum(bgra[c::4]) / (width * height)) for c in range(3)]
    samples = array.array('f')
    if has_audio:
        samples.frombytes(b''.join(decode(clip, '-map', '0:a:0', '-f', 'f32le', '-ac', '2', '-ar', '48000')))
    rms = math.sqrt(sum(v * v for v in samples) / len(samples)) if samples else 0
    expected = (f'{width} {height} {frames} {crc_luma:08x} {crc_chroma:08x} {SAMPLE_FRAME} {means[0]} {means[1]} '
                f'{means[2]} {len(samples) // 2} {round(rms * 1000)}\n')
    out.write_text(expected)
    print(f'make-mf-video-clip: {clip} ({clip.stat().st_size} bytes): {expected.strip()}')


if __name__ == '__main__':
    main()
