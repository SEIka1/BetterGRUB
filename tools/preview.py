#!/usr/bin/env python3
"""Run the BetterGRUB demo with a disposable FAT disk and private OVMF variables."""
import argparse
import hashlib
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


def expect_motion(q, capture, size, regions, selection, expected_rgb, timeout=15, ambient=False):
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
        if diff[selection] > 50 and diff[2] == 0:
            if ambient and diff[1 - selection] > 15 and (diff[3] > 12 if selection == 1 else diff[3] == 0):
                return b
            if not ambient and diff[1 - selection] == 0:
                return b
    raise RuntimeError(f"Expected motion in region {selection}, observed {diff}")


def spell_measurements(frame):
    w, h, data = frame

    def pixels(x0, y0, x1, y1):
        for y in range(y0 * h // 1080, y1 * h // 1080, 2):
            row = (y * w + x0 * w // 1920) * 3
            for i in range(row, (y * w + x1 * w // 1920) * 3, 6):
                yield data[i], data[i+1], data[i+2]

    # Count the bright body, including the nearly white center of the blue spell.
    blue = sum(b > 200 and g > 202 and b + 6 >= r for r, g, b in pixels(330, 415, 654, 715))
    shadow = sum(r < 110 and g < 38 and b < 40 for r, g, b in pixels(1434, 521, 1534, 622))
    bridge = list(pixels(800, 505, 1150, 752))
    return blue, shadow, bridge


def surrounding_painting(frame):
    """Fingerprint every pixel outside the two lens surfaces, including the entire stream."""
    w, h, data = frame
    lenses = [(x*w//1920, (x+rw)*w//1920, y*h//1080, (y+rh)*h//1080)
              for x, y, rw, rh in ((248,383,488,360), (1230,413,508,320))]
    result = hashlib.sha256()
    pixels = memoryview(data)
    for row in range(h):
        start = 0
        for x0, x1, y0, y1 in lenses:
            if y0 <= row < y1:
                result.update(pixels[(row*w+start)*3:(row*w+x0)*3])
                start = x1
        result.update(pixels[(row*w+start)*3:(row+1)*w*3])
    return result.digest()


def smoke(q, folder, screenshots=None):
    capture = folder / "screen.ppm"
    frieren = ((280, 420, 420, 270), (1250, 440, 449, 260), (800, 170, 300, 220), (800, 505, 350, 247))
    portrait, space = (132, 99, 94), (7, 12, 21)
    first = expect_motion(q, capture, (1920, 1080), frieren, 0, portrait, timeout=40, ambient=True)
    time.sleep(.7)
    settled = q.shot(screenshots / "frieren-windows.ppm" if screenshots else capture)
    blue_on, hole_idle, clean = spell_measurements(settled)
    q.key("down")
    time.sleep(.7)
    warped = expect_motion(q, capture, (1920, 1080), frieren, 1, portrait, ambient=True)
    if screenshots:
        shutil.copyfile(capture, screenshots / "frieren-arch.ppm")
    blue_idle, hole_on, stream = spell_measurements(warped)
    cyan = sum(g > g0 + 12 and b > b0 + 10 and g-r > g0-r0 + 8
               for (r, g, b), (r0, g0, b0) in zip(stream, clean))
    assert blue_on > blue_idle * 1.25 and hole_on > hole_idle * 1.5, (blue_on, blue_idle, hole_on, hole_idle)
    assert cyan > 12, f"Missing source-colored material between the spells: {cyan}"
    q.key("up")
    time.sleep(.7)
    restored = expect_motion(q, capture, (1920, 1080), frieren, 0, portrait, ambient=True)
    assert surrounding_painting(restored) == surrounding_painting(settled), "A stream left pixels behind after focus moved"
    q.key("end")  # The Themes entry has no lens of its own.
    time.sleep(.7)
    expect_motion(q, capture, (1920, 1080), frieren, 0, portrait, ambient=True)
    q.key("home")

    def save(name):
        if screenshots:
            q.shot(screenshots / (name + ".ppm"))

    def choose(index):
        q.key("t")
        time.sleep(.5)
        for _ in range(index):
            q.key("down")
        q.key("ret")

    def check_art(name, swatches, positions=((84, 8), (75, 23), (88, 92))):
        end = time.monotonic() + 8
        while time.monotonic() < end:
            w, h, pixels = q.shot(capture)
            offsets = [(y*h//100*w + x*w//100)*3 for x, y in positions]
            if all(max(abs(a-b) for a,b in zip(pixels[n:n+3], color)) <= 20
                   for n, color in zip(offsets, swatches)):
                save(name)
                return
            time.sleep(.12)
        if screenshots:
            shutil.copyfile(capture, screenshots / (name + "-unexpected.ppm"))
        observed = [tuple(pixels[n:n+3]) for n in offsets]
        raise RuntimeError(f"Could not recognize {name}, sampled {observed}")

    q.key("t")
    time.sleep(.7)
    check_art("frieren-picker", ((35,31,56), (40,44,89), (19,20,41)))
    q.key("down")
    q.key("ret")
    # This example uses a PNG, a different artboard and different GRUB classes.
    stars = ((125, 194, 300, 205), (760, 210, 318, 215), (525, 203, 160, 188))
    expect_motion(q, capture, (1280, 720), stars, 0, space)
    save("binary-stars")
    q.key("down")
    expect_motion(q, capture, (1280, 720), stars, 1, space)
    q.run("system_reset")
    time.sleep(2)
    expect_motion(q, capture, (1280, 720), stars, 0, space, timeout=40)
    choose(2)
    check_art("drakengard3", ((68,65,76), (101,71,83), (91,81,89)))
    choose(3)
    check_art("nier-automata", ((152,133,126), (54,46,33), (19,19,19)))
    choose(4)
    world = ((30,21,15), (38,27,19), (30,21,15))
    edge = ((84,8), (22,7), (88,92))
    check_art("minecraft", world, edge)
    choose(5)
    check_art("minecraft-title", ((190,202,246), (197,207,240), (75,72,57)), edge)
    q.key("ret")
    check_art("minecraft", world, edge)
    print(f"PASS: {first[0]}x{first[1]}, ambient motion, focus growth (blue light {blue_on/blue_idle:.2f}x, "
          f"shadow {hole_on/hole_idle:.2f}x by area), {cyan} cyan bridge samples, exact restoration, "
          "five themes, both Minegrub screens and persistence after reset.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path, nargs="?",
                        default=Path(__file__).resolve().parents[1] / "build/BetterGRUB-demo.zip")
    parser.add_argument("--smoke", action="store_true", help="exercise the themes headlessly and exit")
    parser.add_argument("--screenshots", type=Path, help="save PPM screenshots from the smoke test here")
    args = parser.parse_args()
    if args.screenshots:
        if not args.smoke:
            parser.error("--screenshots requires --smoke")
        args.screenshots = args.screenshots.resolve()
        args.screenshots.mkdir(parents=True, exist_ok=True)
    code, seed_vars = firmware()
    accelerator = os.environ.get("QEMU_ACCEL", "tcg")
    if accelerator not in ("tcg", "kvm"):
        parser.error("QEMU_ACCEL must be tcg or kvm")
    with tempfile.TemporaryDirectory(prefix="bettergrub-preview-") as tmp:
        folder = Path(tmp)
        disk = build_disk(args.bundle, folder)
        variables = folder / "vars.fd"
        shutil.copyfile(seed_vars, variables)
        sock = folder / "qmp"
        cmd = ["qemu-system-x86_64", "-machine", "q35,accel=" + accelerator, "-m", "384", "-vga", "std",
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
                smoke(monitor, folder, args.screenshots)
                monitor.run("quit")
        finally:
            if proc.poll() is None:
                proc.terminate()
            proc.wait(timeout=7)


if __name__ == "__main__":
    main()
