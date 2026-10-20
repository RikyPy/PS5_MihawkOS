#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""MihawkOS's own music, in the Frutiger Aero manner: original pieces made only by this script (no recording, no
sample, no third-party note), seeded, so every run gives the same samples. Like the desktop's Aero Meadow
(tools/make-mihawk-wallpaper.py), it is drawn, not sourced.

  tools/make-mihawk-music.py OUT_DIR [--only NAME] [--wav]   each piece as NAME.ogg (and NAME.wav for listening), and
                                                              music.json: their lengths, loudness and hashes

The title ships the pieces from ps5/assets/music (made by this script; tests/test_mihawk_music.py renders them again
and compares every byte), and the launcher and the first-launch tour play them (examples/ps5proton/launcher_music.hpp).

What it sounds like:
- bright major and Lydian harmony: add9 and major-seventh pads of detuned soft saws, slow to swell, filtered;
- glassy bells (FM with an inharmonic modulator), marimba and celesta motifs, pentatonic and sparse;
- bubbles (short sines gliding up an octave) and water-drop plinks;
- a soft room (a convolution reverb from seeded noise) and a ping-pong echo on the bells;
- in some pieces a calm brushed beat at 72-88 BPM.

Each piece loops without a seam: it is rendered a few seconds long, and that tail (the last notes' decay and the
room's) is folded back into its start, as the loop's next pass would hear it. It is normalised to -18 LUFS (ITU-R
BS.1770: K-weighted, gated) with peaks under -1 dBFS. It is encoded as OGG Vorbis by tools/vorbis-encode.c, built
here against libogg 1.3.5 and libvorbis 1.3.7 from their pinned tarballs, with a fixed stream serial, so the file is
the same bytes on every run. LOOPSTART and LOOPLENGTH comments let a player loop it.
"""
import argparse
import hashlib
import json
import math
import subprocess
import tarfile
import urllib.request
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
RATE = 48000
SEED = 20261019
TAIL = 6.0            # seconds rendered past the loop, folded back into its start
LOUDNESS = -18.0      # LUFS
PEAK = 10 ** (-1.0 / 20)
QUALITY = 0.5         # Vorbis VBR quality (about 160 kb/s for these pieces)
LIBOGG = ("https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz",
          "c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705")
LIBVORBIS = ("https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz",
             "b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b")


def hz(note):
    """A MIDI note's frequency."""
    return 440.0 * 2.0 ** ((note - 69) / 12.0)


def ramp(n, attack, release):
    """An envelope of n samples: a smooth rise over attack seconds and a fall over the last release seconds."""
    t = np.arange(n) / RATE
    env = np.ones(n)
    if attack > 0:
        rise = np.clip(t / attack, 0, 1)
        env *= rise * rise * (3 - 2 * rise)
    if release > 0:
        fall = np.clip((n / RATE - t) / release, 0, 1)
        env *= fall * fall * (3 - 2 * fall)
    return env


# ---- voices: each returns a mono signal starting at its note's onset ----

def pad(freq, seconds, rng):
    """A soft saw pad: three detuned voices, eight harmonics rolled off like a gentle low-pass."""
    n = int((seconds + 2.5) * RATE)
    t = np.arange(n) / RATE
    out = np.zeros(n)
    for cents in (-7.0, 0.0, 6.0):
        f = freq * 2 ** (cents / 1200)
        for k in range(1, 9):
            if f * k > 6000:
                break
            out += np.sin(2 * np.pi * f * k * t + rng.uniform(0, 2 * np.pi)) * (1.0 / k) * math.exp(-(k - 1) / 2.2)
    return out * ramp(n, 1.6, 2.4) / 3.0


def bell(freq, rng):
    """A glassy bell: FM with a 3.5 ratio modulator whose depth fades, and a quiet inharmonic partial."""
    n = int(3.2 * RATE)
    t = np.arange(n) / RATE
    index = 1.8 * np.exp(-t / 0.35)
    tone = np.sin(2 * np.pi * freq * t + index * np.sin(2 * np.pi * freq * 3.5 * t)) * np.exp(-t / 1.05)
    tone += 0.22 * np.sin(2 * np.pi * freq * 2.76 * t) * np.exp(-t / 0.45)
    return tone * ramp(n, 0.003, 0.05)


def marimba(freq, rng):
    """A wooden bar: the fundamental, its 3.93 overtone and a tick, each dying faster."""
    n = int(1.6 * RATE)
    t = np.arange(n) / RATE
    tone = np.sin(2 * np.pi * freq * t) * np.exp(-t / 0.55)
    tone += 0.32 * np.sin(2 * np.pi * freq * 3.93 * t) * np.exp(-t / 0.13)
    tone += 0.08 * np.sin(2 * np.pi * freq * 9.2 * t) * np.exp(-t / 0.035)
    return tone * ramp(n, 0.002, 0.05)


def celesta(freq, rng):
    """A celesta: a pure tone with a metallic fourth partial that fades first."""
    n = int(2.4 * RATE)
    t = np.arange(n) / RATE
    tone = np.sin(2 * np.pi * freq * t) * np.exp(-t / 0.95)
    tone += 0.30 * np.sin(2 * np.pi * freq * 4.0 * t) * np.exp(-t / 0.30)
    tone += 0.12 * np.sin(2 * np.pi * freq * 2.0 * t) * np.exp(-t / 0.6)
    return tone * ramp(n, 0.002, 0.05)


def bubble(freq, rng):
    """A bubble: a sine gliding up an octave in 70 ms, gone in a tenth of a second."""
    n = int(0.16 * RATE)
    t = np.arange(n) / RATE
    glide = 0.07 / math.log(2)
    phase = 2 * np.pi * freq * glide * (2 ** (t / 0.07) - 1)
    return np.sin(phase) * (1 - np.exp(-t / 0.003)) * np.exp(-t / 0.032)


def drop(freq, rng):
    """A water drop: a plink falling an octave, very short."""
    n = int(0.18 * RATE)
    t = np.arange(n) / RATE
    glide = 0.045 / math.log(2)
    phase = 2 * np.pi * freq * glide * (1 - 2 ** (-t / 0.045))
    return np.sin(phase) * (1 - np.exp(-t / 0.002)) * np.exp(-t / 0.05)


def shaker(level, rng):
    """A brushed shaker: bright noise, a quick swell and a short fall."""
    n = int(0.12 * RATE)
    noise = rng.standard_normal(n + 2)
    bright = noise[2:] - 2 * noise[1:-1] + noise[:-2]   # a second difference: the hiss without the rumble
    t = np.arange(n) / RATE
    return level * bright * (1 - np.exp(-t / 0.006)) * np.exp(-t / 0.028) / 4.0


def kick(level, rng):
    """A soft kick: a low sine falling from 110 to 48 Hz, round and short."""
    n = int(0.5 * RATE)
    t = np.arange(n) / RATE
    phase = 2 * np.pi * (48 * t + 62 * 0.04 * (1 - np.exp(-t / 0.04)))
    return level * np.sin(phase) * (1 - np.exp(-t / 0.004)) * np.exp(-t / 0.22)


def bass(freq, seconds, rng):
    """A round bass under each chord: a sine and a little of its octave."""
    n = int((seconds + 0.6) * RATE)
    t = np.arange(n) / RATE
    return (np.sin(2 * np.pi * freq * t) + 0.18 * np.sin(4 * np.pi * freq * t)) * ramp(n, 0.06, 0.6)


# ---- mixing ----

class Bus:
    """A stereo track: voices are added at a time, a level and a pan (-1 left .. 1 right, constant power)."""

    def __init__(self, seconds):
        self.data = np.zeros((2, int((seconds + TAIL) * RATE) + RATE))

    def add(self, at, signal, level=1.0, pan=0.0):
        start = int(round(at * RATE))
        end = min(start + len(signal), self.data.shape[1])
        if end <= start:
            return
        angle = (pan + 1) * np.pi / 4
        self.data[0, start:end] += signal[:end - start] * level * math.cos(angle)
        self.data[1, start:end] += signal[:end - start] * level * math.sin(angle)


def echo(data, delay, taps, gain):
    """A ping-pong echo: taps copies, each delay later and gain softer, alternating sides."""
    out = data.copy()
    shift = int(delay * RATE)
    for k in range(1, taps + 1):
        side = k % 2
        moved = np.zeros_like(data)
        moved[:, shift * k:] = data[:, :-shift * k]
        out[side] += moved[side] * gain ** k * 0.9
        out[1 - side] += moved[1 - side] * gain ** k * 0.4
    return out


def room(data, seconds, rng):
    """A soft room: each side convolved with its own decaying, darkened seeded noise (RT60 seconds)."""
    n = int(seconds * RATE)
    t = np.arange(n) / RATE
    out = np.zeros((2, data.shape[1] + n - 1))
    size = 1 << int(math.ceil(math.log2(data.shape[1] + n)))
    for side in range(2):
        noise = rng.standard_normal(n) * np.exp(-t * 6.9 / seconds)
        noise = np.convolve(noise, np.ones(9) / 9, mode="same")         # darker: a gentle low-pass
        noise[: int(0.022 * RATE)] = 0                                    # 22 ms before the first reflection
        noise /= math.sqrt(float(np.sum(noise * noise)))
        out[side] = np.fft.irfft(np.fft.rfft(data[side], size) * np.fft.rfft(noise, size), size)[: out.shape[1]]
    return out


def fold(data, seconds):
    """The loop: what sounds past its end is added to its start, as the next pass would hear it."""
    n = int(round(seconds * RATE))
    out = data[:, :n].copy()
    rest = data[:, n:]
    while rest.shape[1]:
        take = min(n, rest.shape[1])
        out[:, :take] += rest[:, :take]
        rest = rest[:, take:]
    return out


def loudness(data):
    """Integrated loudness in LUFS (ITU-R BS.1770-4): K-weighted (both stages, applied by their magnitude response,
    which is all a mean square needs), 400 ms blocks with 75% overlap, absolute and relative gates."""
    size = 1 << int(math.ceil(math.log2(data.shape[1])))
    w = np.linspace(0, np.pi, size // 2 + 1)
    z = np.exp(-1j * w)

    def response(b, a):
        return (b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z)

    gain = np.abs(response((1.53512485958697, -2.69169618940638, 1.19839281085285),
                           (1.0, -1.69065929318241, 0.73248077421585)) *
                  response((1.0, -2.0, 1.0), (1.0, -1.99004745483398, 0.99007225036621)))
    weighted = np.fft.irfft(np.fft.rfft(data, size, axis=1) * gain, size, axis=1)[:, : data.shape[1]]
    block, hop = int(0.4 * RATE), int(0.1 * RATE)
    power = np.array([np.mean(weighted[:, i:i + block] ** 2, axis=1).sum()
                      for i in range(0, data.shape[1] - block + 1, hop)])
    lufs = -0.691 + 10 * np.log10(np.maximum(power, 1e-20))
    gated = power[lufs > -70]
    relative = -0.691 + 10 * math.log10(float(np.mean(gated))) - 10
    final = gated[-0.691 + 10 * np.log10(gated) > relative]
    return -0.691 + 10 * math.log10(float(np.mean(final)))


# ---- the pieces ----

def piece_bubble_garden(rng):
    """Bubble Garden: F Lydian at 80 BPM, 32 bars. A swelling Lydian pad, bells from bar 5, bubbles throughout, a soft
    brushed beat from bar 9 to bar 28."""
    beat = 60 / 80
    bar = 4 * beat
    seconds = 32 * bar
    chords = [  # root for the bass, then the pad's notes, two bars each
        (41, [53, 57, 60, 64, 67]),      # Fmaj9
        (43, [55, 59, 62, 69]),          # G add9: the Lydian II
        (45, [57, 60, 64, 67]),          # Am7
        (47, [55, 59, 62, 69]),          # G add9 over B: back to the start
    ]
    pentatonic = [65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 84]   # F G A B C D E F G A C: Lydian, sparse
    pads, bells, sparkle, beatbus, low = Bus(seconds), Bus(seconds), Bus(seconds), Bus(seconds), Bus(seconds)
    for i in range(16):
        root, notes = chords[i % 4]
        at = i * 2 * bar
        for k, note in enumerate(notes):
            pads.add(at, pad(hz(note), 2 * bar, rng), 0.055, -0.5 + k / max(1, len(notes) - 1))
        low.add(at, bass(hz(root), 2 * bar, rng), 0.20)
    motif = [(0.0, 4), (1.0, 6), (1.5, 7), (3.0, 5), (4.5, 8), (6.0, 6)]   # (beat, scale degree) over two bars
    for i in range(2, 16):
        at = i * 2 * bar
        shift = [0, 1, -1, 2][i % 4]
        for b, degree in motif:
            if rng.random() < 0.82:
                note = pentatonic[max(0, min(len(pentatonic) - 1, degree + shift))]
                bells.add(at + b * beat, bell(hz(note + 12), rng), 0.11, rng.uniform(-0.6, 0.6))
    t = 0.4
    while t < seconds:
        sparkle.add(t, bubble(rng.uniform(650, 1500), rng), 0.10, rng.uniform(-0.8, 0.8))
        if rng.random() < 0.3:
            sparkle.add(t + 0.06, bubble(rng.uniform(900, 1800), rng), 0.07, rng.uniform(-0.8, 0.8))
        t += rng.uniform(0.7, 2.2)
    for b in range(8 * 4, 28 * 4):
        at = b * beat
        if b % 4 == 0 or b % 4 == 2:
            beatbus.add(at, kick(1.0, rng), 0.22)
        for half, swing in ((0.0, 0.0), (0.5, 0.06)):
            beatbus.add(at + (half + swing) * beat, shaker(1.0, rng), 0.05 if half else 0.035, 0.3)
    return seconds, beat, pads, bells, sparkle, beatbus, low


def piece_glass_meadow(rng):
    """Glass Meadow: C major at 72 BPM, 24 bars. Marimba arpeggios over an add9 pad, water drops, a celesta answer
    in the second half; no beat."""
    beat = 60 / 72
    bar = 4 * beat
    seconds = 24 * bar
    chords = [(36, [48, 55, 62, 64, 67]), (45, [57, 60, 64, 67, 71]), (41, [53, 57, 60, 64, 67]),
              (43, [55, 57, 62, 67, 69])]   # Cadd9, Am9, Fmaj7, Gsus2
    pads, bells, sparkle, beatbus, low = Bus(seconds), Bus(seconds), Bus(seconds), Bus(seconds), Bus(seconds)
    for i in range(12):
        root, notes = chords[i % 4]
        at = i * 2 * bar
        for k, note in enumerate(notes):
            pads.add(at, pad(hz(note), 2 * bar, rng), 0.045, -0.6 + 1.2 * k / (len(notes) - 1))
        low.add(at, bass(hz(root), 2 * bar, rng), 0.18)
        tones = sorted(set(n + 12 for n in notes[1:]))
        order = tones + tones[-2:0:-1]
        for step in range(16):   # eighths over two bars, up and down the chord
            if step % 8 == 7 and rng.random() < 0.5:
                continue
            note = order[step % len(order)]
            bells.add(at + step * beat / 2, marimba(hz(note), rng), 0.16 if step % 4 == 0 else 0.11,
                      -0.4 + 0.8 * (step % len(order)) / len(order))
        if i >= 6:
            for b, offset in ((1.0, 24), (3.5, 26), (6.0, 21)):
                if rng.random() < 0.7:
                    bells.add(at + b * beat, celesta(hz(notes[-1] + offset - 12), rng), 0.08, 0.5)
    t = 1.1
    while t < seconds:
        sparkle.add(t, drop(rng.uniform(1100, 1900), rng), 0.09, rng.uniform(-0.9, 0.9))
        if rng.random() < 0.4:
            sparkle.add(t + rng.uniform(0.2, 0.5), bubble(rng.uniform(600, 1100), rng), 0.08, rng.uniform(-0.7, 0.7))
        t += rng.uniform(1.4, 3.6)
    return seconds, beat, pads, bells, sparkle, beatbus, low


def piece_sky_lounge(rng):
    """Sky Lounge: D major at 88 BPM, 32 bars. A celesta hook over a calm brushed beat, a Lydian turn in the third
    chord, bubbles rising in pairs."""
    beat = 60 / 88
    bar = 4 * beat
    seconds = 32 * bar
    chords = [(38, [50, 57, 61, 64, 66]), (47, [50, 54, 57, 61, 62]), (43, [50, 54, 59, 61, 66]),
              (45, [52, 57, 59, 62, 66])]   # Dmaj9, Bm9, Gmaj7#11, A6sus
    hook = [(0.0, 74), (0.5, 76), (1.0, 78), (2.0, 81), (3.0, 78), (4.5, 76), (5.0, 74), (6.0, 71), (7.0, 74)]
    pads, bells, sparkle, beatbus, low = Bus(seconds), Bus(seconds), Bus(seconds), Bus(seconds), Bus(seconds)
    for i in range(16):
        root, notes = chords[i % 4]
        at = i * 2 * bar
        for k, note in enumerate(notes):
            pads.add(at, pad(hz(note), 2 * bar, rng), 0.05, -0.5 + k / (len(notes) - 1))
        low.add(at, bass(hz(root), 2 * bar, rng), 0.20)
        if 4 <= i < 14:
            for b, note in hook:
                if i % 4 == 3 and b > 4:
                    note = note + 2   # the turn: a lifted answer
                if rng.random() < 0.9:
                    bells.add(at + b * beat, celesta(hz(note + 12), rng), 0.10, rng.uniform(-0.3, 0.5))
    for b in range(4 * 4, 32 * 4):
        at = b * beat
        if b % 4 in (0, 2):
            beatbus.add(at, kick(1.0, rng), 0.24 if b % 4 == 0 else 0.16)
        for q in range(4):
            beatbus.add(at + (q * 0.25 + (0.04 if q % 2 else 0.0)) * beat, shaker(1.0, rng),
                        0.045 if q == 2 else 0.025, -0.2)
    t = 0.8
    while t < seconds:
        f = rng.uniform(700, 1300)
        sparkle.add(t, bubble(f, rng), 0.09, rng.uniform(-0.8, 0.8))
        sparkle.add(t + 0.09, bubble(f * 1.5, rng), 0.06, rng.uniform(-0.8, 0.8))
        t += rng.uniform(1.2, 3.0)
    return seconds, beat, pads, bells, sparkle, beatbus, low


PIECES = {"bubble-garden": piece_bubble_garden, "glass-meadow": piece_glass_meadow, "sky-lounge": piece_sky_lounge}


def render(name):
    """One piece as 16-bit stereo samples, looped, normalised; with what it measured."""
    rng = np.random.default_rng([SEED, list(PIECES).index(name)])
    seconds, beat, pads, bells, sparkle, beatbus, low = PIECES[name](rng)
    bells_wet = echo(bells.data, 0.75 * beat, 3, 0.45)   # a dotted eighth
    dry = pads.data + bells_wet + sparkle.data + beatbus.data + low.data
    send = 0.55 * pads.data + 0.8 * bells_wet + 0.6 * sparkle.data + 0.2 * beatbus.data
    wet = room(send, 2.6, rng)
    mix = np.zeros((2, wet.shape[1]))
    mix[:, : dry.shape[1]] += dry
    mix += 0.42 * wet
    loop = fold(mix, seconds)
    gain = 10 ** ((LOUDNESS - loudness(loop)) / 20)
    loop *= gain
    peak = float(np.max(np.abs(loop)))
    if peak > PEAK:   # a soft knee above 0.7 of the ceiling: rare at -18 LUFS
        knee = 0.7 * PEAK
        over = np.abs(loop) > knee
        loop[over] = np.sign(loop[over]) * (knee + (PEAK - knee) * np.tanh((np.abs(loop[over]) - knee) / (PEAK - knee)))
    samples = np.round(np.clip(loop, -1, 32767 / 32768) * 32767).astype("<i2")
    return samples, seconds, loudness(samples.astype(float) / 32767), float(np.max(np.abs(samples))) / 32767


def fetch(url, sha256, folder):
    path = folder / url.rsplit("/", 1)[1]
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != sha256:
        folder.mkdir(parents=True, exist_ok=True)
        data = urllib.request.urlopen(url, timeout=120).read()
        if hashlib.sha256(data).hexdigest() != sha256:
            raise SystemExit(f"{url}: not the pinned file")
        path.write_bytes(data)
    return path


def encoder():
    """tools/vorbis-encode.c against the pinned libogg and libvorbis, built once into .deps/vorbis-encoder."""
    work = ROOT / ".deps/vorbis-encoder"
    binary = work / "vorbis-encode"
    stamp = work / "stamp"
    want = hashlib.sha256((LIBOGG[1] + LIBVORBIS[1]).encode() + (ROOT / "tools/vorbis-encode.c").read_bytes()).hexdigest()
    if binary.is_file() and stamp.is_file() and stamp.read_text() == want:
        return binary
    sources = []
    for url, sha256 in (LIBOGG, LIBVORBIS):
        archive = fetch(url, sha256, work / "downloads")
        with tarfile.open(archive) as tar:
            tar.extractall(work, filter="data")
    ogg, vorbis = work / "libogg-1.3.5", work / "libvorbis-1.3.7"
    (ogg / "include/ogg/config_types.h").write_text(
        "#ifndef __CONFIG_TYPES_H__\n#define __CONFIG_TYPES_H__\n#include <stdint.h>\n"
        "typedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\ntypedef int32_t ogg_int32_t;\n"
        "typedef uint32_t ogg_uint32_t;\ntypedef int64_t ogg_int64_t;\ntypedef uint64_t ogg_uint64_t;\n#endif\n")
    sources += [ogg / "src/bitwise.c", ogg / "src/framing.c"]
    sources += [path for path in sorted((vorbis / "lib").glob("*.c"))
                if path.name not in ("barkmel.c", "psytune.c", "tone.c", "vorbisfile.c")]
    subprocess.run(["cc", "-std=gnu99", "-O2", "-ffp-contract=off", "-w", "-I", str(ogg / "include"),
                    "-I", str(vorbis / "include"), "-I", str(vorbis / "lib"), str(ROOT / "tools/vorbis-encode.c"),
                    *map(str, sources), "-lm", "-o", str(binary)], check=True)
    stamp.write_text(want)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", type=Path)
    parser.add_argument("--only", choices=PIECES)
    parser.add_argument("--wav", action="store_true", help="also write each piece as WAV, for listening")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    encode = encoder()
    record = {"numpy": np.__version__, "seed": SEED, "pieces": {}}
    for name in [args.only] if args.only else PIECES:
        samples, seconds, measured, peak = render(name)
        raw = args.out / (name + ".raw")
        raw.write_bytes(samples.T.tobytes())
        frames = samples.shape[1]
        ogg = args.out / (name + ".ogg")
        subprocess.run([str(encode), str(raw), str(ogg), str(QUALITY), "20261019", f"TITLE={name}",
                        "ARTIST=MihawkOS (tools/make-mihawk-music.py)", "LOOPSTART=0", f"LOOPLENGTH={frames}"], check=True)
        if args.wav:
            with wave.open(str(args.out / (name + ".wav")), "wb") as preview:
                preview.setnchannels(2)
                preview.setsampwidth(2)
                preview.setframerate(RATE)
                preview.writeframes(raw.read_bytes())
        record["pieces"][name] = {
            "seconds": round(frames / RATE, 3), "frames": frames, "lufs": round(measured, 2),
            "peak_dbfs": round(20 * math.log10(max(peak, 1e-9)), 2),
            "pcm_sha256": hashlib.sha256(raw.read_bytes()).hexdigest(),
            "ogg_sha256": hashlib.sha256(ogg.read_bytes()).hexdigest(), "ogg_bytes": ogg.stat().st_size}
        raw.unlink()
        print(f"{name}: {frames / RATE:.1f} s, {measured:.2f} LUFS, peak {record['pieces'][name]['peak_dbfs']} dBFS, "
              f"{ogg.stat().st_size} bytes")
    (args.out / "music.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
