"""Render the extracted radio music and pack 22,050 Hz stereo IMA ADPCM blocks."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

COMMANDS = (*range(1, 19), *range(0xe0, 0xf5))
BLOCK = 1024
FRAMES = 1017


def encoded_blocks(wav, samples):
    with wav.open("rb") as source:
        riff, _, wave = struct.unpack("<4sI4s", source.read(12))
        if (riff, wave) != (b"RIFF", b"WAVE"):
            raise ValueError("Expected RIFF WAVE")
        valid = False
        while header := source.read(8):
            name, size = struct.unpack("<4sI", header)
            data = source.read(size)
            if len(data) != size:
                raise ValueError("Incomplete WAV chunk")
            if size & 1:
                source.read(1)
            if name == b"fmt ":
                codec, channels, rate, _, block, bits, _, frames = struct.unpack_from("<HHIIHHHH", data)
                valid = (codec, channels, rate, block, bits, frames) == (17, 2, 22050, BLOCK, 4, FRAMES)
            if name == b"data" and valid:
                length = ((samples + FRAMES - 1) // FRAMES) * BLOCK
                if len(data) < length:
                    raise ValueError("Incomplete ADPCM data")
                return data[:length]
    raise ValueError("Missing stereo IMA ADPCM data")


def prepare(assets, renderer):
    destination = assets / "sound" / "music.ima"
    with tempfile.TemporaryDirectory(dir=destination.parent) as folder:
        work = Path(folder)
        entries = []
        with (work / "bank").open("wb") as bank:
            bank.write(b"ORMUSIC\x01" + bytes(len(COMMANDS) * 12))
            for command in COMMANDS:
                if command == 17:  # Another command for Splash Wave
                    entries.append(entries[2])
                    continue
                raw, wav = work / "track.s16le", work / "track.wav"
                result = subprocess.check_output([str(renderer), str(assets), str(command), str(raw)], text=True)
                samples, loop = map(int, result.strip().splitlines()[-1].split())
                if raw.stat().st_size != 4 * samples or not -1 <= loop < samples:
                    raise ValueError(f"Invalid length or loop for command {command}")
                subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", "22050",
                                "-ac", "2", "-i", str(raw), "-c:a", "adpcm_ima_wav", "-block_size",
                                str(BLOCK), str(wav)], check=True)
                entries.append((bank.tell(), samples, loop & 0xffffffff))
                bank.write(encoded_blocks(wav, samples))
                print(f"Command {command:02X}   {samples / 22050:.1f} seconds", flush=True)
            bank.seek(8)
            for entry in entries:
                bank.write(struct.pack(">III", *entry))
        (work / "bank").replace(destination)
    return destination


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    parser.add_argument("renderer", type=Path)
    args = parser.parse_args()
    print(prepare(args.assets.resolve(), args.renderer.resolve()))
