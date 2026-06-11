"""
Generate tiled calibration checkerboard patterns on A4 page.
4 patterns in 4-column grid, fills entire page, high resolution, no text.
"""
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.pdfgen import canvas

OUTPUT = "calibration_patterns_tiled.pdf"

# Each pattern: (sq_size_mm, cols, rows)
# cols×rows = number of SQUARES (inner corners = cols-1 × rows-1)
PATTERNS = [
    (1.0, 8, 6),    # 8×6mm
    (0.5, 8, 6),    # 4×3mm
    (2.0, 8, 6),    # 16×12mm
    (5.0, 8, 6),    # 40×30mm
]


def draw_pattern_plain(c: canvas.Canvas, x_pos: float, y_pos: float, sq: float, cols: int, rows: int):
    """Draw checkerboard pattern + scale ruler below + border."""
    board_w = cols * sq * mm
    board_h = rows * sq * mm

    # Draw checkerboard
    for row in range(rows):
        for col in range(cols):
            if (row + col) % 2 == 0:
                c.setFillColorRGB(0, 0, 0)
            else:
                c.setFillColorRGB(1, 1, 1)
            x = x_pos + col * sq * mm
            y = y_pos + (rows - 1 - row) * sq * mm
            c.rect(x, y, sq * mm, sq * mm, fill=1, stroke=0)

    # Border around pattern
    c.setStrokeColorRGB(0, 0, 0)
    c.setLineWidth(0.2)
    c.rect(x_pos, y_pos, board_w, board_h, fill=0, stroke=1)

    # Scale ruler below board (5mm ticks)
    ruler_y = y_pos - 3 * mm
    ruler_len = min(board_w, 20 * mm)
    c.setStrokeColorRGB(0, 0, 0)
    c.setLineWidth(0.3)
    c.line(x_pos, ruler_y, x_pos + ruler_len, ruler_y)

    # Tick marks
    tick = 0
    while tick * mm <= ruler_len + 0.01:
        tx = x_pos + tick * mm
        h = 1.5 * mm if tick % 5 == 0 else 0.7 * mm
        c.line(tx, ruler_y, tx, ruler_y + h)
        tick += 1


def main():
    page_w, page_h = A4
    margin = 5 * mm
    spacing = 7 * mm

    # Calculate dimensions for each pattern
    tiles = []
    for sq, cols, rows in PATTERNS:
        w = cols * sq * mm
        h = rows * sq * mm
        tiles.append((sq, cols, rows, w, h))

    # Calculate row height = height of tallest pattern + ruler + spacing
    max_height = max(t[4] for t in tiles)
    ruler_space = 4 * mm
    row_height = max_height + ruler_space + spacing

    # Calculate available space
    avail_w = page_w - 2 * margin
    col_width = (avail_w - 3 * spacing) / 4

    # Calculate how many rows fit
    avail_h = page_h - 2 * margin
    num_rows = int(avail_h / row_height)

    c = canvas.Canvas(OUTPUT, pagesize=A4, compress=1)

    # Draw grid
    for row_idx in range(num_rows):
        y_base = page_h - margin - row_idx * row_height

        if y_base < margin:
            break

        for col_idx, (sq, cols, rows, tile_w, tile_h) in enumerate(tiles):
            x_base = margin + col_idx * (col_width + spacing)

            # Center pattern within column
            x_center = x_base + (col_width - tile_w) / 2
            # Align to top of row
            y_pos = y_base - tile_h

            draw_pattern_plain(c, x_center, y_pos, sq, cols, rows)

    c.save()
    print(f"Generated {OUTPUT} — {num_rows} rows of 4 patterns per row")


if __name__ == "__main__":
    main()
