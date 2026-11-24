"""Make the M10 update ZIP from a 68060 build and its Workbench launchers."""
import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def package(binaries, destination):
    files = Path(__file__).resolve().parent
    with ZipFile(destination, "w", ZIP_DEFLATED) as archive:
        archive.write(binaries / "OutRunners", "M9-Pad4-060")
        for name in ("Pad4Audio", "Pad4Setup"):
            archive.write(binaries / name, name)
        for mode in ("AGA", "RTG"):
            play = f"Play Pad4 {mode}"
            run = f"Run Pad4 {mode}"
            archive.write(files / play, play)
            archive.write(files / run, run)
            archive.write(files / "launcher.info", play + ".info")
        archive.write(files / "Controls.txt", "Controls.txt")
        archive.write(files.parent / "KALMS.txt", "KALMS.txt")
        archive.write(files.parents[2] / "LICENSE", "LICENSE")
        for notice in ("MAME.txt", "ymfm.txt"):
            archive.write(files / "licenses" / notice, "licenses/" + notice)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binaries", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    package(args.binaries, args.destination)
