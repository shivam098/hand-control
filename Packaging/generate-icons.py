"""Regenerate the checked-in native icons from the SVG on macOS."""
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

ICONS = Path(__file__).resolve().parent / "Icons"
MAC = (16, 32, 128, 256, 512)
WINDOWS = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def main():
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        iconset = work / "HandControl.iconset"
        iconset.mkdir()
        master = work / "1024.png"
        subprocess.run(["sips", "-s", "format", "png", ICONS / "HandControl.svg", "--out", master],
                       check=True, stdout=subprocess.DEVNULL)
        for size in sorted(set(MAC) | {2 * n for n in MAC} | set(WINDOWS)):
            if size != 1024:
                subprocess.run(["sips", "-z", str(size), str(size), master, "--out", work / f"{size}.png"],
                               check=True, stdout=subprocess.DEVNULL)
        for size in MAC:
            for scale, suffix in ((1, ""), (2, "@2x")):
                shutil.copyfile(work / f"{size * scale}.png", iconset / f"icon_{size}x{size}{suffix}.png")
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", ICONS / "HandControl.icns"], check=True)

        # Windows 10/11 accept PNG frames. ICO stores a dimension of 256 as zero.
        frames = [(work / f"{size}.png").read_bytes() for size in WINDOWS]
        directory = bytearray(struct.pack("<HHH", 0, 1, len(frames)))
        offset = 6 + 16 * len(frames)
        for size, data in zip(WINDOWS, frames):
            directory.extend(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
            offset += len(data)
        (ICONS / "HandControl.ico").write_bytes(directory + b"".join(frames))


if __name__ == "__main__":
    main()
