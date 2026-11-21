# BetterGRUB

BetterGRUB lets a GRUB theme animate part of its background when a boot entry is selected. In the Frieren theme, selecting Windows bends the blue spell above her right hand. Selecting Arch Linux pulls light around the dark sphere above her left. The selection fades between the two while the rest of the painting stays still.

![Frieren in an actual GRUB menu, switching from the blue spell on the left to the eclipse on the right and back](docs/frieren.gif)

The recording is from QEMU/OVMF. The renderer targets 30 frames per second and runs inside GNU GRUB 2.14 on x86-64 UEFI. Both the illustrated [Frieren theme](examples/frieren) and the procedural [Binary Stars theme](examples/binary-stars) are included.

## Try the demo

Download **BetterGRUB-demo.zip** from the [first preview release](https://github.com/SEIka1/BetterGRUB/releases/tag/v0.1.0). On Linux, run these commands with the path to the downloaded ZIP.

```sh
git clone https://github.com/SEIka1/BetterGRUB.git
cd BetterGRUB
python3 tools/preview.py /path/to/BetterGRUB-demo.zip
```

The preview opens a disposable VM. Use **Up** and **Down** to change focus. Press **T** to choose a theme. The Windows and Arch Linux entries demonstrate selection and return to the menu when activated. A reset of the VM keeps the selected theme for the rest of that session.

The preview uses QEMU, OVMF, dosfstools, mtools and `sfdisk`. On Debian or Ubuntu they are available in `qemu-system-x86 ovmf dosfstools mtools fdisk`. For other firmware layouts, set `OVMF_CODE` and `OVMF_VARS` to a matching pair of firmware files.

## Make a theme

Add a `desktop-lens` line for each animated region in your `theme.txt`. Coordinates are pixels in the original image, measured from its top-left corner.

```text
desktop-image: "scene.jpg"
desktop-image-scale-method: "stretch"
desktop-lens: "windows vortex 248 383 488 360 #82e0ff 26 100"
desktop-lens: "arch eclipse 1230 413 508 320 #ffa93e 26 100"
```

GRUB matches the first word to a `--class` on a menu entry. Here, `--class windows` activates the vortex. Each line sets the position, dimensions, particle color, particle count and motion speed. You can use your own class names and artwork. [Theme authoring guide](docs/theme-authoring.md)

## Build from source

Use an x86-64 Linux host with a C toolchain, GNU Make, binutils, Bison, Flex, GNU awk, gettext, texinfo, liblzma development headers, pkg-config, patch, xz, curl and Python 3.

```sh
sh build.sh
python3 tools/preview.py --smoke build/BetterGRUB-demo.zip
```

The build downloads and checks the SHA-256 of the GNU release, applies the compositor patch and packages a matching EFI image and module directory. `GRUB_TARBALL` can point to a local copy of the release. `JOBS` controls build parallelism. The smoke test checks the focus animation on both targets in both themes and confirms that the selected theme survives a firmware reset. [Upstream GNU GRUB releases](https://ftp.gnu.org/gnu/grub/)

## Try it from an existing GRUB

Extract the complete `EFI/BetterGRUB` directory onto your EFI System Partition. At the command prompt of an existing UEFI GRUB, enter the following commands. The firmware's **Boot from EFI file** option can also open `EFI/BetterGRUB/BetterGRUB.efi` directly.

```grub
search --file --set=root /EFI/BetterGRUB/BetterGRUB.efi
chainloader /EFI/BetterGRUB/BetterGRUB.efi
boot
```

The preview EFI is unsigned. Secure Boot requires an EFI image accepted by your firmware's trust policy. To make real OS entries, adapt the demo's `EFI/BetterGRUB/grub/grub.cfg` using the kernel, initrd and chainloader commands from your working configuration. Keep the matching BetterGRUB modules in that same installation.

## License

The code is available under [GPL-3.0-or-later](LICENSE). The Frieren example features unofficial fan artwork. The Binary Stars source artwork and the DejaVu font notices are described in [Art and fonts](ARTWORK.md).
