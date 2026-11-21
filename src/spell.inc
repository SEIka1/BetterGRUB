/* SPDX-License-Identifier: GPL-3.0-or-later
   Ambient spell emission and curved transfers between two theme-defined lenses.
   All geometry is evaluated in fixed point. The menu owns the drawing surface. */

struct spell_point { int x, y; };

struct spell_surface
{
  grub_uint8_t *data;
  int width, height;
  unsigned pitch, bytes;
};

static struct spell_surface
spell_surface (const struct grub_video_bitmap *image, const grub_video_rect_t *r)
{
  unsigned pitch = image->mode_info.pitch, bytes = image->mode_info.bytes_per_pixel;
  struct spell_surface surface = {
    (grub_uint8_t *) image->data + r->y * pitch + r->x * bytes,
    r->width, r->height, pitch, bytes
  };
  return surface;
}

static int
spell_ease (int t)
{
  return t * t * (768 - 2 * t) >> 16;
}

static int
spell_lerp (int a, int b, int t)
{
  return a + ((b - a) * t >> 8);
}

static unsigned
spell_seed (unsigned i)
{
  unsigned v = (i + 1) * 0x9e3779b9U;
  v ^= v >> 16;
  v *= 0x21f0aaadU;
  return v ^ (v >> 15);
}

static void
spell_pixel (const struct spell_surface *s, int x, int y, const grub_uint8_t color[3], int alpha)
{
  grub_uint8_t *p;
  unsigned c;
  if ((unsigned) x >= (unsigned) s->width || (unsigned) y >= (unsigned) s->height)
    return;
  p = s->data + y * s->pitch + x * s->bytes;
  for (c = 0; c < 3; c++)
    p[c] += ((color[c] - p[c]) * alpha) >> 8;
}

/* A subpixel core between two scanlines, with a fainter fringe for bright strokes. */
static void
spell_line (const struct spell_surface *s, struct spell_point a, struct spell_point b,
            const grub_uint8_t color[3], int alpha)
{
  int dx = b.x - a.x, dy = b.y - a.y;
  int horizontal = grub_abs (dx) >= grub_abs (dy);
  int major = horizontal ? dx : dy, minor = horizontal ? dy : dx;
  int length = grub_abs (major), sign = major < 0 ? -1 : 1;
  int u = horizontal ? a.x : a.y, v = (horizontal ? a.y : a.x) * 256;
  int increment = length ? minor * 256 / length : 0;
  int i, fringe = alpha > 65 ? alpha / 5 : 0;
  for (i = 0; i <= length; i++, u += sign, v += increment)
    {
      int row = v >> 8, fraction = v & 255;
      int low = alpha * (256 - fraction) >> 8, high = alpha - low;
      if (horizontal)
        {
          spell_pixel (s, u, row, color, low);
          spell_pixel (s, u, row + 1, color, high);
          if (fringe)
            {
              spell_pixel (s, u, row - 1, color, fringe * (256 - fraction) >> 8);
              spell_pixel (s, u, row + 2, color, fringe * fraction >> 8);
            }
        }
      else
        {
          spell_pixel (s, row, u, color, low);
          spell_pixel (s, row + 1, u, color, high);
          if (fringe)
            {
              spell_pixel (s, row - 1, u, color, fringe * (256 - fraction) >> 8);
              spell_pixel (s, row + 2, u, color, fringe * fraction >> 8);
            }
        }
    }
}

static struct spell_point
spell_polar (const struct spell_surface *s, int radius, int phase, int flatten)
{
  int rx = s->width / 2, ry = s->height / 2;
  int x = (radius * lens_wave (phase + 256)) >> 8;
  int y = (radius * lens_wave (phase)) >> 8;
  struct spell_point p = { rx + x * rx / 256, ry + (y * flatten / 256) * ry / 256 };
  return p;
}

/* A colored wake with a saturated outer glow and a narrow incandescent center.
   Offsets are measured across the stroke, so its width tracks the display mode. */
