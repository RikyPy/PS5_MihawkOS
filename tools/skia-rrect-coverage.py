#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A rounded rectangle's corner coverage as Chromium's Skia rasterizes it (CPU, analytic anti-aliasing).

The prototype's 1 px window ring (box-shadow 0 0 0 1px) is a rounded rectangle Skia fills with analytic AA
(SkScan_AAAPath.cpp, aaa_walk_convex_edges): SkPath::addRRect's conics become two quads each (SkAutoConicToQuads,
tolerance 1/4 px), each quad a few line segments in 16.16 fixed point with their y snapped to 1/4 px
(SkAnalyticQuadraticEdge), and each pixel row's coverage is those trapezoids' areas with Skia's own approximations
(partial triangles, smooth edges carried to the next whole row). Its corners are lighter than an exact circle and not
symmetric: on File Explorer's ring over the reference desktop (2026-10-18) the supersampled circle missed 24 of 272
tip pixels by up to 4 levels (light) and 49 by up to 17 (dark); this model's coverage puts 272/272 and 271/272 within
a level (worst 2), and the inactive ring's 136 in light and dark within a level.

Only what a rounded rectangle at whole-pixel bounds needs is here, re-expressed from Skia's fixed-point arithmetic
(BSD 3-Clause, see NOTICE.md). The corners do not depend on where the rectangle lies or its size (checked at seven
places and sizes), so a small one is drawn.

  python3 tools/skia-rrect-coverage.py 9     the C initializer of pw_window_ring_corners (src/pw_window_effects.h)
"""
import sys

import numpy as np

f32 = np.float32
ONE = 1 << 16
MAXS32 = (1 << 31) - 1


def cdiv(a, b):  # C's division, toward zero
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def fmul(a, b): return (a * b) >> 16
def ceil_fixed(x): return (x + 0xffff) & ~0xffff
def ceil_int(x): return (x + 0xffff) >> 16
def snap_y(y): return (y + (ONE >> 3)) >> 14 << 14  # SnapY: to 1/4 px
def inverse(i): return cdiv(1 << 22, i)  # QuickFDot6Inverse::Lookup


def quick_div(a, b):
    if 8 <= abs(b) < 1024 and abs(a) < 1 << 12:
        return (a * inverse(b)) >> 6
    return cdiv(a << 16, b)


def conic_quads(p0, p1, p2, w=f32(0.70710678118654752440)):
    """SkAutoConicToQuads at tolerance 1/4: SkConic::computeQuadPOW2, then halving in float."""
    p0, p1, p2 = (np.array(p, dtype=f32) for p in (p0, p1, p2))
    a = f32(w - f32(1))
    k = f32(a / (f32(4) * (f32(2) + a)))
    x, y = k * (p0[0] - f32(2) * p1[0] + p2[0]), k * (p0[1] - f32(2) * p1[1] + p2[1])
    error, pow2 = np.sqrt(f32(x * x + y * y)), 0
    while pow2 < 5 and error > 0.25:
        error, pow2 = f32(error * f32(0.25)), pow2 + 1
    conics = [(p0, p1, p2, f32(w))]
    for _ in range(pow2):
        halves = []
        for c0, c1, c2, cw in conics:
            scale, wp1 = f32(f32(1) / (f32(1) + cw)), cw * c1
            m = (c0 + f32(2) * wp1 + c2) * scale * f32(0.5)
            nw = np.sqrt(f32(f32(0.5) + cw * f32(0.5)))
            halves += [(c0, (c0 + wp1) * scale, m, nw), (m, (wp1 + c2) * scale, c2, nw)]
        conics = halves
    return [c[:3] for c in conics]


class Edge:
    """SkAnalyticEdge: the current line segment [upper_y, lower_y) and, for a quad, its forward differences."""

    def line(self, x0, y0, x1, y1, slope):  # updateLine
        if y0 > y1:
            x0, x1, y0, y1 = x1, x0, y1, y0
        dx, dy = (x1 - x0) >> 10, (y1 - y0) >> 10
        if dy == 0:
            return False
        flat = abs(slope >> 10)
        self.x = self.upper_x = x0
        self.y = self.upper_y = y0
        self.lower_y, self.dx = y1, slope
        self.dy = MAXS32 if dx == 0 or slope == 0 else inverse(flat) if flat < 1024 else abs(quick_div(dy, dx))
        return True

    def update(self):  # updateQuadratic; False when the edge has no segment left
        if self.count <= 0:
            return False
        ok, count, oldx, oldy, dx, dy, shift = False, self.count, self.qx, self.qy, self.qdx, self.qdy, self.shift
        while True:
            count -= 1
            if count > 0:
                newx, newy = oldx + (dx >> shift), oldy + (dy >> shift)
                if abs(dy >> shift) >= 2 * ONE:
                    diff = (newy - self.sy) >> 10
                    slope = quick_div((newx - self.sx) >> 10, diff) if diff else MAXS32
                    sy = min(self.last_y, (newy + 0x8000) & ~0xffff)
                    sx = newx - fmul(slope, newy - sy)
                else:
                    sy, sx = min(self.last_y, snap_y(newy)), newx
                    diff = (sy - self.sy) >> 10
                    slope = quick_div((newx - self.sx) >> 10, diff) if diff else MAXS32
                dx, dy = dx + self.qddx, dy + self.qddy
            else:
                newx, newy = sx, sy = self.last_x, self.last_y
                diff = (newy - self.sy) >> 10
                slope = quick_div((newx - self.sx) >> 10, diff) if diff else MAXS32
            if slope < MAXS32:
                ok = self.line(self.sx, self.sy, sx, sy, slope)
            oldx, oldy = newx, newy
            if count <= 0 or ok:
                break
        self.qx, self.qy, self.qdx, self.qdy, self.sx, self.sy, self.count = newx, newy, dx, dy, sx, sy, count
        return ok

    def go_y(self, y):
        if y == self.y + ONE:
            self.x += self.dx
        elif y != self.y:
            self.x = self.upper_x + fmul(self.dx, y - self.upper_y)
        self.y = y


def quad_edge(pts):
    """SkAnalyticQuadraticEdge::setQuadratic: 1/4 px fixed point, 2^shift segments by the curve's depth."""
    (x0, y0), (x1, y1), (x2, y2) = ((int(f32(x) * f32(256)), int(f32(y) * f32(256))) for x, y in pts)
    if y0 > y2:
        x0, y0, x2, y2 = x2, y2, x0, y0
    if (y0 + 32) >> 6 == (y2 + 32) >> 6:
        return None
    dx, dy = abs(((x1 << 1) - x0 - x2) >> 2), abs(((y1 << 1) - y0 - y2) >> 2)
    dist = ((dx + (dy >> 1) if dx > dy else dy + (dx >> 1)) + 16) >> 5
    shift = min(max(dist.bit_length() >> 1, 1), 6)
    e = Edge()
    e.count, e.shift = 1 << shift, shift - 1
    ax, ay = (x0 - 2 * x1 + x2) << 9, (y0 - 2 * y1 + y2) << 9
    e.qx, e.qdx, e.qddx = (x0 << 10) >> 2, ((x1 - x0) << 10) + (ax >> shift) >> 2, (ax >> (shift - 1)) >> 2
    e.qy, e.qdy, e.qddy = snap_y((y0 << 10) >> 2), ((y1 - y0) << 10) + (ay >> shift) >> 2, (ay >> (shift - 1)) >> 2
    e.last_x, e.last_y = (x2 << 10) >> 2, snap_y((y2 << 10) >> 2)
    e.sx, e.sy = e.qx, e.qy
    return e if e.update() else None


