#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output=${1:-"$project/build"}
mkdir -p "$output"
output=$(CDPATH= cd -- "$output" && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/bettergrub.XXXXXXXX")
archive=$(mktemp "$output/.bettergrub-zip.XXXXXXXX")
trap 'rm -rf "$scratch"; rm -f "$archive"' EXIT
trap 'exit 1' HUP INT TERM
version=2.14
sum=bc8d3c73535b8838d8c8e2654d73edc4e6ae8c8acdb45d5df5dc9a1547446d43
if [ -n "${GRUB_TARBALL:-}" ]; then
    cp "$GRUB_TARBALL" "$scratch/upstream.tar.xz"
else
    curl --fail --location --retry 3 "https://ftp.gnu.org/gnu/grub/grub-$version.tar.xz" -o "$scratch/upstream.tar.xz"
fi
(cd "$scratch" && printf '%s  upstream.tar.xz\n' "$sum" | sha256sum -c -)
tar -xf "$scratch/upstream.tar.xz" -C "$scratch"
src="$scratch/grub-$version"
patch -d "$src" -p1 < "$project/patches/grub-2.14.patch"
cp "$project/src/gfxmenu_lens.h" "$src/include/grub/gfxmenu_lens.h"
cp "$project/src/lens.inc" "$src/grub-core/gfxmenu/lens.inc"
cp "$project/src/spell.inc" "$src/grub-core/gfxmenu/spell.inc"
mkdir "$scratch/obj"
cd "$scratch/obj"
"$src/configure" --prefix=/usr --target=x86_64 --with-platform=efi \
    --disable-nls --disable-grub-mkfont --disable-grub-mount --disable-device-mapper --disable-werror
make -j"${JOBS:-4}"
make -C grub-core install DESTDIR="$scratch/install"
modules="$scratch/install/usr/lib/grub/x86_64-efi"
profile="$scratch/esp/EFI/BetterGRUB"
grub="$profile/grub"
mkdir -p "$grub/x86_64-efi" "$grub/themes" "$grub/fonts"
./grub-mkimage -O x86_64-efi -d "$modules" -p /EFI/BetterGRUB/grub \
    -o "$profile/BetterGRUB.efi" part_gpt part_msdos fat
cp "$modules"/*.mod "$modules"/*.lst "$grub/x86_64-efi/"
for name in frieren binary-stars drakengard3 nier-automata minecraft; do
    cp -R "$project/examples/$name" "$grub/themes/"
done
cp "$project/examples/fonts/"* "$grub/fonts/"
cp "$project/examples/grub.cfg" "$grub/grub.cfg"
./grub-script-check "$grub/grub.cfg"
./grub-editenv "$grub/demo.env" create
./grub-editenv "$grub/demo.env" set bettergrub_theme=frieren
cp "$project/LICENSE" "$profile/COPYING.txt"
cp "$project/ARTWORK.md" "$profile/ARTWORK.md"
python3 "$project/tools/pack.py" "$scratch/esp" "$archive"
mv -f "$archive" "$output/BetterGRUB-demo.zip"
printf 'Built %s/BetterGRUB-demo.zip\n' "$output"
