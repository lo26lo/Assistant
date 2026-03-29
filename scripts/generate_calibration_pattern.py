"""
Generate microscope calibration checkerboard patterns as PDF.
Prints at exact real-world dimensions — verify with calipers after printing.
"""
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.pdfgen import canvas

OUTPUT = "calibration_patterns.pdf"

# Each pattern: (label, square_size_mm, cols, rows)
# cols×rows = number of SQUARES (inner corners = cols-1 × rows-1)
PATTERNS = [
    ("1.0mm squares — 8×6 (7×5 inner corners)", 1.0, 8, 6),
    ("0.5mm squares — 8×6 (7×5 inner corners)", 0.5, 8, 6),
    ("2.0mm squares — 8×6 (7×5 inner corners)", 2.0, 8, 6),
]


def draw_pattern(c: canvas.Canvas, label: str, sq: float, cols: int, rows: int):
    page_w, page_h = A4
    board_w = cols * sq * mm
    board_h = rows * sq * mm

    # Center on page
    x0 = (page_w - board_w) / 2
    y0 = (page_h - board_h) / 2

    # Title
    c.setFont("Helvetica-Bold", 14)
    c.drawCentredString(page_w / 2, page_h - 30 * mm, f"Calibration Checkerboard — {label}")

    # Dimensions info
    c.setFont("Helvetica", 9)
    c.drawCentredString(page_w / 2, page_h - 37 * mm,
                        f"Board: {cols*sq:.1f} × {rows*sq:.1f} mm  |  "
                        f"Inner corners: {cols-1} × {rows-1}  |  "
                        f"Square: {sq} mm")
    c.drawCentredString(page_w / 2, page_h - 42 * mm,
                        "PRINT AT 100% SCALE — NO FIT-TO-PAGE — Verify with calipers!")

    # Draw checkerboard
    for row in range(rows):
        for col in range(cols):
            if (row + col) % 2 == 0:
                c.setFillColorRGB(0, 0, 0)
            else:
                c.setFillColorRGB(1, 1, 1)
            x = x0 + col * sq * mm
            y = y0 + (rows - 1 - row) * sq * mm
            c.rect(x, y, sq * mm, sq * mm, fill=1, stroke=0)

    # Thin border around the board
    c.setStrokeColorRGB(0.5, 0.5, 0.5)
    c.setLineWidth(0.3)
    c.rect(x0, y0, board_w, board_h, fill=0, stroke=1)

    # Scale ruler below board (5mm ticks)
    ruler_y = y0 - 8 * mm
    ruler_len = min(board_w, 20 * mm)
    c.setStrokeColorRGB(0, 0, 0)
    c.setLineWidth(0.5)
    c.line(x0, ruler_y, x0 + ruler_len, ruler_y)
    c.setFont("Helvetica", 6)
    tick = 0
    while tick * mm <= ruler_len + 0.01:
        tx = x0 + tick * mm
        h = 2 * mm if tick % 5 == 0 else 1 * mm
        c.line(tx, ruler_y, tx, ruler_y + h)
        if tick % 5 == 0:
            c.drawCentredString(tx, ruler_y - 2.5 * mm, f"{tick}")
        tick += 1
    c.drawString(x0 + ruler_len + 2 * mm, ruler_y - 1 * mm, "mm")


def main():
    c = canvas.Canvas(OUTPUT, pagesize=A4)
    for i, (label, sq, cols, rows) in enumerate(PATTERNS):
        draw_pattern(c, label, sq, cols, rows)
        c.showPage()
    c.save()
    print(f"Generated {OUTPUT} with {len(PATTERNS)} pages")


if __name__ == "__main__":
    main()