static void
spell_ribbon (const struct spell_surface *s, struct spell_point a, struct spell_point b,
              const grub_uint8_t hue[3], const grub_uint8_t core[3], int alpha, int width)
{
  int dx = b.x - a.x, dy = b.y - a.y;
  int horizontal = grub_abs (dx) >= grub_abs (dy);
  int major = horizontal ? dx : dy, minor = horizontal ? dy : dx;
  int length = grub_abs (major), sign = major < 0 ? -1 : 1;
  int u = horizontal ? a.x : a.y, v = (horizontal ? a.y : a.x) * 256;
  int increment = length ? minor * 256 / length : 0;
  int i, j, half = width * 256;
  for (i = 0; i <= length; i++, u += sign, v += increment)
    for (j = -width; j <= width + 1; j++)
      {
        int offset = grub_abs (j * 256 - (v & 255));
        int row = (v >> 8) + j, x = horizontal ? u : row, y = horizontal ? row : u;
        if (offset < half)
          {
            int weight = 256 - offset / width;
            spell_pixel (s, x, y, hue, ((weight * weight >> 8) * alpha) >> 8);
            if (offset < 370)
              spell_pixel (s, x, y, core, (370 - offset) * alpha / 370);
          }
      }
}

static struct spell_point
spell_mote (const struct spell_surface *s, unsigned seed, int age, int focus, int eclipse)
{
  int angle = seed & 1023;
  int drift = 50 + (int) ((seed >> 16) & 127);
  int launch = (eclipse ? 52 : 46) + focus / 9;
  int radius = launch + age * (219 - launch + (int) ((seed >> 20) & 23)) / 1024;
  int phase = angle + (eclipse ? -1 : 1) * ((drift * age) >> 10)
              + lens_wave ((seed >> 7) + age * 2) * age / 4608;
  return spell_polar (s, radius, phase, eclipse ? 164 + ((seed >> 12) & 63) : 245);
}

static void
spell_emission (const struct grub_video_bitmap *image, const struct lens_region *r,
                unsigned elapsed, int focus)
{
  const struct grub_lens_spec *spec = r->spec;
  struct spell_surface s = spell_surface (image, &r->bounds);
  int i, j, eclipse = spec->eclipse;
  int intensity = spec->ambient + ((256 - (int) spec->ambient) * focus >> 8);
  int tracks = spec->particles ? (eclipse ? 5 : 7) : 0;
  grub_uint8_t bright[3], glow[3];
  for (i = 0; i < 3; i++)
    {
      bright[i] = (spec->color[i] + 255) / 2;
      glow[i] = spec->color[i] * spec->color[i] / 255;
    }

  /* Broken filaments rotate at different depths; the blue corona also ripples radially. */
  for (i = 0; i < tracks; i++)
    {
      unsigned seed = spell_seed (i + 73);
      int cycle = (elapsed / (3 + i % 4) + seed) & 1023;
      int span = (eclipse ? 184 : 126) + ((seed >> 12) & 127);
      int opacity = (49 + intensity / 2) * (166 + lens_wave (cycle * 2 + i) / 3) >> 8;
      int base = (eclipse ? 57 : 70) + 13 * i + (focus * (eclipse ? 24 : 40) >> 8);
      struct spell_point previous = { 0, 0 };
      for (j = 0; j <= 28; j++)
        {
          int phase = (eclipse ? -cycle : cycle) + span * j / 28;
          int ripple = lens_wave (3 * phase + (seed >> 16) - elapsed / 3) / (eclipse ? 52 : 18);
          int detail = lens_wave (7 * phase + elapsed / 2) / (eclipse ? 90 : 37);
          int fade = 256 - grub_abs (14 - j) * 17;
          struct spell_point next = spell_polar (&s, base + ripple + detail, phase, eclipse ? 148 + i * 11 : 235);
          if (j)
            {
              if (!eclipse && (i & 1) == 0)
                spell_ribbon (&s, previous, next, glow, bright, opacity * fade >> 8,
                              2 + focus * s.width / 48000);
              else
                spell_line (&s, previous, next, spec->color, opacity * fade >> 8);
            }
          previous = next;
        }
    }
  for (i = 0; i < (int) spec->particles; i++)
    {
      unsigned seed = spell_seed (i + (eclipse ? 13 : 0));
      int age = ((elapsed * (grub_uint64_t) (400 + ((seed >> 15) & 511)) / 1000) + seed) & 1023;
      int tail = 23 + (int) ((seed >> 12) & 31) + focus / 6;
      int fade = lens_clamp (grub_min (age, 1024 - age) * 2, 256);
      int alpha = ((83 + intensity * 2 / 3) * fade) >> 8;
      struct spell_point head, middle, end;
      /* Extra motes fade in individually as the focus grows. */
      if (i >= (int) spec->particles * (100 + (int) spec->ambient) / 256)
        alpha = alpha * focus >> 8;
      if (alpha < 3)
        continue;
      head = spell_mote (&s, seed, age, focus, eclipse);
      middle = spell_mote (&s, seed, grub_max (0, age - tail / 2), focus, eclipse);
      end = spell_mote (&s, seed, grub_max (0, age - tail), focus, eclipse);
      spell_line (&s, end, middle, spec->color, alpha / 3);
      if ((i % 3) == 0)
        spell_ribbon (&s, middle, head, glow, bright, alpha, 2 + focus * s.width / 62000);
      else
        spell_line (&s, middle, head, bright, alpha);
      if ((i & 7) == 0 && alpha > 100)
        {
          int k = s.width >= 350 ? 2 : 1;
          spell_pixel (&s, head.x - k, head.y, spec->color, alpha / 2);
          spell_pixel (&s, head.x + k, head.y, spec->color, alpha / 2);
          spell_pixel (&s, head.x, head.y - k, bright, alpha / 2);
          spell_pixel (&s, head.x, head.y + k, bright, alpha / 2);
        }
    }
}