def line_edge(p0, p1):
    """SkAnalyticEdge::setLine for an axis-aligned side at whole pixels."""
    (x0, y0), (x1, y1) = ((int(x) << 16, int(y) << 16) for x, y in (p0, p1))
    e = Edge()
    e.count = 0
    return e if e.line(x0, y0, x1, y1, quick_div((x1 - x0) >> 10, (y1 - y0) >> 10) if y1 != y0 else 0) else None


def smooth_edge(this, nxt):
    if this.count > 0:
        return abs(this.qdx) >> 1 >= abs(this.qddx) and abs(this.qdy) >> 1 >= abs(this.qddy) and \
            (this.qdy - this.qddy) >> this.shift >= ONE
    return abs(nxt.dx - this.dx) <= ONE and nxt.lower_y - nxt.upper_y >= ONE


def smooth(left, rite, rest, stop):
    """is_smooth_enough: may the segments be carried to the next whole row?"""
    if not rest or rest[0].upper_y >= stop << 16:
        return False
    if left.lower_y + ONE < rite.lower_y:
        return smooth_edge(left, rest[0])
    if left.lower_y > rite.lower_y + ONE:
        return smooth_edge(rite, rest[0])
    if len(rest) < 2 or rest[1].upper_y >= stop << 16:
        return False
    a, b = (rest[1], rest[0]) if rest[1].upper_x < rest[0].upper_x else (rest[0], rest[1])
    return smooth_edge(left, a) and smooth_edge(rite, b)


