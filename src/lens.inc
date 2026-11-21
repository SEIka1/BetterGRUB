/* SPDX-License-Identifier: GPL-3.0-or-later
   Selection-driven lenses in the gfxmenu view, before widget compositing. */

struct lens_region
{
  const struct grub_lens_spec *spec;
  grub_video_rect_t bounds;
  grub_uint8_t *original;
  unsigned *x_squared;
  unsigned focus;
};

#define LENS_FLOW_BINS 8

struct lens_flow
{
  struct lens_region *source, *sink;
  grub_video_rect_t bounds;
  grub_uint8_t *original;
  int ux, uy, distance, reach, depth;
  grub_video_rect_t last[LENS_FLOW_BINS];
};

struct grub_lens_animation
{
  struct lens_region regions[GRUB_LENS_MAX];
  struct lens_flow flows[GRUB_LENS_MAX];
  unsigned count, flow_count;
  grub_uint64_t started, last_frame;
};

struct lens_word { const char *start; unsigned length; };

static unsigned
lens_words (const char *value, struct lens_word words[11])
{
  unsigned count = 0;
  while (value && *value)
    {
      const char *start;
      while (*value == ' ' || *value == '\t')
        value++;
      if (!*value)
        break;
      if (count == 11)
        return 0;
      start = value;
      while (*value && *value != ' ' && *value != '\t')
        value++;
      words[count].start = start;
      words[count++].length = value - start;
    }
  return count;
}

static int
lens_word_is (struct lens_word word, const char *literal)
{
  return word.length == grub_strlen (literal)
         && grub_memcmp (word.start, literal, word.length) == 0;
}

static int
lens_number (struct lens_word word, unsigned limit, unsigned *value)
{
  unsigned i, n = 0;
  for (i = 0; i < word.length; i++)
    {
      unsigned digit = (unsigned char) word.start[i] - '0';
      if (digit > 9 || digit > limit || n > (limit - digit) / 10)
        return 0;
      n = n * 10 + digit;
    }
  *value = n;
  return 1;
}

static int
lens_color (struct lens_word word, unsigned char color[3])
{
  unsigned i;
  if (word.length != 7 || word.start[0] != '#')
    return 0;
  for (i = 0; i < 6; i++)
    {
      int c = word.start[i + 1], v;
      if (c >= '0' && c <= '9')
        v = c - '0';
      else if (c >= 'a' && c <= 'f')
        v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        v = c - 'A' + 10;
      else
        return 0;
      color[i / 2] = (i & 1) ? color[i / 2] | v : v << 4;
    }
  return 1;
}

