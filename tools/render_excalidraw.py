#!/usr/bin/env python3
"""
render_excalidraw.py — render .excalidraw JSON → SVG (single source of truth).

The .excalidraw files under docs/ are the editable masters; the .svg files are
generated renders. Run `make docs-svg` (or this script directly) whenever a
master changes so the two never drift apart.

Supported element types: rectangle, ellipse, diamond, arrow, line, text.
Faithful rendering of complex diagrams:

  * links      — element `link` → wrapped in <a href target=_blank>
  * groups     — `groupIds` (outermost-first) → nested <g id> wrappers with
                 group-aware z-order (a group sits at its lowest member's
                 stacking position, members keep their relative order)
  * rotation   — `angle` (radians) → transform rotate around element center
  * fillStyle  — solid | hachure | cross-hatch → SVG pattern fills
  * isDeleted  — tombstoned elements are skipped
  * dashed/dotted strokes, opacity, rounded corners (roundness type 3)

Multi-line embedded text is centered per the element's textAlign/verticalAlign;
boxes with box-drawing characters render in monospace. Z-order follows the
Excalidraw `index` field (a0 < a1 < ... < a10 — length-aware sort).

Usage:
    python tools/render_excalidraw.py            # every docs/*.excalidraw
    python tools/render_excalidraw.py FILE...    # specific files only

Stdlib only — no pip installs.
"""

import glob
import json
import math
import os
import sys

FONT_BODY = "'Segoe UI', system-ui, sans-serif"
FONT_MONO = "'Cascadia Mono', Consolas, 'Courier New', monospace"
PAD = 24


def esc(s):
    return (s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
             .replace('"', '&quot;'))


def f2(v):
    return '%g' % round(v, 2)


def idx_key(e):
    idx = e.get('index', '')
    return (len(idx), idx)


def is_deleted(e):
    return bool(e.get('isDeleted', False))


def ebox(e):
    """Absolute bounding box of one element (pre-rotation)."""
    t = e['type']
    if t in ('arrow', 'line'):
        pts = e.get('points') or [[0, 0]]
        xs = [e['x'] + p[0] for p in pts]
        ys = [e['y'] + p[1] for p in pts]
        return min(xs), min(ys), max(xs), max(ys)
    if t == 'text':
        fs = e.get('fontSize', 20)
        lines = (e.get('text') or '').split('\n')
        w = max((len(ln) for ln in lines), default=1) * fs * 0.6
        h = len(lines) * fs * 1.25
        return e['x'], e['y'], e['x'] + w, e['y'] + h
    return (e['x'], e['y'],
            e['x'] + e.get('width', 0), e['y'] + e.get('height', 0))


def bbox(els):
    xs0, ys0, xs1, ys1 = [], [], [], []
    for e in els:
        a, b, c, d = ebox(e)
        ang = e.get('angle', 0)
        if ang:
            cx, cy = e['x'] + e.get('width', 0) / 2, e['y'] + e.get('height', 0) / 2
            cs, sn = math.cos(ang), math.sin(ang)
            for px, py in ((a, b), (c, b), (c, d), (a, d)):
                rx = cx + (px - cx) * cs - (py - cy) * sn
                ry = cy + (px - cx) * sn + (py - cy) * cs
                xs0.append(rx)
                ys0.append(ry)
                xs1.append(rx)
                ys1.append(ry)
        else:
            xs0.append(a)
            ys0.append(b)
            xs1.append(c)
            ys1.append(d)
    return min(xs0), min(ys0), max(xs1), max(ys1)


def rot_wrap(e, body):
    """Wrap in a rotation group when the element has an angle."""
    ang = e.get('angle', 0)
    if not ang:
        return body
    cx = e['x'] + e.get('width', 0) / 2
    cy = e['y'] + e.get('height', 0) / 2
    deg = math.degrees(ang)
    return ('<g transform="rotate(%s %s %s)">%s</g>'
            % (f2(deg), f2(cx), f2(cy), body))


def link_wrap(e, body):
    """Wrap in an anchor when the element carries an external link."""
    link = e.get('link')
    if not link:
        return body
    return ('<a href="%s" target="_blank" rel="noopener">%s</a>'
            % (esc(link), body))


