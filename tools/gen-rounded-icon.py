#!/usr/bin/env python3
"""Generates the rounded (Fluent / Windows 11 style) tray icon variant.

    python tools/gen-rounded-icon.py

The glyph this variant replaces is a *stroked outline*: its outer contour is the
speaker's centreline offset by half the stroke width, and its Bluetooth rune is a
filled polygon with sharp corners. The generated variant draws the same speaker
centreline with a round-capped, round-joined stroke of the original's weight - so
the silhouette is preserved and every corner becomes round - and draws the rune as
the canonical polyline with round caps.

Two constraints shape the output:

* Direct2D renders the tray icon with the root element's ``fill`` overridden by
  the theme colour (see Direct2DSvg.hpp), so a stroke has to be expressed as
  filled geometry: one rectangle per segment plus a disc at each end. Subpaths of
  a single fill colour union, so one ``<path>`` is enough and the renderer needs
  no change.
* Only the path commands the shipped glyph already relied on (M, L and Z) are
  emitted. The discs are 24-gons, whose flatness error is about 0.006px at tray
  size, so the file cannot depend on SVG features the renderer may not implement.

Every subpath is emitted in a canonical winding: the pieces of a stroke overlap
(the discs cover the rectangle's ends), and under the nonzero fill rule an
oppositely wound overlap would punch a hole in the glyph.
"""

import math
import pathlib

WEIGHT = 124.0
DISC_SEGMENTS = 24
CANVAS = 1536.0

OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "AudioPlaybackConnector.Rounded.svg"

# Speaker: the shipped outline's centreline, scaled to leave room beside the rune.
# The box runs x 90..350 / y 470..1090 and the cone meets the mouth at x 690.
SPEAKER = [(90, 470), (350, 470), (690, 90), (690, 1470), (350, 1090), (90, 1090)]

# Bluetooth rune: the canonical polyline, mapped onto the shipped rune's footprint.
RUNE_SOURCE = [(6.5, 6.5), (17.5, 17.5), (12, 23), (12, 1), (17.5, 6.5), (6.5, 17.5)]
# The rune is drawn rather than filled, so the stroke widens its footprint: this
# mapping keeps its drawn width and height close to the shipped rune's, and leaves
# a gap of ~126 units - about 1.3px at 16px, where the shipped icon's own gap is
# ~154 units (1.6px) - so the two parts stay apart at tray size.
RUNE_LEFT, RUNE_RIGHT = 940.0, 1460.0
RUNE_TOP, RUNE_BOTTOM = 222.0, 1314.0


def ring(points):
    """Emits a closed subpath, reversed if needed so every subpath winds alike."""
    area = 0.0
    for index in range(len(points)):
        x1, y1 = points[index]
        x2, y2 = points[(index + 1) % len(points)]
        area += x1 * y2 - x2 * y1
    if area < 0:
        points = list(reversed(points))
    return "M%.1f %.1f" % points[0] + "".join("L%.1f %.1f" % p for p in points[1:]) + "Z"


def disc(cx, cy, radius):
    points = []
    for index in range(DISC_SEGMENTS):
        angle = 2.0 * math.pi * index / DISC_SEGMENTS
        points.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    return ring(points)


def segment(x1, y1, x2, y2):
    """One round-capped stroke segment as filled geometry."""
    dx, dy = x2 - x1, y2 - y1
    length = math.hypot(dx, dy)
    nx, ny = -dy / length * WEIGHT / 2.0, dx / length * WEIGHT / 2.0
    corners = [(x1 + nx, y1 + ny), (x2 + nx, y2 + ny),
               (x2 - nx, y2 - ny), (x1 - nx, y1 - ny)]
    radius = WEIGHT / 2.0
    return ring(corners) + disc(x1, y1, radius) + disc(x2, y2, radius)


def stroke(points, closed=True):
    path = "".join(segment(*points[i], *points[i + 1]) for i in range(len(points) - 1))
    return path + (segment(*points[-1], *points[0]) if closed else "")


def rune_point(x, y):
    return (RUNE_LEFT + (x - 6.5) * (RUNE_RIGHT - RUNE_LEFT) / 11.0,
            RUNE_TOP + (y - 1.0) * (RUNE_BOTTOM - RUNE_TOP) / 22.0)


def main():
    rune = [rune_point(x, y) for x, y in RUNE_SOURCE]

    # Centre the drawing on the canvas. The offset is baked into the coordinates
    # rather than applied with a transform, so the file's structure is exactly the
    # shipped glyph's: one viewBox and one <path>.
    points = SPEAKER + rune
    half = WEIGHT / 2.0
    min_x, max_x = min(p[0] for p in points) - half, max(p[0] for p in points) + half
    min_y, max_y = min(p[1] for p in points) - half, max(p[1] for p in points) + half
    shift_x = round((CANVAS - (max_x - min_x)) / 2.0 - min_x, 1)
    shift_y = round((CANVAS - (max_y - min_y)) / 2.0 - min_y, 1)

    def centred(polygon):
        return [(x + shift_x, y + shift_y) for x, y in polygon]

    path = stroke(centred(SPEAKER)) + stroke(centred(rune), closed=False)

    svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d">'
           '<path d="%s"/></svg>\n' % (int(CANVAS), int(CANVAS), path))

    OUTPUT.write_text(svg, encoding="utf-8", newline="\n")

    commands = "".join(sorted(set(c for c in path if c in "MLAZHVCQSTA")))
    print("wrote %s" % OUTPUT.name)
    print("  subpaths: %d, commands: %s, bytes: %d" % (path.count("M"), commands, len(svg)))


if __name__ == "__main__":
    main()