static int
lens_class (struct lens_word word, char name[64])
{
  unsigned i;
  if (!word.length || word.length >= 64)
    return 0;
  for (i = 0; i < word.length; i++)
    {
      char c = word.start[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_'))
        return 0;
    }
  grub_memcpy (name, word.start, word.length);
  name[word.length] = 0;
  return 1;
}

void
grub_gfxmenu_lens_add (grub_gfxmenu_view_t view, const char *value)
{
  struct grub_lens_theme *theme = &view->lens_theme;
  struct grub_lens_spec spec = { .particles = 26, .speed = 100 };
  struct lens_word words[11];
  unsigned i, n = lens_words (value, words);
  if (theme->count >= GRUB_LENS_MAX || n < 7 || !lens_class (words[0], spec.class_name))
    return;
  spec.eclipse = lens_word_is (words[1], "eclipse");
  if ((!spec.eclipse && !lens_word_is (words[1], "vortex"))
      || !lens_number (words[2], IMAGE_HW_MAX_PX, &spec.x)
      || !lens_number (words[3], IMAGE_HW_MAX_PX, &spec.y)
      || !lens_number (words[4], IMAGE_HW_MAX_PX, &spec.width)
      || !lens_number (words[5], IMAGE_HW_MAX_PX, &spec.height)
      || spec.width < 8 || spec.height < 8 || !lens_color (words[6], spec.color)
      || (n >= 8 && !lens_number (words[7], 64, &spec.particles))
      || (n >= 9 && (!lens_number (words[8], 200, &spec.speed) || spec.speed < 25))
      || (n >= 10 && !lens_number (words[9], 100, &spec.ambient))
      || (n == 11 && !lens_class (words[10], spec.source_class)))
    return;
  for (i = 0; i < theme->count; i++)
    if (grub_strcmp (theme->specs[i].class_name, spec.class_name) == 0)
      return;
  theme->specs[theme->count++] = spec;
}

int
grub_gfxmenu_lens_index (grub_gfxmenu_view_t view, grub_menu_entry_t entry)
{
  struct grub_menu_entry_class *c;
  unsigned i;
  if (view->lenses && entry)
    for (c = entry->classes; c; c = c->next)
      if (c->name)
        for (i = 0; i < view->lenses->count; i++)
          if (grub_strcmp (c->name, view->lenses->regions[i].spec->class_name) == 0)
            return (int) i;
  return -1;
}

static void
lens_free (struct grub_lens_animation *animation)
{
  unsigned i;
  if (!animation)
    return;
  for (i = 0; i < animation->count; i++)
    {
      grub_free (animation->regions[i].original);
      grub_free (animation->regions[i].x_squared);
    }
  for (i = 0; i < animation->flow_count; i++)
    grub_free (animation->flows[i].original);
  grub_free (animation);
}

static void lens_prepare_flows (struct grub_lens_animation *animation,
                                const struct grub_video_bitmap *image);

static void
lens_start (grub_gfxmenu_view_t view)
{
  const struct grub_video_bitmap *raw = view->raw_desktop_image;
  const struct grub_video_bitmap *image = view->scaled_desktop_image;
  struct grub_lens_animation *animation;
  grub_err_t saved_error = grub_errno;
  unsigned i, pixels = 0;
  if (!view->lens_theme.count || view->lenses || !raw || !image
      || view->desktop_image_scale_method != GRUB_VIDEO_BITMAP_SELECTION_METHOD_STRETCH
      || (image->mode_info.blit_format != GRUB_VIDEO_BLIT_FORMAT_RGB_888
          && image->mode_info.blit_format != GRUB_VIDEO_BLIT_FORMAT_RGBA_8888))
    return;
  unsigned rw = raw->mode_info.width, rh = raw->mode_info.height;
  unsigned w = image->mode_info.width, h = image->mode_info.height;
  unsigned bytes = image->mode_info.bytes_per_pixel;
  if (!rw || !rh || w > IMAGE_HW_MAX_PX || h > IMAGE_HW_MAX_PX)
    return;

  animation = grub_zalloc (sizeof (*animation));
  if (!animation)
    goto fallback;
  for (i = 0; i < view->lens_theme.count; i++)
    {
      const struct grub_lens_spec *s = &view->lens_theme.specs[i];
      struct lens_region *region;
      grub_video_rect_t r;
      unsigned j, row, col;
      if (s->x >= rw || s->y >= rh || s->width > rw - s->x || s->height > rh - s->y)
        continue;
      r.x = s->x * w / rw;
      r.y = s->y * h / rh;
      r.width = (s->x + s->width) * w / rw - r.x;
      r.height = (s->y + s->height) * h / rh - r.y;
      if (r.width < 8 || r.height < 8 || r.width * r.height > 1048576
          || pixels + r.width * r.height > 2097152)
        continue;
      for (j = 0; j < animation->count; j++)
        {
          const grub_video_rect_t *other = &animation->regions[j].bounds;
          if (r.x < other->x + other->width && other->x < r.x + r.width
              && r.y < other->y + other->height && other->y < r.y + r.height)
            break;
        }
      if (j < animation->count)
        continue;
      region = &animation->regions[animation->count++];
      region->bounds = r;
      region->spec = s;
      /* The packed RGB sampler may read one padding byte after the last triplet. */
      region->original = grub_malloc (r.width * r.height * bytes + 1);
      region->x_squared = grub_malloc (r.width * sizeof (*region->x_squared));
      if (!region->original || !region->x_squared)
        goto fallback;
      region->original[r.width * r.height * bytes] = 0;
      for (col = 0; col < r.width; col++)
        {
          int rx = r.width / 2;
          int nx = ((int) col - rx) * (65536 / rx) / 256;
          region->x_squared[col] = nx * nx;
        }
      for (row = 0; row < r.height; row++)
        grub_memcpy (region->original + row * r.width * bytes,
                     (const grub_uint8_t *) image->data + (r.y + row) * image->mode_info.pitch
                     + r.x * bytes, r.width * bytes);
      pixels += r.width * r.height;
    }
  if (!animation->count)
    goto fallback;
  lens_prepare_flows (animation, image);
  animation->started = animation->last_frame = grub_get_time_ms ();
  view->lenses = animation;
  return;
 fallback:
  lens_free (animation);
  grub_errno = saved_error;
}

/* Parabolic sine: 1024 units per turn and 256 units of amplitude. */
static int
lens_wave (unsigned phase)
{
  int u = phase & 511;
  int amplitude = u * (512 - u) >> 8;
  return phase & 512 ? -amplitude : amplitude;
}

static int
lens_clamp (int value, int upper)
{
  return value < 0 ? 0 : grub_min (value, upper);
}

#include "spell.inc"

static void
lens_sparks (grub_uint8_t *dst, unsigned pitch, unsigned bytes, const struct lens_region *region,
             unsigned elapsed)
{
  const struct grub_lens_spec *s = region->spec;
  int rx = region->bounds.width / 2, ry = region->bounds.height / 2;
  unsigned i;
  for (i = 0; i < s->particles; i++)
    {
      unsigned orbit = i * 359 + elapsed / (7 + i % 8);
      int radius = 133 + (i * 53 % 93);
      int head = 115 + lens_wave (orbit * 4 + i * 19) / 3;
      int step;
      for (step = 0; step < 5; step++)
        {
          unsigned phase = orbit - step * 2;
          int x = rx + lens_wave (phase + 256) * rx * radius / 65536;
          int y = ry + lens_wave (phase) * ry * radius / (s->eclipse ? 125000 : 70000);
          grub_uint8_t *p = dst + y * pitch + x * bytes;
          int opacity = head * (5 - step) * (int) region->focus / (256 * 5);
          unsigned c;
          for (c = 0; c < 3; c++)
            p[c] += ((s->color[c] - p[c]) * opacity) / 256;
        }
    }
}

/* Two 8-bit channels per product. The weights sum to 256, so each accumulated
   channel and its rounding bias fit in a 16-bit lane without a carry. */
static inline unsigned
lens_pair (unsigned a, unsigned b, unsigned c, unsigned d,
           int wa, int wb, int wc, int wd)
{
  const unsigned mask = 0x00ff00ff;
  return (((a & mask) * wa + (b & mask) * wb + (c & mask) * wc + (d & mask) * wd
           + 0x00800080) >> 8) & mask;
}

static void
lens_render (struct grub_video_bitmap *image, const struct lens_region *region, unsigned elapsed)
{
  struct { short pull, bend, light; } field[256];
  const grub_video_rect_t *r = &region->bounds;
  const int w = r->width, h = r->height, rx = w / 2, ry = h / 2, sy = 65536 / ry;
  const unsigned pitch = image->mode_info.pitch, bytes = image->mode_info.bytes_per_pixel;
  const int eclipse = region->spec->eclipse;
  grub_uint8_t *dst = (grub_uint8_t *) image->data + r->y * pitch + r->x * bytes;
  elapsed = (unsigned) ((grub_uint64_t) elapsed * region->spec->speed / 100);
  unsigned rotation = elapsed / (eclipse ? 3 : 2);
  int radius, y;
  int ambient = region->spec->ambient, focus = spell_ease (region->focus);
  for (radius = 0; (ambient || region->focus) && radius < 256; radius++)
    {
      int edge = 256 - radius;
      int core = lens_clamp ((radius - (eclipse ? 3 : 0)) * (eclipse ? 18 : 30), 256);
      int influence = (((int) region->focus * edge * edge) >> 16) * core >> 8;
      int pulse = lens_wave (rotation + 7 * radius);
      int shear = lens_wave (rotation / 2 + 3 * radius);
      field[radius].pull = influence * ((eclipse ? 52 : 33) + pulse / 6) / 256;
      field[radius].bend = influence * ((eclipse ? 92 : 36) + shear / 4) / 256;
      field[radius].light = influence * grub_max (pulse, 0) / (eclipse ? 1370 : 1180);
      if (ambient)
        {
          int plateau = eclipse ? 12 : 44;
          int shoulder = 256 - lens_clamp ((radius - plateau) * 256 / (256 - plateau), 256);
          int envelope = shoulder * shoulder >> 8;
          int inner = lens_clamp ((80 - radius) * 256 / 80, 256);
          /* Idle refraction stays close to the core. Particles still cross the entire surface. */
          envelope = envelope * spell_lerp (inner, 256, focus) >> 8;
          int activity = ambient + ((256 - ambient) * focus >> 8);
          int ring = lens_wave (rotation + 10 * radius);
          int shell = lens_clamp ((radius - (eclipse ? 11 : 0)) * (eclipse ? 6 : 10), 256);
          int twist = eclipse ? 56 + shear / 4 : shear / 3 + ring / 6;
          field[radius].pull = envelope * (focus * (eclipse ? 118 : 161) / 256
                                 + activity * (12 + (ring * shell / 256) / (eclipse ? 10 : 7)) / 256) / 256;
          field[radius].bend = (((envelope * shell >> 8) * activity) >> 8) * twist / 256;
          field[radius].light = ((envelope * activity >> 8) * (33 + pulse / 8)) >> 8;
        }
    }
  for (y = 0; y < h; y++)
    {
      int x, dy = y - ry, ny = dy * sy / 256, ny2 = ny * ny;
      int ray = eclipse ? lens_clamp (((dy < 0 ? -dy : dy) * 160 / ry - 4) * 15, 256) : 256;
      grub_uint8_t *out = dst + y * pitch;
      if (ambient)
        grub_memcpy (out, region->original + y * w * bytes, w * bytes);
      else if (!region->focus)
        {
          grub_memcpy (out, region->original + y * w * bytes, w * bytes);
          continue;
        }
      for (x = 0; x < w; x++, out += bytes)
        {
          int dx = x - rx, pull, bend, lx, ly, fx, fy, light;
          const grub_uint8_t *top, *bottom;
          unsigned a, b, c, d, rb, ga;
          int wa, wb, wc, wd;
          radius = (region->x_squared[x] + ny2) >> 8;
          if (radius >= 256)
            continue;
          pull = field[radius].pull;
          bend = field[radius].bend;
          light = field[radius].light;
          if (ambient && !(pull | bend | light))
            continue;
          lx = lens_clamp (x * 16 - (dx * pull + dy * bend) / 16, (w - 1) * 16 - 1);
          if (ambient)
            {
              int shift = dx * bend * ray / 256;
              if (eclipse)
                {
                  /* Light on each side of the horizon stays on that side of the beam. */
                  int limit = grub_abs (dy) * (256 - pull) / 2;
                  shift = grub_max (-limit, grub_min (shift, limit));
                }
              ly = lens_clamp (y * 16 - (dy * pull - shift) / 16, (h - 1) * 16 - 1);
            }
          else
            ly = lens_clamp (y * 16 - ((dy * pull - dx * bend) / 16) * ray / 256, (h - 1) * 16 - 1);
          fx = lx & 15;
          fy = ly & 15;
          top = region->original + ((ly >> 4) * w + (lx >> 4)) * bytes;
          bottom = top + w * bytes;
          a = grub_le_to_cpu32 (grub_get_unaligned32 (top));
          b = grub_le_to_cpu32 (grub_get_unaligned32 (top + bytes));
          c = grub_le_to_cpu32 (grub_get_unaligned32 (bottom));
          d = grub_le_to_cpu32 (grub_get_unaligned32 (bottom + bytes));
          wa = (16 - fx) * (16 - fy);
          wb = fx * (16 - fy);
          wc = (16 - fx) * fy;
          wd = fx * fy;
          rb = lens_pair (a, b, c, d, wa, wb, wc, wd);
          ga = lens_pair (a >> 8, b >> 8, c >> 8, d >> 8, wa, wb, wc, wd);
          out[0] = grub_min (((rb & 255) * (256 + light)) >> 8, 255U);
          out[1] = grub_min (((ga & 255) * (256 + light)) >> 8, 255U);
          out[2] = grub_min (((rb >> 16) * (256 + light)) >> 8, 255U);
          if (bytes == 4)
            out[3] = ga >> 16;
        }
    }
  if (ambient)
    spell_emission (image, region, elapsed, focus);
  else if (region->focus)
    lens_sparks (dst, pitch, bytes, region, elapsed);
}

void
grub_gfxmenu_lens_idle (void *data)
{
  grub_gfxmenu_view_t view = data;
  struct grub_lens_animation *a = view->lenses;
  grub_uint64_t now = grub_get_time_ms (), delta = now - a->last_frame;
  grub_video_rect_t dirty[(1 + LENS_FLOW_BINS) * GRUB_LENS_MAX];
  unsigned step, count = 0, region_count, i, pass;
  int selected;
  if (delta < 33)
    return;
  a->last_frame = now;
  selected = grub_gfxmenu_lens_index (view, grub_menu_get_entry (view->menu, view->selected));
  step = grub_min (delta, (grub_uint64_t) 256);
  /* Restore last frame's streams before either endpoint is redrawn. */
  for (i = 0; i < a->flow_count; i++)
    spell_restore_flow (view->scaled_desktop_image, &a->flows[i]);
  for (i = 0; i < a->count; i++)
    {
      struct lens_region *r = &a->regions[i];
      unsigned fade = r->spec->ambient ? (step + 1) / 2 : step;
      if (selected == (int) i)
        r->focus = grub_min (r->focus + fade, 256U);
      else if (r->focus)
        r->focus -= grub_min (r->focus, fade);
      else if (!r->spec->ambient)
        continue;
      lens_render (view->scaled_desktop_image, r, (unsigned) (now - a->started));
      dirty[count++] = r->bounds;
    }
  region_count = count;
  for (i = 0; i < a->flow_count; i++)
    {
      struct lens_flow *flow = &a->flows[i];
      grub_video_rect_t paint[LENS_FLOW_BINS] = {{0}};
      if (flow->sink->focus)
        spell_draw_flow (view->scaled_desktop_image, flow, (unsigned) (now - a->started), paint);
      count += spell_dirty_flow (flow, paint, dirty, region_count, dirty + count);
    }
  if (!count)
    return;
  for (pass = 0; pass <= (unsigned) view->double_repaint; pass++)
    {
      for (i = 0; i < count; i++)
        {
          grub_video_set_area_status (GRUB_VIDEO_AREA_ENABLED);
          grub_gfxmenu_view_redraw (view, &dirty[i]);
        }
      grub_video_set_area_status (GRUB_VIDEO_AREA_DISABLED);
      if (pass == 0)
        grub_video_present_rects (dirty, count);
    }
}