def render_text(x, y, w, h, el):
    """Multi-line text block inside (x,y,w,h) per element alignment."""
    fs = el.get('fontSize', 20)
    fill = el.get('strokeColor', '#1e1e1e')
    lines = (el.get('text') or '').split('\n')
    n = len(lines)
    lh = fs * 1.25
    ta = el.get('textAlign', 'left')
    anchor = {'left': 'start', 'center': 'middle', 'right': 'end'}.get(ta, 'start')
    ax = {'start': x, 'middle': x + w / 2, 'end': x + w}[anchor]
    va = el.get('verticalAlign', 'top')
    if va == 'middle':
        base = y + h / 2 + fs * 0.35 - (n - 1) * lh / 2
    elif va == 'bottom':
        base = y + h - fs * 0.25 - (n - 1) * lh
    else:
        base = y + fs * 0.8
    fam = FONT_MONO if any('\u2500' in ln for ln in lines) else FONT_BODY
    out = ('<text x="%s" y="%s" text-anchor="%s" font-family="%s" '
           'font-size="%s" fill="%s">'
           % (f2(ax), f2(base), anchor, fam, f2(fs), esc(fill)))
    for i, ln in enumerate(lines):
        out += '<tspan x="%s" dy="%s">%s</tspan>' % (f2(ax),
                                                     f2(lh) if i else '0',
                                                     esc(ln))
    out += '</text>'
    return out


def render_shape(e, patterns):
    x, y = e['x'], e['y']
    w, h = e.get('width', 0), e.get('height', 0)
    t = e['type']
    fill = e.get('backgroundColor', 'transparent')
    if fill not in (None, 'transparent'):
        pkey = (fill, e.get('strokeColor', '#1e1e1e'), e.get('fillStyle', 'solid'))
        if pkey in patterns:
            fill = 'url(#%s)' % patterns[pkey]
    stroke = e.get('strokeColor', '#1e1e1e')
    sw = e.get('strokeWidth', 2)
    dash = ''
    if e.get('strokeStyle') == 'dashed':
        dash = ' stroke-dasharray="6,4"'
    elif e.get('strokeStyle') == 'dotted':
        dash = ' stroke-dasharray="2,4"'
    op = e.get('opacity', 100)
    pre = '<g opacity="%s">' % (op / 100) if op < 100 else ''
    post = '</g>' if op < 100 else ''

    if t == 'rectangle':
        rr = e.get('roundness') or {}
        rx = min(w, h) * 0.08 if rr.get('type') == 3 else 0
        rx = min(rx, 24)
        body = ('<rect x="%s" y="%s" width="%s" height="%s" rx="%s" '
                'fill="%s" stroke="%s" stroke-width="%s"%s/>'
                % (f2(x), f2(y), f2(w), f2(h), f2(rx),
                   esc(fill), esc(stroke), sw, dash))
    elif t == 'ellipse':
        body = ('<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="%s" '
                'stroke="%s" stroke-width="%s"%s/>'
                % (f2(x + w / 2), f2(y + h / 2), f2(w / 2), f2(h / 2),
                   esc(fill), esc(stroke), sw, dash))
    elif t == 'diamond':
        pts = '%s,%s %s,%s %s,%s %s,%s' % (f2(x + w / 2), f2(y),
                                           f2(x + w), f2(y + h / 2),
                                           f2(x + w / 2), f2(y + h),
                                           f2(x), f2(y + h / 2))
        body = ('<polygon points="%s" fill="%s" stroke="%s" '
                'stroke-width="%s"%s/>' % (pts, esc(fill), esc(stroke), sw, dash))
    else:
        return ''

    if e.get('text'):
        body += render_text(x, y, w, h, e)
    return pre + body + post


def render_arrow(e, marker_id):
    pts = e.get('points') or [[0, 0]]
    x0, y0 = e['x'], e['y']
    d = 'M ' + ' L '.join('%s %s' % (f2(x0 + p[0]), f2(y0 + p[1])) for p in pts)
    stroke = e.get('strokeColor', '#1e1e1e')
    sw = e.get('strokeWidth', 2)
    dash = ''
    if e.get('strokeStyle') == 'dashed':
        dash = ' stroke-dasharray="6,4"'
    elif e.get('strokeStyle') == 'dotted':
        dash = ' stroke-dasharray="2,4"'
    marker = ' marker-end="url(#%s)"' % marker_id if e['type'] == 'arrow' else ''
    return ('<path d="%s" fill="none" stroke="%s" stroke-width="%s"%s%s/>'
            % (d, esc(stroke), sw, dash, marker))


def pattern_defs(els):
    """Hachure / cross-hatch fills → <pattern> defs, keyed by (fill,stroke,style)."""
    used = {}
    for e in els:
        fill = e.get('backgroundColor')
        style = e.get('fillStyle')
        if fill in (None, 'transparent') or style not in ('hachure', 'cross-hatch'):
            continue
        key = (fill, e.get('strokeColor', '#1e1e1e'), style)
        used.setdefault(key, 'pat%d' % len(used))
    out = []
    for (fill, stroke, style), pid in used.items():
        lines = ['<path d="M0,8 L8,0" stroke="%s" stroke-width="1"/>' % esc(stroke)]
        if style == 'cross-hatch':
            lines.append('<path d="M0,0 L8,8" stroke="%s" stroke-width="1"/>' % esc(stroke))
        out.append('<pattern id="%s" width="8" height="8" patternUnits="userSpaceOnUse">'
                   '<rect width="8" height="8" fill="%s"/>%s</pattern>'
                   % (pid, esc(fill), ''.join(lines)))
    return out, used


