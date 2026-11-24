"""Build the optional Amiga 22,050 Hz signed PCM bank from extracted samples."""
import argparse
from functools import lru_cache
import math
import operator
from pathlib import Path
import tempfile
import wave

ROM_SIZE = 4 * 1024 * 1024
MAGIC = b"ORPCM22\x01"


def coefficients():
    """31-tap Blackman-windowed low-pass, cutoff 10 kHz at 44.1 kHz."""
    cutoff = 10000 / 44100
    taps = []
    for i in range(31):
        x = i - 15
        sinc = 2 * cutoff if x == 0 else math.sin(2 * math.pi * cutoff * x) / (math.pi * x)
        window = 0.42 - 0.5 * math.cos(2 * math.pi * i / 30) + 0.08 * math.cos(4 * math.pi * i / 30)
        taps.append(sinc * window)
    scale = sum(taps)
    return [tap / scale for tap in taps]


def prepare(folder):
    samples = folder / "sound" / "samples"
    bank = bytearray([255]) * (ROM_SIZE // 2)
    taps = coefficients()

    @lru_cache(maxsize=32)
    def convert(name, parity):
        with wave.open(str(samples / (name + ".wav")), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 1, 44100):
                raise ValueError(f"{name} requires 44,100 Hz mono 8-bit PCM")
            size = wav.getnframes()
            pcm = wav.readframes(size)
        if len(pcm) != size or not size:
            raise ValueError(f"Incomplete or empty sample {name}")
        data = pcm[:1] * 15 + pcm + pcm[-1:] * 15
        gain = 10 ** (-12 / 20) if name == "tyres/off_road" else 1.0
        out = bytearray()
        for i in range(parity, size, 2):
            filtered = sum(map(operator.mul, taps, data[i:i + 31]))
            value = round((filtered - 128) * gain)
            out.append(max(-128, min(127, value)) & 255)
        return size, out

    for line in (samples / "samples.txt").read_text().splitlines():
        fields = line.partition("#")[0].split()
        if not fields or fields[0] in ("page", "uses"):
            continue
        address, length = (int(v, 16) for v in fields[:2])
        size, data = convert(fields[2], address & 1)
        if size != length or address < 0 or address + size > ROM_SIZE:
            raise ValueError(f"Invalid sample extent for {fields[2]}")
        first = (address + 1) // 2
        bank[first:first + len(data)] = data
    dest = folder / "sound" / "pcm22.bin"
    with tempfile.NamedTemporaryFile(dir=dest.parent, delete=False) as temp:
        try:
            temp.write(MAGIC)
            temp.write(bank)
        except BaseException:
            Path(temp.name).unlink(missing_ok=True)
            raise
    Path(temp.name).replace(dest)
    return dest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    print(prepare(parser.parse_args().assets))
