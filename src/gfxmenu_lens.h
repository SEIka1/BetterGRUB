/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GRUB_GFXMENU_LENS_HEADER
#define GRUB_GFXMENU_LENS_HEADER 1

#define GRUB_LENS_MAX 8

struct grub_lens_spec
{
  char class_name[64];
  unsigned x, y, width, height;
  unsigned eclipse, particles, speed;
  unsigned char color[3];
};

struct grub_lens_theme
{
  unsigned count;
  int replace_icons;
  struct grub_lens_spec specs[GRUB_LENS_MAX];
};

#endif