def order_elements(els):
    """
    Group-aware z-order: a group renders as one unit at the stacking position
    of its lowest (earliest) member; members keep their relative order.
    Ungrouped elements sort by their own index against group positions.
    """
    flat = sorted(els, key=idx_key)
    pos = {e['id']: i for i, e in enumerate(flat)}
    outer = {}
    for e in els:
        if e.get('groupIds'):
            outer.setdefault(e['groupIds'][0], []).append(e)
    gz = {g: min(pos[e['id']] for e in members) for g, members in outer.items()}

    def key(e):
        if e.get('groupIds'):
            return (gz[e['groupIds'][0]], pos[e['id']])
        return (pos[e['id']], pos[e['id']])

    return sorted(flat, key=key)


def render(src, dst):
    with open(src, encoding='utf-8') as fh:
        doc = json.load(fh)
    els = [e for e in doc['elements'] if not is_deleted(e)]

    minx, miny, maxx, maxy = bbox(els)
    W, H = maxx - minx + 2 * PAD, maxy - miny + 2 * PAD
    bg = (doc.get('appState') or {}).get('viewBackgroundColor', '#ffffff')

    colors = {}
    for e in els:
        if e['type'] == 'arrow':
            colors.setdefault(e.get('strokeColor', '#1e1e1e'),
                              'ah%d' % len(colors))
    defs = ['<marker id="%s" markerWidth="9" markerHeight="9" refX="7.5" '
            'refY="4.5" orient="auto"><path d="M0,0 L9,4.5 L0,9 Z" fill="%s"/>'
            '</marker>' % (mid, esc(c)) for c, mid in colors.items()]
    pdefs, patterns = pattern_defs(els)
    defs.extend(pdefs)

    parts = []
    open_groups = []
    prev_gids = []
    for e in order_elements(els):
        gids = list(e.get('groupIds') or [])
        # longest common prefix with the previous element's groups → close
        # groups we left, open ones we entered (nesting support)
        common = 0
        for a, b in zip(prev_gids, gids):
            if a == b:
                common += 1
            else:
                break
        while len(open_groups) > common:
            open_groups.pop()
            parts.append('  </g>')
        while len(open_groups) < len(gids):
            g = gids[len(open_groups)]
            open_groups.append(g)
            parts.append('  <g id="%s">' % esc(g))
        prev_gids = gids

        t = e['type']
        if t in ('rectangle', 'ellipse', 'diamond'):
            body = render_shape(e, patterns)
        elif t in ('arrow', 'line'):
            mid = colors.get(e.get('strokeColor', '#1e1e1e'), '')
            body = render_arrow(e, mid)
        elif t == 'text':
            body = render_text(e['x'], e['y'],
                               e.get('width', 0), e.get('height', 0), e)
        else:
            body = ''
        body = rot_wrap(e, body)
        body = link_wrap(e, body)
        if body:
            parts.append('  ' + body)
    while open_groups:
        open_groups.pop()
        parts.append('  </g>')

    svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="%s %s %s %s" '
           'width="%s" height="%s" font-family="%s">\n'
           % (f2(minx - PAD), f2(miny - PAD), f2(W), f2(H), f2(W), f2(H),
              FONT_BODY))
    svg += ('  <rect x="%s" y="%s" width="%s" height="%s" fill="%s"/>\n'
            % (f2(minx - PAD), f2(miny - PAD), f2(W), f2(H), esc(bg)))
    if defs:
        svg += '  <defs>\n' + ''.join('    ' + d + '\n' for d in defs) + '  </defs>\n'
    for p in parts:
        svg += p + '\n'
    svg += '</svg>\n'
    with open(dst, 'w', encoding='utf-8') as fh:
        fh.write(svg)
    print('  ✓ %s → %s' % (src, dst))


def main():
    args = sys.argv[1:]
    files = args or sorted(glob.glob('docs/*.excalidraw'))
    if not files:
        print('no .excalidraw files found', file=sys.stderr)
        return 1
    for src in files:
        dst = os.path.splitext(src)[0] + '.svg'
        render(src, dst)
    print('rendered %d diagram(s) — svg now mirrors the .excalidraw masters' % len(files))
    return 0


if __name__ == '__main__':
    sys.exit(main())