static int
spell_distance (int x, int y)
{
  int low = 0, high = grub_abs (x) + grub_abs (y);
  grub_uint64_t square = (grub_int64_t) x * x + (grub_int64_t) y * y;
  while (low < high)
    {
      int mid = low + (high - low + 1) / 2;
      if ((grub_uint64_t) mid * mid > square)
        high = mid - 1;
      else
        low = mid;
    }
  return low;
}

static void
lens_prepare_flows (struct grub_lens_animation *a, const struct grub_video_bitmap *image)
{
  unsigned i, j, budget = 1048576;
  for (i = 0; i < a->count; i++)
    {
      struct lens_region *sink = &a->regions[i];
      if (!sink->spec->source_class[0])
        continue;
      for (j = 0; j < a->count; j++)
        if (i != j && grub_strcmp (sink->spec->source_class, a->regions[j].spec->class_name) == 0)
          {
            struct lens_region *source = &a->regions[j];
            const grub_video_rect_t *s = &source->bounds, *t = &sink->bounds;
            int sx = s->x + s->width / 2, sy = s->y + s->height / 2;
            int tx = t->x + t->width / 2, ty = t->y + t->height / 2;
            int dx = tx - sx, dy = ty - sy, distance = spell_distance (dx, dy);
            int reach = grub_min (t->width / 3, t->height / 2);
            int depth = grub_min (distance / 5, (int) grub_max (s->height, t->height) / 2);
            int ux = dx * 256 / distance, uy = dy * 256 / distance;
            int bx = -uy * depth / 256, by = ux * depth / 256;
            int left = grub_min (sx, tx) + grub_min (bx, 0) - 6;
            int top = grub_min (sy, ty) + grub_min (by, 0) - 6;
            int right = grub_max (sx, tx) + grub_max (bx, 0) + 7;
            int bottom = grub_max (sy, ty) + grub_max (by, 0) + 7;
            struct lens_flow *flow = &a->flows[a->flow_count];
            unsigned row, rowbytes, area;
            grub_err_t saved = grub_errno;
            /* Include the final orbit, plus the counter-bend at the source. */
            int margin = depth / 4 + 4;
            int orbit_x = reach * (grub_abs (ux) + grub_abs (uy) * 2 / 3) / 256 + 4;
            int orbit_y = reach * (grub_abs (uy) + grub_abs (ux) * 2 / 3) / 256 + 4;
            left = grub_max (0, grub_min (left - margin, tx - orbit_x));
            top = grub_max (0, grub_min (top - margin, ty - orbit_y));
            right = grub_min ((int) image->mode_info.width, grub_max (right + margin, tx + orbit_x + 1));
            bottom = grub_min ((int) image->mode_info.height, grub_max (bottom + margin, ty + orbit_y + 1));
            area = (right - left) * (bottom - top);
            if (area > budget)
              break;
            flow->original = grub_malloc (area * image->mode_info.bytes_per_pixel);
            if (!flow->original)
              {
                grub_errno = saved;
                break;
              }
            flow->bounds = (grub_video_rect_t) { .x = left, .y = top, .width = right - left, .height = bottom - top };
            flow->source = source;
            flow->sink = sink;
            flow->ux = ux;
            flow->uy = uy;
            flow->distance = distance;
            flow->reach = reach;
            flow->depth = depth;
            struct spell_surface surface = spell_surface (image, &flow->bounds);
            rowbytes = surface.width * surface.bytes;
            for (row = 0; row < (unsigned) surface.height; row++)
              grub_memcpy (flow->original + row * rowbytes, surface.data + row * surface.pitch, rowbytes);
            a->flow_count++;
            budget -= area;
            break;
          }
    }
}