def triangle(a, b): return (((a >> 11) * (a >> 11) * (b >> 11)) >> 8) & 0xff  # partial_triangle_to_alpha
def trapezoid(l1, l2): return ((l1 + l2) // 2 >> 8) & 0xff
def partial(alpha, height): return ((alpha * height + 0x8000) >> 16) & 0xff  # get_partial_alpha(alpha, SkFixed)


class Mask:
    """Coverage as the RLE blitter leaves it: full-height rows go straight to the device, partial rows add up and
    are flushed with snapAlpha."""

    def __init__(self):
        self.direct, self.added = {}, {}

    def put(self, x, y, alpha, full, scale=True):
        """A pixel's alpha in a row of height full/255: blit_single_alpha and blit_two_alphas scale it by full,
        blit_full_alpha and blit_aaa_trapezoid_row have it already."""
        if full == 0xff:
            self.direct[(x, y)] = alpha
        else:
            self.added[(x, y)] = self.added.get((x, y), 0) + ((alpha * full) >> 8 if scale else alpha)

    def coverage(self):
        out = {p: 0xff if a > 247 else 0 if a < 8 else a for p, a in self.added.items()}
        out.update(self.direct)
        return out


def below_line(alphas, off, l, r, dy, full):
    n = ceil_int(r)
    if n == 1:
        alphas[off] = (trapezoid(l, r) * full) >> 8
    elif n > 1:
        last = r - ((n - 1) << 16)
        last_h = fmul(last, dy)
        alphas[off + n - 1] = (fmul(last, last_h) >> 9) & 0xff
        a16 = last_h + (dy >> 1)
        for i in range(n - 2, 0, -1):
            alphas[off + i], a16 = (a16 >> 8) & 0xff, a16 + dy
        alphas[off] = (full - triangle(ONE - l, dy)) & 0xff


def above_line(alphas, off, l, r, dy, full):
    n = ceil_int(r)
    if n == 1:
        alphas[off] = ((((n << 17) - l - r) >> 9) * full) >> 8
    elif n > 1:
        first = ONE - l
        first_h = fmul(first, dy)
        alphas[off] = (fmul(first, first_h) >> 9) & 0xff
        a16 = first_h + (dy >> 1)
        for i in range(1, n - 1):
            alphas[off + i], a16 = (a16 >> 8) & 0xff, a16 + dy
        alphas[off + n - 1] = (full - triangle(r - ((n - 1) << 16), dy)) & 0xff


def aaa_row(mask, y, ul, ur, ll, lr, ldy, rdy, full):  # blit_aaa_trapezoid_row
    left, n = ul >> 16, ceil_int(lr) - (ul >> 16)
    if n == 1:
        mask.put(left, y, trapezoid(ur - ul, lr - ll), full)
        return
    alphas = [full] * n
    for upper, lower, dy, below in ((ul, ll, ldy, True), (ur, lr, rdy, False)):
        u, lo = upper >> 16, ceil_int(lower)
        cut = [0] * (n + 2)
        if u + 2 == lo:
            first = (u << 16) + ONE - upper
            second = lower - upper - first
            pair = ((full - triangle(first, dy)) & 0xff, triangle(second, dy)) if below else \
                (triangle(first, dy), (full - triangle(second, dy)) & 0xff)
            cut[u - left], cut[u - left + 1] = pair
        else:
            (below_line if below else above_line)(cut, u - left, upper - (u << 16), lower - (u << 16), dy, full)
        for i in range(u, lo):
            alphas[i - left] = max(alphas[i - left] - cut[i - left], 0)
    for i in range(n):
        mask.put(left + i, y, alphas[i], full, False)


def trapezoid_row(mask, y, ul, ur, ll, lr, ldy, rdy, full):  # blit_trapezoid_row
    if ul > ur:
        return
    if ll > lr:
        ll = lr = (max(min(ul, ll), min(ur, lr)) + min(max(ul, ll), max(ur, lr))) // 2
    if ul == ur and ll == lr:
        return
    ul, ll = min(ul, ll), max(ul, ll)
    ur, lr = min(ur, lr), max(ur, lr)
    join_l, join_r = ceil_fixed(ll), ur & ~0xffff
    if join_l > join_r:
        aaa_row(mask, y, ul, ur, ll, lr, ldy, rdy, full)
        return
    if ul < join_l:
        n = ceil_int(join_l - ul)
        if n == 1:
            mask.put(ul >> 16, y, trapezoid(join_l - ul, join_l - ll), full)
        elif n == 2:
            first = join_l - ONE - ul
            mask.put(ul >> 16, y, triangle(first, ldy), full)
            mask.put((ul >> 16) + 1, y, (full - triangle(ll - ul - first, ldy)) & 0xff, full)
        else:
            aaa_row(mask, y, ul, join_l, ll, join_l, ldy, MAXS32, full)
    for x in range(join_l >> 16, join_r >> 16):
        mask.put(x, y, full, full, False)
    if lr > join_r:
        n = ceil_int(lr - join_r)
        if n == 1:
            mask.put(join_r >> 16, y, trapezoid(ur - join_r, lr - join_r), full)
        elif n == 2:
            first = join_r + ONE - ur
            mask.put(join_r >> 16, y, (full - triangle(first, rdy)) & 0xff, full)
            mask.put((join_r >> 16) + 1, y, triangle(lr - ur - first, rdy), full)
        else:
            aaa_row(mask, y, join_r, ur, join_r, lr, MAXS32, rdy, full)


def rrect_coverage(l, t, r, b, radius):
    """{(x, y): 0..255} of the rounded rectangle [l, r) x [t, b) as SkPath::addRRect and aaa_walk_convex_edges draw it."""
    arcs = (((l, t + radius), (l, t), (l + radius, t)), ((r - radius, t), (r, t), (r, t + radius)),
            ((r, b - radius), (r, b), (r - radius, b)), ((l + radius, b), (l, b), (l, b - radius)))
    sides = (((r, t + radius), (r, b - radius)), ((l, b - radius), (l, t + radius)))
    edges = [e for arc in arcs for e in map(quad_edge, conic_quads(*arc)) if e] + [e for e in (line_edge(*s) for s in sides) if e]
    edges.sort(key=lambda e: (e.upper_y, e.x, e.dx))
    mask, stop = Mask(), b
    left, rite, rest = edges[0], edges[1], edges[2:]
    y = max(left.upper_y, rite.upper_y)
    snap, half = ONE >> 4, ONE >> 5
    while True:
        if left.lower_y <= y or rite.lower_y <= y:
            done = left if left.lower_y <= y else rite
            if not done.update():
                if not rest or rest[0].upper_y >> 16 >= stop:
                    break
                if done is left:
                    left = rest.pop(0)
                else:
                    rite = rest.pop(0)
            continue
        if y >> 16 >= stop:
            break
        left.go_y(y)
        rite.go_y(y)
        if left.x > rite.x or (left.x == rite.x and left.dx > rite.dx):
            left, rite = rite, left
        bottom = min(left.lower_y, rite.lower_y)
        if smooth(left, rite, rest, stop):
            bottom = ceil_fixed(bottom)
        bottom = min(bottom, stop << 16)
        lx, rx = left.x, rite.x
        if left.dx == 0 and rite.dx == 0:  # straight sides at whole pixels: full rows
            for row in range(ceil_int(y), bottom >> 16):
                for x in range(ceil_int(lx), rx >> 16):
                    mask.put(x, row, 0xff, 0xff)
            y = bottom
        else:
            lx, rx = lx + half, rx + half
            rows = ceil_int(bottom) - (y >> 16)
            steps = []
            if rows > 1 and y & 0xffff:
                rows -= 1
                steps.append(ceil_fixed(y + 1) - y)
            steps += [ONE] * (rows - 1)
            for dy in steps:
                nl, nr = lx + (fmul(left.dx, dy) if dy != ONE else left.dx), rx + (fmul(rite.dx, dy) if dy != ONE else rite.dx)
                trapezoid_row(mask, y >> 16, lx & -snap, rx & -snap, nl & -snap, nr & -snap, left.dy, rite.dy,
                              0xff if dy == ONE else partial(0xff, dy))
                lx, rx, y = nl, nr, y + dy
            dy = bottom - y
            nl, nr = max(lx + fmul(left.dx, dy), half), rx + fmul(rite.dx, dy)
            trapezoid_row(mask, y >> 16, lx & -snap, rx & -snap, nl & -snap, nr & -snap, left.dy, rite.dy, partial(0xff, dy))
            lx, rx, y = nl - half, nr - half, bottom
        left.x, rite.x = lx, rx
        left.y = rite.y = y
    return mask.coverage()


def ring_corners(radius):
    """[corner][j][i] for corners top-left, top-right, bottom-left, bottom-right: i across from the corner's outermost
    column, j down (or up) from its outermost row."""
    l = t = 16
    r, b = l + 2 * radius + 6, t + 2 * radius + 6
    cover = rrect_coverage(l, t, r, b, radius)
    return [[[cover.get((ox + sx * i, oy + sy * j), 0) for i in range(radius)] for j in range(radius)]
            for ox, oy, sx, sy in ((l, t, 1, 1), (r - 1, t, -1, 1), (l, b - 1, 1, -1), (r - 1, b - 1, -1, -1))]


if __name__ == '__main__':
    corners = ring_corners(int(sys.argv[1]) if len(sys.argv) > 1 else 9)
    print('{' + ',\n '.join('{' + ','.join('{' + ','.join(str(v) for v in row) + '}' for row in c) + '}' for c in corners) + '}')
