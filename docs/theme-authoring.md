# Writing a selection-driven theme

Start with an opaque JPEG or RGB/RGBA PNG. Paint a visible detail where you want the lens to form, such as a ring, a spiral or a star field. The renderer samples the original artwork on every frame, so texture around the center makes the motion visible.

## Describe a lens

```text
desktop-image: "sky.png"
desktop-image-scale-method: "stretch"
desktop-lens: "frost vortex 105 174 340 250 #87e2ec 18 130"
desktop-lens: "ember eclipse 740 190 360 260 #ffb66a 34 75"
```

| Field | Meaning |
| --- | --- |
| Class | A GRUB menu-entry class. Use up to 63 ASCII letters, digits, hyphens or underscores. |
| Effect | `vortex` twists the texture around the center. `eclipse` bends the surrounding texture around a central shadow and a horizontal beam. |
| X and Y | Position of the rectangle's top-left corner in the source image. |
| Width and height | Dimensions of the rectangle in source pixels. The lens is an ellipse inscribed in this rectangle. |
| Color | Particle tint in `#RRGGBB` form. The underlying art supplies the color of the warp. |
| Particles | Optional count from 0 to 64. The default is 26. Zero disables local motes and corona filaments. |
| Speed | Optional percentage from 25 to 200. The default is 100. Supply the particle count before speed. |
| Ambient | Optional strength from 0 to 100. Zero (default) keeps the original focus-only animation. A positive value enables persistent local emission and a larger, eased magnification on focus. |
| Source class | Optional final word. When this lens gains focus, a portion of the named lens's material crosses the background and spirals inward. Supply the preceding fields first. |

In your GRUB configuration, give each real boot entry the class used by its lens. The same entry can carry conventional icon classes and a theme-specific class.

```grub
menuentry 'Example system' --class frost {
    echo 'Replace this body with your working boot commands.'
}
```

Keep labels, timers and other interface elements as normal GRUB theme widgets. BetterGRUB draws them over the animated background. Focus-only lenses fade over about a quarter of a second. Ambient lenses grow and settle over about half a second and continue emitting when focus moves away.

## Connect two lenses

```text
desktop-lens: "spring vortex 248 383 488 360 #71dcff 56 112 48"
desktop-lens: "well eclipse 1230 413 508 320 #ff993e 46 102 42 spring"
```

With `well` focused, moving streaks inherit the color of `spring`. The route curves across the gap in the direction perpendicular to the line between the centers, then spirals into the receiving lens. For a source on the left and a receiver on the right, the route bows downward. Leave space there in the painting. Other particles escape the source throughout the transfer. Moving focus away fades the stream and restores the original background.

The order of the two declarations does not matter. A missing source or a self-reference leaves the individual lenses available. The renderer reserves up to 1,048,576 displayed pixels in total for restoring stream corridors. If a corridor exceeds the remaining budget or cannot be allocated, the endpoint animations still work.

## Resolutions and fallback

Rectangles scale with the stretched desktop image. Use source-image coordinates even when the display has a different resolution. Other scaling modes use the static GRUB theme.

A theme can define up to eight lenses. Use distinct classes and non-overlapping rectangles. Definitions are read in order. A malformed definition, an out-of-bounds rectangle or a later region overlapping an active region is skipped. If an entry has several matching classes, its first matching GRUB class wins.

Each region must be at least 8 × 8 pixels in the source and on screen. The preboot renderer budgets up to 1,048,576 displayed pixels per lens and 2,097,152 in total. An allocation failure falls back to the static menu.

Ordinary menu icons remain available. A theme that uses full-screen class icons for static selection can set the following property. An active lens then takes the place of that entry's icon. Entries with no active lens keep their icon.

```text
desktop-lens-icons: "replace"
```

The Frieren example includes full-screen icon layouts for 1920×1080, 1280×720, 1024×768 and 800×600. The top-level `theme.txt` uses a responsive menu for other modes. The [demo configuration](../examples/grub.cfg) shows how to choose a layout at boot.