static void
spell_restore_flow (const struct grub_video_bitmap *image, const struct lens_flow *f)
{
  struct spell_surface s = spell_surface (image, &f->bounds);
  int i, y, rowbytes = s.width * s.bytes;
  for (i = 0; i < LENS_FLOW_BINS; i++)
    {
      const grub_video_rect_t *r = &f->last[i];
      int x = r->x * s.bytes;
      for (y = r->y; y < (int) (r->y + r->height); y++)
        grub_memcpy (s.data + y * s.pitch + x, f->original + y * rowbytes + x, r->width * s.bytes);
    }
}

static void
spell_union (grub_video_rect_t *a, const grub_video_rect_t *b)
{
  if (!a->width)
    *a = *b;
  else if (b->width)
    {
      int right = grub_max (a->x + a->width, b->x + b->width);
      int bottom = grub_max (a->y + a->height, b->y + b->height);
      a->x = grub_min (a->x, b->x);
      a->y = grub_min (a->y, b->y);
      a->width = right - a->x;
      a->height = bottom - a->y;
    }
}

/* Track the painted footprint in strips along the corridor. A diagonal trail
   then presents only its narrow band instead of the large bounding rectangle. */
static void
spell_mark (const struct spell_surface *s, grub_video_rect_t paint[LENS_FLOW_BINS],
            struct spell_point a, struct spell_point b, int radius)
{
  int x0 = grub_max (0, grub_min (a.x, b.x) - radius), y0 = grub_max (0, grub_min (a.y, b.y) - radius);
  int x1 = grub_min (s->width, grub_max (a.x, b.x) + radius + 2);
  int y1 = grub_min (s->height, grub_max (a.y, b.y) + radius + 2);
  int horizontal = s->width >= s->height;
  int extent = horizontal ? s->width : s->height;
  int step = (extent + LENS_FLOW_BINS - 1) / LENS_FLOW_BINS;
  int i, last = ((horizontal ? x1 : y1) - 1) / step;
  if (x0 >= x1 || y0 >= y1)
    return;
  for (i = (horizontal ? x0 : y0) / step; i <= last; i++)
    {
      int start = i * step, end = start + step;
      int x = horizontal ? grub_max (x0, start) : x0;
      int y = horizontal ? y0 : grub_max (y0, start);
      grub_video_rect_t r = { .x = x, .y = y, .width = (horizontal ? grub_min (x1, end) : x1) - x,
                              .height = (horizontal ? y1 : grub_min (y1, end)) - y };
      spell_union (&paint[i], &r);
    }
}

static unsigned
spell_dirty_flow (struct lens_flow *f, const grub_video_rect_t paint[LENS_FLOW_BINS],
                  const grub_video_rect_t *regions, unsigned count, grub_video_rect_t *dirty)
{
  unsigned i, j, written = 0;
  for (i = 0; i < LENS_FLOW_BINS; i++)
    {
      grub_video_rect_t r = f->last[i];
      spell_union (&r, &paint[i]);
      f->last[i] = paint[i];
      if (!r.width)
        continue;
      r.x += f->bounds.x;
      r.y += f->bounds.y;
      for (j = 0; j < count; j++)
        if (regions[j].x <= r.x && regions[j].y <= r.y
            && r.x + r.width <= regions[j].x + regions[j].width
            && r.y + r.height <= regions[j].y + regions[j].height)
          break;
      if (j == count)
        dirty[written++] = r;
    }
  return written;
}

/* First follow an escaping fluctuation, then wind into a shrinking orbit.
   Both pieces meet at the near side of the accretion disk. */
