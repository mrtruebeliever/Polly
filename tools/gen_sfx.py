#!/usr/bin/env python3
"""Generates Polly's chirp samples (resources/sfx/*.raw).

Tapping the parrot used to play three pure sine notes, which sounds like a
doorbell. These two samples sound like a bird instead -- a whistled tweet and a
parrot's rough squawk -- and the app plays them through speaker_play_tracks() at
a few pitches and patterns, so no two taps sound quite alike. They are computed,
not recorded, so there is nothing to license; rerun after changing them.

Output: mono, signed 8-bit PCM at 16 kHz (SpeakerPcmFormat_16kHz_8bit).

    python3 tools/gen_sfx.py            # writes resources/sfx/*.raw
    python3 tools/gen_sfx.py --wav DIR  # also writes .wav previews to DIR
"""
import argparse
import math
import os
import random
import struct
import wave

RATE = 16000
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'resources', 'sfx')


def tweet():
    """A whistled upward sweep with a quick warble, the way small birds chirp."""
    dur = 0.13
    n = int(RATE * dur)
    out, phase = [], 0.0
    for i in range(n):
        t = i / RATE
        x = t / dur
        f = 2100 + 1500 * x ** 0.7 + 140 * math.sin(2 * math.pi * 38 * t)
        phase += 2 * math.pi * f / RATE
        env = math.sin(math.pi * x) ** 1.4
        out.append(math.tanh(1.4 * math.sin(phase)) * env)
    return out


class Resonator:
    """Two-pole band-pass: gives the buzz a vocal colour (a formant)."""
    def __init__(self, freq, bw):
        r = math.exp(-math.pi * bw / RATE)
        self.a1 = 2 * r * math.cos(2 * math.pi * freq / RATE)
        self.a2 = -r * r
        self.g = 1 - r
        self.y1 = self.y2 = 0.0

    def __call__(self, x):
        y = self.g * x + self.a1 * self.y1 + self.a2 * self.y2
        self.y2, self.y1 = self.y1, y
        return y


def squawk():
    """A parrot's rough 'krraa': a buzzy, slightly noisy tone whose pitch rises
    and falls, shaped by two formants so it reads as a voice, not a buzzer."""
    rnd = random.Random(3)
    dur = 0.22
    n = int(RATE * dur)
    f1, f2 = Resonator(1400, 300), Resonator(2600, 400)
    out, phase = [], 0.0
    for i in range(n):
        t = i / RATE
        x = t / dur
        f0 = 560 + 220 * math.sin(math.pi * x) + 30 * math.sin(2 * math.pi * 55 * t)
        phase = (phase + f0 / RATE) % 1.0
        buzz = 2 * phase - 1 + rnd.uniform(-0.45, 0.45)
        y = f1(buzz) + 0.7 * f2(buzz)
        env = min(1.0, t / 0.012) * (1 - x) ** 0.6
        out.append(y * env)
    return out


def to_pcm8(samples):
    peak = max(abs(s) for s in samples) or 1.0
    return b''.join(struct.pack('b', max(-127, min(127, int(round(s / peak * 120)))))
                    for s in samples)


def write_wav(path, samples):
    peak = max(abs(s) for s in samples) or 1.0
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', int(s / peak * 30000)) for s in samples))


SOUNDS = {'tweet': tweet, 'squawk': squawk}

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--wav', help='also write .wav previews to this directory')
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    for name, fn in SOUNDS.items():
        s = fn()
        with open(os.path.join(OUT, name + '.raw'), 'wb') as f:
            f.write(to_pcm8(s))
        if args.wav:
            write_wav(os.path.join(args.wav, name + '.wav'), s)
        print('%-6s %5d bytes' % (name, len(s)))
