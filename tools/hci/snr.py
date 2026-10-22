"""SNR of a decoded file against the reference, after finding the codec delay.

usage: python snr.py reference.wav decoded.wav   (16-bit PCM WAVs, same rate)
"""
import math
import struct
import sys
import wave


def read(path):
    w = wave.open(path, 'rb')
    ch, n = w.getnchannels(), w.getnframes()
    raw = w.readframes(n)
    s = struct.unpack('<%dh' % (len(raw) // 2), raw)
    return [s[i::ch] for i in range(ch)], w.getframerate()


ref, rate = read(sys.argv[1])
dec, rate2 = read(sys.argv[2])
assert rate == rate2, 'sample rates differ'

# skip the first 0.5 s (encoder start-up), compare 2 s
start, length = rate // 2, 2 * rate
r = ref[0][start:start + length]
best = None
for delay in range(0, 2048):
    d = dec[0][start + delay:start + delay + length]
    if len(d) < length:
        break
    err = sum((a - b) ** 2 for a, b in zip(r[:4800], d[:4800]))
    if best is None or err < best[0]:
        best = (err, delay)
delay = best[1]

for c in range(len(ref)):
    r = ref[c][start:start + length]
    d = dec[c][start + delay:start + delay + length]
    sig = sum(a * a for a in r)
    # Least-squares gain separates a level offset (scaling in a decoder's
    # output format) from actual coding noise.
    gain = sum(a * b for a, b in zip(r, d)) / (sum(b * b for b in d) or 1)
    noise = sum((a - gain * b) ** 2 for a, b in zip(r, d)) or 1
    print(f'channel {c}: delay {delay} samples, gain {20 * math.log10(gain):+.2f} dB, '
          f'SNR {10 * math.log10(sig / noise):.1f} dB')
