#!/usr/bin/env python3
"""Run the BetterGRUB demo with a disposable FAT disk and private OVMF variables."""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
from zipfile import ZipFile


def firmware():
    code, variables = os.environ.get("OVMF_CODE"), os.environ.get("OVMF_VARS")
    if code and variables:
        return Path(code), Path(variables)
    for directory in (Path("/usr/share/OVMF"), Path("/usr/share/edk2/x64"), Path("/usr/share/edk2/ovmf")):
        for suffix in ("_4M", ""):
            code = directory / f"OVMF_CODE{suffix}.fd"
            variables = directory / f"OVMF_VARS{suffix}.fd"
            if code.is_file() and variables.is_file():
                return code, variables
    raise SystemExit("Set OVMF_CODE and OVMF_VARS to a matching x86-64 OVMF firmware pair.")


def build_disk(bundle, folder):
    tree = folder / "files"
    with ZipFile(bundle) as pkg:
        for name in pkg.namelist():
            parts = name.split("/")
            if parts[:2] != ["EFI", "BetterGRUB"] or any(p in ("", ".", "..") for p in parts):
                raise ValueError(f"Unexpected archive member {name!r}")
            destination = tree.joinpath(*parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            with pkg.open(name) as source, destination.open("xb") as target:
                shutil.copyfileobj(source, target)
    fallback = tree / "EFI/BOOT"
    fallback.mkdir()
    shutil.copyfile(tree / "EFI/BetterGRUB/BetterGRUB.efi", fallback / "BOOTX64.EFI")
    fs = folder / "esp.fat"
    subprocess.run(["mkfs.fat", "-C", "-F", "32", str(fs), "65536"], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["mcopy", "-i", str(fs), "-s", str(tree / "EFI"), "::"], check=True)
    disk = folder / "preview.img"
    with disk.open("wb") as output:
        output.truncate(66 * 1024 * 1024)
    subprocess.run(["sfdisk", str(disk)], check=True, stdout=subprocess.DEVNULL,
                   input=b"label: gpt\n\nstart=2048,size=131072,type=c12a7328-f81f-11d2-ba4b-00a0c93ec93b\n")
    with disk.open("r+b") as output, fs.open("rb") as source:
        output.seek(1024 * 1024)
        shutil.copyfileobj(source, output)
    return disk


class Monitor:
    def __init__(self, connection):
        self.connection = connection
        self.input = connection.makefile("rb")
        if "QMP" not in json.loads(self.input.readline()):
            raise RuntimeError("Missing QMP greeting")
        self.run("qmp_capabilities")

    def run(self, command, **arguments):
        self.connection.sendall(json.dumps({"execute": command, "arguments": arguments}).encode() + b"\n")
        for line in self.input:
            reply = json.loads(line)
            if "error" in reply:
                raise RuntimeError(reply["error"])
            if "return" in reply:
                return reply["return"]
        raise EOFError(command)

    def key(self, key):
        self.run("send-key", keys=[{"type": "qcode", "data": key}], **{"hold-time": 60})
        time.sleep(.25)

    def shot(self, path):
        self.run("screendump", filename=str(path))
        with path.open("rb") as ppm:
            assert ppm.readline() == b"P6\n"
            width, height = map(int, ppm.readline().split())
            assert ppm.readline() == b"255\n"
            return width, height, ppm.read()


def samples(frame, rect, source_size):
    w, h, data = frame
    x, y, width, height = rect
    left, right = x * w // source_size[0], (x + width) * w // source_size[0]
    top, bottom = y * h // source_size[1], (y + height) * h // source_size[1]
    return b"".join(data[(row*w+left)*3:(row*w+right)*3:12] for row in range(top, bottom, 4))


def expect_motion(q, capture, size, regions, selection, expected_rgb, timeout=15):
    until = time.monotonic() + timeout
    diff = []
    while time.monotonic() < until:
        a = q.shot(capture)
        time.sleep(.20)
        b = q.shot(capture)
        w, h = b[:2]
        mid = (h // 2 * w + w // 2) * 3
        if a[:2] != b[:2] or any(abs(actual - wanted) > 36 for actual, wanted in
                                  zip(b[2][mid:mid+3], expected_rgb)):
            continue
        diff = [sum(abs(p - n) > 3 for p, n in zip(samples(a, r, size), samples(b, r, size))) for r in regions]
        if diff[selection] > 50 and diff[1 - selection] == diff[2] == 0:
            return a[:2]
    raise RuntimeError(f"Expected motion in region {selection}, observed {diff}")


def smoke(q, folder):
    capture = folder / "screen.ppm"
    frieren = ((280, 420, 420, 270), (1250, 440, 449, 260), (800, 330, 300, 320))
    portrait, space = (132, 99, 94), (7, 12, 21)
    size = expect_motion(q, capture, (1920, 1080), frieren, 0, portrait, timeout=40)
    q.key("down")
    expect_motion(q, capture, (1920, 1080), frieren, 1, portrait)
    q.key("t")
    time.sleep(.7)
    q.key("down")
    q.key("ret")
    # This example uses a PNG, a different artboard and different GRUB classes.
    stars = ((125, 194, 300, 205), (760, 210, 318, 215), (525, 203, 160, 188))
    expect_motion(q, capture, (1280, 720), stars, 0, space)
    q.key("down")
    expect_motion(q, capture, (1280, 720), stars, 1, space)
    q.run("system_reset")
    time.sleep(2)
    expect_motion(q, capture, (1280, 720), stars, 0, space, timeout=40)
    print(f"PASS: {size[0]}x{size[1]}, both focus targets in both themes, choice persisted after reset.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path, nargs="?",
                        default=Path(__file__).resolve().parents[1] / "build/BetterGRUB-demo.zip")
    parser.add_argument("--smoke", action="store_true", help="exercise both themes headlessly and exit")
    args = parser.parse_args()
    code, seed_vars = firmware()
    with tempfile.TemporaryDirectory(prefix="bettergrub-preview-") as tmp:
        folder = Path(tmp)
        disk = build_disk(args.bundle, folder)
        variables = folder / "vars.fd"
        shutil.copyfile(seed_vars, variables)
        sock = folder / "qmp"
        cmd = ["qemu-system-x86_64", "-machine", "q35,accel=tcg", "-m", "384", "-vga", "std",
               "-nic", "none", "-serial", "none", "-parallel", "none",
               "-drive", f"if=pflash,unit=0,format=raw,readonly=on,file={code}",
               "-drive", f"if=pflash,unit=1,format=raw,file={variables}",
               "-drive", f"format=raw,cache=writethrough,file={disk}"]
        if os.environ.get("QEMU_FIRMWARE_DIR"):
            cmd.extend(["-L", os.environ["QEMU_FIRMWARE_DIR"]])
        if not args.smoke:
            subprocess.run(cmd, check=True)
            return
        cmd.extend(["-display", "none", "-qmp", f"unix:{sock},server=on,wait=off"])
        proc = subprocess.Popen(cmd)
        try:
            until = time.monotonic() + 10
            while not sock.exists() and proc.poll() is None and time.monotonic() < until:
                time.sleep(.04)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(10)
                connection.connect(str(sock))
                monitor = Monitor(connection)
                smoke(monitor, folder)
                monitor.run("quit")
        finally:
            if proc.poll() is None:
                proc.terminate()
            proc.wait(timeout=7)


if __name__ == "__main__":
    main()
