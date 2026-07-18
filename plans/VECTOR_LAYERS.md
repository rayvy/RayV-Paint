# Vector Layers — Checklist & Status

Krita-aligned shape layers: **geometry is truth**, `tileCache` is disposable raster.

See also `Documentation.MD` § Vector layers.

## How to use (quick)

1. **Layers → Vec** or **Layer → New Vector Layer** (or auto-create when drawing).
2. Draw: **Rectangle / Ellipse / Line / Pen / Freehand / Polygon**.
3. Auto → **Select shapes** — blue box, **drag body** = move, **corner grips** = resize (Shift = uniform).
4. **Double-click** / **Enter** → **Edit nodes**.
5. **Edit**: drag nodes/handles · **double-click segment** = insert · **N** = node type · **Break** splits path.
6. **]** / **[** = raise/lower · **Ctrl+]** / **Ctrl+[** = front/back.
7. **Ctrl+C / V / D** = copy / paste / duplicate.
8. Tool Settings: numeric **X/Y/W/H**, round corners, **Apply style**, **To path**, **Export SVG**.
9. Canvas **VECTOR HUD** shows live tips.

## Status

### A. Layer / document
- [x] New Vector Layer
- [x] Type UI **Vector** (serialize `"vector"`, load `vector_svg` alias)
- [x] Import SVG → Vector layer (nanosvg)
- [x] Open / drop `.svg`
- [x] Export SVG (menu + Tool Settings)
- [x] Rasterize layer drops `vectorDoc`
- [x] Convert shape to path
- [x] Shape z-order
- [x] Align (L/HC/R/T/VC/B) multi-select
- [ ] Groups / booleans
- [x] `.rayp` geometry JSON + re-raster on load
- [x] `VectorEditCommand` undo
- [x] Content paint locked; mask OK

### B. Creation tools
- [x] Vector Pen (Bezier)
- [x] Rectangle (+ corner radius)
- [x] Ellipse
- [x] Line
- [x] Polyline / polygon tool
- [x] Freehand path (simplify + smooth)
- [ ] Calligraphy / text / symbols

### C. Object mode
- [x] Select / move + **Shift multi-select**
- [x] Numeric X/Y/W/H + scale styles + corner resize grips
- [x] Fill/stroke + Apply style (+ gradient/dash) to selection
- [x] Copy/paste/duplicate (Ctrl+C/V/D)
- [x] Delete shape(s)
- [x] Z-order ]/[ · Ctrl+] front · Ctrl+[ back

### D. Point edit
- [x] Edit tool, select/move nodes + handles
- [x] Node types cycle (**N**)
- [x] Insert node (double-click segment)
- [x] Delete node
- [x] Convert rect/ellipse/line → path
- [x] Break path at node
- [x] Join open paths (nearby endpoints)

### E. Style
- [x] Solid fill + stroke width
- [x] Linear gradient fill
- [x] Dash stroke

### F. Rendering
- [x] Dirty-rect tile raster (CPU)
- [x] Coarse while drag
- [ ] Zoom-aware density polish
- [x] Selection bbox + nodes + resize grips + multi-select
- [x] Composite via existing tiles

## Remaining
Booleans, text, shape groups, GPU stroke, Python API.

## Optimization invariants
1. No full-doc float buffer on path edit  
2. Dirty AABB (+ shape expansion) only  
3. Coarse flatten while dragging  
4. Geometry undo (JSON), not pixel undo for edits  
