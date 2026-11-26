"""Build the portable Windows x64 app with Zig 0.13 and the official MediaPipe C runtime."""
import hashlib
import json
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / ".build/windows-deps"
BUILD = ROOT / ".build/windows-package"
DEST = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.home() / "Downloads/Hand Control"
MP = "0.10.32"
WHEEL = f"mediapipe-{MP}-py3-none-win_amd64.whl"
MODEL = "https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task"
HEADERS = [
    "components/containers/category.h", "components/containers/landmark.h", "core/base_options.h",
    "core/common.h", "core/mp_status.h", "vision/core/image.h", "vision/hand_landmarker/hand_landmarker.h",
    "vision/hand_landmarker/hand_landmarker_result.h",
]


def fetch(url, path, digest=None):
    if not path.exists():
        with urllib.request.urlopen(url, timeout=90) as response:
            data = response.read()
        if digest and hashlib.sha256(data).hexdigest() != digest:
            raise RuntimeError(f"Download checksum failed: {path.name}")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    if digest and hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise RuntimeError(f"Cached checksum failed: {path}")


def main():
    DEPS.mkdir(parents=True, exist_ok=True)
    BUILD.mkdir(parents=True, exist_ok=True)
    DEST.mkdir(parents=True, exist_ok=True)
    zig = [shutil.which("zig")] if shutil.which("zig") else [sys.executable, "-m", "ziglang"]
    zig_env = json.loads(subprocess.check_output([*zig, "env"], cwd=ROOT, text=True))
    zig_lib = ROOT / zig_env["lib_dir"]
    wheel = DEPS / WHEEL
    url = ""
    if not wheel.exists():
        with urllib.request.urlopen(f"https://pypi.org/pypi/mediapipe/{MP}/json", timeout=30) as resp:
            url = next(f["url"] for f in json.load(resp)["urls"] if f["filename"] == WHEEL)
    fetch(url, wheel, "438d98977a4e1b0f88070ad16537a328912f847ce9debf236da047b0b483cbfd")
    fetch(MODEL, DEPS / "hand_landmarker.task", "fbc2a30080c3c557093b5ddfc334698132eb341044ccee322ccf8bcf3607cde1")
    header_hash = hashlib.sha256()
    for suffix in HEADERS:
        name = f"mediapipe/tasks/c/{suffix}"
        path = DEPS / "headers" / name
        fetch(f"https://raw.githubusercontent.com/google-ai-edge/mediapipe/v{MP}/{name}", path)
        header_hash.update(name.encode() + b"\0" + path.read_bytes())
    if header_hash.hexdigest() != "90453684c6980666373aafdb4d7d396f7197d9b04b6e7cafdc9555278c2df8e5":
        raise RuntimeError("The MediaPipe headers do not match the pinned ABI.")

    win = ROOT / "Windows"
    core = ROOT / "Sources/GestureCore"
    target = ["-target", "x86_64-windows-gnu"]
    def run(*args):
        subprocess.run([*zig, *args], cwd=ROOT, check=True)

    run("dlltool", "-m", "i386:x86-64", "-d", win/"mediapipe.def", "-l", BUILD/"libmediapipe.a")
    run("cc", *target, "-O2", "-std=c11", "-I", core/"include", "-c", core/"GestureCore.c", "-o", BUILD/"core.o")
    exe = BUILD / "Hand Control.exe"
    run("c++", *target, "-std=c++17", "-O2", "-s", "-static", "-Wl,--subsystem,windows", "-municode",
        "-DUNICODE", "-D_UNICODE", "-DNOMINMAX", "-DMP_EXPORT=", "-I", core/"include", "-I", DEPS/"headers",
        win/"Main.cpp", win/"Camera.cpp", win/"Tracker.cpp", BUILD/"core.o", BUILD/"libmediapipe.a",
        "-lmfreadwrite", "-lmf", "-lmfplat", "-lmfuuid", "-lole32", "-luuid", "-luser32", "-lgdi32",
        "-ladvapi32", "-lshell32", "-o", exe)
    output = DEST / "Hand Control - Windows x64.zip"
    folder = "Hand Control/"
    with zipfile.ZipFile(wheel) as upstream, zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        archive.write(exe, folder + exe.name)
        archive.writestr(folder + "libmediapipe.dll", upstream.read("mediapipe/tasks/c/libmediapipe.dll"))
        archive.writestr(folder + "MediaPipe-LICENSE.txt", upstream.read(f"mediapipe-{MP}.dist-info/licenses/LICENSE"))
        archive.write(DEPS/"hand_landmarker.task", folder + "hand_landmarker.task")
        archive.write(win/"NOTICE.txt", folder + "NOTICE.txt")
        for component, path in [("Zig", zig_lib.parent/"LICENSE"), ("mingw-w64", zig_lib/"libc/mingw/COPYING"),
                                ("libcxx", zig_lib/"libcxx/LICENSE.TXT"), ("libcxxabi", zig_lib/"libcxxabi/LICENSE.TXT"),
                                ("libunwind", zig_lib/"libunwind/LICENSE.TXT")]:
            archive.write(path, folder + f"licenses/{component}.txt")
        archive.write(ROOT/"Getting Started.txt", folder + "Getting Started.txt")
    print(output)


if __name__ == "__main__":
    main()
