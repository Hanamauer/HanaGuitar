"""Visual + numeric check of the HanaGuitar for indie bounces (numpy + Pillow only).

Produces demo/<name>.png: waveform, log-frequency spectrogram and (for the
single-note glide) a pitch track, so the sound can be inspected instead of
guessed at.
"""
import sys, wave, os
import numpy as np
from PIL import Image, ImageDraw

SR = 48000


def load(path):
    w = wave.open(path, 'rb')
    n, ch, sw = w.getnframes(), w.getnchannels(), w.getsampwidth()
    raw = np.frombuffer(w.readframes(n), dtype=np.uint8).reshape(-1, 3)
    v = (raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8)
         | (raw[:, 2].astype(np.int32) << 16))
    v = np.where(v >= 1 << 23, v - (1 << 24), v).astype(np.float64) / 8388607.0
    w.close()
    return v.reshape(-1, ch)


def stft(x, n=2048, hop=256):
    win = np.hanning(n)
    frames = 1 + max(0, (len(x) - n) // hop)
    S = np.empty((frames, n // 2 + 1), dtype=np.float32)
    for i in range(frames):
        seg = x[i * hop:i * hop + n] * win
        S[i] = np.abs(np.fft.rfft(seg))
    return S


def log_spec_img(S, fmin=40, fmax=16000, rows=360, cols=None):
    freqs = np.fft.rfftfreq(2048, 1 / SR)
    idx = np.searchsorted(freqs, np.geomspace(fmin, fmax, rows))
    idx = np.clip(idx, 0, len(freqs) - 1)
    M = S[:, idx].T[::-1]            # rows: high freq at top
    db = 20 * np.log10(M / 1024.0 + 1e-9)
    db = np.clip((db + 92) / 70, 0, 1) ** 0.8
    if cols:
        xp = np.linspace(0, 1, M.shape[1])
        xi = np.linspace(0, 1, cols)
        db = np.stack([np.interp(xi, xp, row) for row in db])
    return (db * 255).astype(np.uint8)


def cmap(v):
    # dark -> amber -> white, easy to read
    r = np.clip(v * 1.7, 0, 1) ** 0.8
    g = np.clip(v * 1.25 - 0.10, 0, 1) ** 1.1
    b = np.clip(v * 1.9 - 0.75, 0, 1) ** 1.4
    return np.stack([r, g, b], axis=-1)


def pitch_track(x, sr=SR, n=4096, hop=512, fmin=60, fmax=1200):
    out = []
    for i in range(0, len(x) - n, hop):
        f = x[i:i + n] - x[i:i + n].mean()
        if np.sqrt((f ** 2).mean()) < 1e-4:
            out.append(0.0)
            continue
        ac = np.correlate(f, f, 'full')[n - 1:]
        lo, hi = int(sr / fmax), int(sr / fmin)
        k = int(np.argmax(ac[lo:hi])) + lo
        if 0 < k < len(ac) - 1:
            a, b, c = ac[k - 1], ac[k], ac[k + 1]
            k = k + 0.5 * (a - c) / (a - 2 * b + c + 1e-12)
        out.append(sr / k)
    return np.array(out)


def analyse(name, with_pitch=False):
    path = f"demo/{name}.wav"
    v = load(path)
    x = v.mean(axis=1)
    peak, rms = np.abs(v).max(), np.sqrt((x ** 2).mean())
    d = np.abs(np.diff(x))
    print(f"{name:9s} peak {20*np.log10(peak):6.2f} dBFS  rms {20*np.log10(rms):6.2f} dBFS  "
          f"crest {20*np.log10(peak/rms):5.1f} dB  maxstep {d.max():.3f}")

    S = stft(x)
    spec = log_spec_img(S, cols=1100)
    H, W = spec.shape
    img = Image.new("RGB", (W, H + 120), (12, 12, 14))
    rgb = (cmap(spec / 255.0) * 255).astype(np.uint8)
    img.paste(Image.fromarray(rgb), (0, 50))

    # waveform strip
    dr = ImageDraw.Draw(img)
    env = np.abs(x)
    step = max(1, len(env) // W)
    col = [env[i:i + step].max() for i in range(0, len(env) - step, step)][:W]
    for xx, a in enumerate(col):
        h = int(a * 45)
        dr.line([(xx, 48 - h), (xx, 48)], fill=(90, 220, 200))

    if with_pitch:
        p = pitch_track(x)
        valid = p > 0
        semis = 12 * np.log2(np.maximum(p, 1e-6) / 246.94)
        pts = []
        for i, s in enumerate(semis):
            if valid[i]:
                t = i * 512 / SR
                pts.append((int(t / (len(x) / SR) * W), int(50 + H - (s + 6) / 12 * H)))
        if len(pts) > 1:
            dr.line(pts, fill=(255, 220, 90), width=2)
        dr.text((8, 8), f"{name}: pitch (line) over spectrogram", fill=(230, 230, 230))

    dr.text((8, 8), f"{name}  peak {20*np.log10(peak):.1f} dBFS  rms {20*np.log10(rms):.1f} dBFS",
            fill=(230, 230, 230))
    out = f"demo/{name}.png"
    img.save(out)
    print("   wrote", out, img.size)


if __name__ == "__main__":
    names = sys.argv[1:] or ["shoegaze", "midwest", "glide"]
    for n in names:
        analyse(n, with_pitch=(n == "glide"))