static struct spell_point
spell_stream_point (const struct lens_flow *f, int lane, int t, unsigned time)
{
  int along, across, reach = f->reach, depth = f->depth;
  int start_x = (int) f->source->bounds.x + f->source->bounds.width / 2 - f->bounds.x;
  int start_y = (int) f->source->bounds.y + f->source->bounds.height / 2 - f->bounds.y;
  if (t < 760)
    {
      int u = t * 256 / 760, hump = u * (256 - u) >> 6;
      int length = f->distance - reach;
      int swing = lens_wave (time / 3 - t * 2 + lane * 181);
      int ripple = lens_wave (time / 2 - t * 4 + lane * 293);
      int emit = (lane % 4 - 1) * depth * 2 / 3;
      int bend = depth * (68 + (lane % 5) * 16) / 100;
      int control = spell_lerp (emit, bend, u);
      along = length * u / 256 + (hump * (int) lens_wave (t + lane * 83) / 256) * reach / 1600;
      across = hump * (control + (swing * depth / 1260) + ripple * depth / 3600) / 256;
    }
  else
    {
      int u = (t - 760) * 256 / 264;
      int angle = 512 + u * (750 + (lane % 4) * 92) / 256;
      int radius = spell_lerp (reach, reach * 22 / 100, u);
      int flatten = 123 + 10 * (lane % 5);
      along = f->distance + radius * lens_wave (angle + 256) / 256;
      across = ((radius * lens_wave (angle) / 256) * flatten) / 256;
    }
  struct spell_point p = { start_x + (along * f->ux - across * f->uy) / 256,
                           start_y + (along * f->uy + across * f->ux) / 256 };
  return p;
}

static void
spell_draw_flow (const struct grub_video_bitmap *image, const struct lens_flow *f, unsigned elapsed,
                 grub_video_rect_t paint[LENS_FLOW_BINS])
{
  struct spell_surface s = spell_surface (image, &f->bounds);
  const grub_uint8_t *color = f->source->spec->color;
  grub_uint8_t hot[3], glow[3];
  int lane, i, focus = spell_ease (f->sink->focus);
  unsigned time = (grub_uint64_t) elapsed * f->sink->spec->speed / 100;
  for (i = 0; i < 3; i++)
    {
      hot[i] = (color[i] + 2 * 255) / 3;
      glow[i] = color[i] * color[i] / 255;
      glow[i] = glow[i] * glow[i] / 255;
    }
  for (lane = 0; lane < 7; lane++)
    {
      unsigned seed = spell_seed (lane + 34);
      int phase = ((grub_uint64_t) time * (280 + (seed & 127)) / 1000 + lane * 149) & 1023;
      int head = phase * (620 + phase * 404 / 1024) / 1024;
      int tail = 186 + (int) ((seed >> 8) & 127);
      struct spell_point previous = spell_stream_point (f, lane, 0, time);
      for (i = 8; i <= 1024; i += 8)
        {
          int gap = head - i;
          int envelope = lens_clamp (grub_min (i * 3, (1024 - i) * 3), 256);
          int wake = gap >= 0 && gap < tail ? (tail - gap) * 235 / tail : 0;
          int thread = lane < 3 ? 78 + lens_wave (time / 3 - 2 * i + lane * 300) / 7 : 0;
          int alpha = ((grub_min (246, thread + wake) * focus) >> 8) * envelope >> 8;
          struct spell_point next = spell_stream_point (f, lane, i, time);
          if (i > 760 && f->sink->spec->eclipse)
            {
              const grub_video_rect_t *t = &f->sink->bounds;
              int x = (next.x + (int) f->bounds.x - (int) t->x - (int) t->width / 2) * 1024 / (int) t->width;
              int y = (next.y + (int) f->bounds.y - (int) t->y - (int) t->height / 2) * 700 / (int) t->height;
              alpha = alpha * lens_clamp ((x * x + y * y - 6900) / 22, 256) >> 8;
            }
          if (alpha > 3)
            {
              int width = 2 + (int) f->source->bounds.width * (2 + alpha / 45) / 400;
              width = grub_min (width, 12);
              spell_ribbon (&s, previous, next, glow, gap >= 0 && gap < 45 ? hot : color,
                            alpha, width);
              spell_mark (&s, paint, previous, next, width);
            }
          previous = next;
        }
    }
}
