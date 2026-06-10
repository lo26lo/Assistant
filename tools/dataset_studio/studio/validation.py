"""Validation du dataset projet : orchestre le DatasetValidator (vendorisé de
Pokemon-Dataset-Creator) session par session, agrège les stats de classes, et
génère une mosaïque d'aperçu avec les bboxes dessinées (contrôle visuel rapide
que les labels collent aux composants)."""

import random
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import cv2
import numpy as np

from .vendor.dataset_validator import DatasetValidator

PALETTE = [  # BGR, une couleur par classe (cyclique)
    (66, 133, 244), (52, 168, 83), (251, 188, 5), (234, 67, 53),
    (171, 71, 188), (0, 172, 193), (255, 112, 67), (158, 157, 36),
    (92, 107, 192), (38, 166, 154), (236, 64, 122), (141, 110, 99),
    (120, 144, 156), (124, 179, 66),
]


@dataclass
class ValidationSummary:
    per_session: list[dict] = field(default_factory=list)   # résultats bruts par session
    class_counts: dict[int, int] = field(default_factory=dict)
    total_images: int = 0
    total_errors: int = 0
    total_warnings: int = 0
    report_paths: list[Path] = field(default_factory=list)
    preview_path: Path | None = None


def validate_project(sessions: list[Path], reports_dir: Path,
                     class_names: list[str], log=print) -> ValidationSummary:
    summary = ValidationSummary()
    counts: dict[int, int] = defaultdict(int)

    for session in sessions:
        log(f"🔍 Validation de {session.name} …")
        validator = DatasetValidator(session)
        results = validator.validate()
        results["session"] = session.name
        summary.per_session.append(results)
        summary.total_images += results["total_images"]
        summary.total_errors += len(results["errors"])
        summary.total_warnings += len(results["warnings"])
        for cls_id, n in results.get("class_distribution", {}).items():
            counts[int(cls_id)] += n

        reports_dir.mkdir(parents=True, exist_ok=True)
        report = reports_dir / f"validation_{session.name}.html"
        validator.save_report_html(str(report))
        summary.report_paths.append(report)

    summary.class_counts = dict(sorted(counts.items()))

    # Classes absentes — important avant d'entraîner
    for cls_id, name in enumerate(class_names):
        if counts.get(cls_id, 0) == 0:
            log(f"⚠️  Classe '{name}' (id {cls_id}) : aucune annotation dans le dataset")

    if sessions:
        summary.preview_path = _make_preview(sessions, reports_dir, class_names, log)
    return summary


def _draw_boxes(img: np.ndarray, label_file: Path, class_names: list[str]):
    h, w = img.shape[:2]
    if not label_file.exists():
        return
    for line in label_file.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) != 5:
            continue
        cls = int(float(parts[0]))
        cx, cy, bw, bh = (float(v) for v in parts[1:])
        x1 = int((cx - bw / 2) * w); y1 = int((cy - bh / 2) * h)
        x2 = int((cx + bw / 2) * w); y2 = int((cy + bh / 2) * h)
        color = PALETTE[cls % len(PALETTE)]
        cv2.rectangle(img, (x1, y1), (x2, y2), color, 2)
        name = class_names[cls] if cls < len(class_names) else str(cls)
        cv2.putText(img, name, (x1, max(12, y1 - 4)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.45, color, 1, cv2.LINE_AA)


def _make_preview(sessions: list[Path], reports_dir: Path,
                  class_names: list[str], log=print,
                  grid=(3, 3), tile_w=420) -> Path | None:
    """Mosaïque grid[0]×grid[1] d'images aléatoires avec bboxes dessinées."""
    rng = random.Random()
    pool = []
    for s in sessions:
        pool += [(p, s / "labels" / (p.stem + ".txt"))
                 for p in (s / "images").glob("*.jp*g")]
        pool += [(p, s / "labels" / (p.stem + ".txt"))
                 for p in (s / "images").glob("*.png")]
    if not pool:
        return None
    picks = rng.sample(pool, min(grid[0] * grid[1], len(pool)))

    tiles = []
    for img_path, label_path in picks:
        img = cv2.imread(str(img_path))
        if img is None:
            continue
        _draw_boxes(img, label_path, class_names)
        scale = tile_w / img.shape[1]
        tiles.append(cv2.resize(img, (tile_w, int(img.shape[0] * scale))))
    if not tiles:
        return None

    tile_h = min(t.shape[0] for t in tiles)
    tiles = [t[:tile_h] for t in tiles]
    rows = []
    for r in range(0, len(tiles), grid[1]):
        row = tiles[r:r + grid[1]]
        while len(row) < grid[1]:
            row.append(np.zeros_like(tiles[0]))
        rows.append(cv2.hconcat(row))
    mosaic = cv2.vconcat(rows)

    out = reports_dir / "preview_bboxes.jpg"
    cv2.imwrite(str(out), mosaic, [cv2.IMWRITE_JPEG_QUALITY, 90])
    log(f"🖼️  Aperçu bboxes : {out}")
    return out
