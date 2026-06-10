"""Générateur de dataset factice — pour tester le wizard sans vraies données.

Produit des sessions au même layout que la capture Jetson (Phase A) :
    <dest>/session_fake_<n>/images/*.jpg + labels/*.txt + manifest.jsonl

Les images imitent grossièrement un PCB (fond vert texturé) avec des
"composants" rectangulaires dont la taille/couleur dépend de la classe.
Les labels YOLO sont exacts par construction → la validation doit passer.
"""

import json
import random
from pathlib import Path

import cv2
import numpy as np

# (largeur, hauteur) typiques en px et couleur BGR par classe — purement cosmétique
CLASS_STYLES = {
    0:  ((30, 14), (60, 60, 60)),      # resistor
    1:  ((26, 16), (50, 90, 140)),     # capacitor
    2:  ((34, 20), (40, 40, 90)),      # inductor
    3:  ((28, 13), (30, 30, 30)),      # diode
    4:  ((18, 18), (60, 200, 230)),    # led
    5:  ((34, 28), (25, 25, 25)),      # transistor_sot
    6:  ((70, 44), (20, 20, 20)),      # ic_soic
    7:  ((110, 110), (15, 15, 15)),    # ic_qfp
    8:  ((60, 60), (18, 18, 18)),      # ic_qfn
    9:  ((130, 130), (10, 10, 10)),    # ic_bga
    10: ((90, 36), (200, 200, 200)),   # connector
    11: ((46, 22), (190, 190, 170)),   # crystal
    12: ((50, 50), (90, 90, 90)),      # button
    13: ((40, 24), (80, 60, 100)),     # other
}

LIGHT_TAGS = ["ring", "lateral", "ambiant"]
ZOOM_TAGS = ["x0.5", "x1", "x2"]


def _pcb_background(w: int, h: int, rng: random.Random) -> np.ndarray:
    base = np.full((h, w, 3), (40, 90 + rng.randint(-15, 15), 30), np.uint8)
    noise = np.random.default_rng(rng.randint(0, 1 << 30)).integers(
        -12, 12, (h, w, 3), dtype=np.int16)
    img = np.clip(base.astype(np.int16) + noise, 0, 255).astype(np.uint8)
    # quelques "pistes"
    for _ in range(12):
        p1 = (rng.randint(0, w), rng.randint(0, h))
        p2 = (rng.randint(0, w), rng.randint(0, h))
        cv2.line(img, p1, p2, (50, 120, 45), 2)
    return img


def generate(dest: Path, n_sessions: int = 2, images_per_session: int = 30,
             size=(1280, 720), n_classes: int = 14, seed: int = 42,
             log=print) -> list[Path]:
    """Génère les sessions factices, retourne la liste des dossiers créés."""
    rng = random.Random(seed)
    w, h = size
    created = []

    for s in range(1, n_sessions + 1):
        session = Path(dest) / f"session_fake_{s:02d}"
        (session / "images").mkdir(parents=True, exist_ok=True)
        (session / "labels").mkdir(parents=True, exist_ok=True)
        manifest = []
        light = rng.choice(LIGHT_TAGS)
        zoom = rng.choice(ZOOM_TAGS)

        for i in range(images_per_session):
            img = _pcb_background(w, h, rng)
            lines = []
            for _ in range(rng.randint(8, 22)):
                cls = rng.randrange(n_classes)
                (cw, ch), color = CLASS_STYLES.get(cls, ((40, 24), (80, 80, 80)))
                scale = rng.uniform(0.7, 1.6)
                bw, bh = int(cw * scale), int(ch * scale)
                if rng.random() < 0.5:
                    bw, bh = bh, bw  # rotation 90° approximée
                x = rng.randint(0, max(1, w - bw - 1))
                y = rng.randint(0, max(1, h - bh - 1))
                cv2.rectangle(img, (x, y), (x + bw, y + bh), color, -1)
                cv2.rectangle(img, (x, y), (x + bw, y + bh), (210, 210, 210), 1)
                # label YOLO normalisé (exact par construction)
                cx, cy = (x + bw / 2) / w, (y + bh / 2) / h
                lines.append(f"{cls} {cx:.6f} {cy:.6f} {bw / w:.6f} {bh / h:.6f}")

            stem = f"frame_{i:06d}"
            cv2.imwrite(str(session / "images" / f"{stem}.jpg"), img,
                        [cv2.IMWRITE_JPEG_QUALITY, 95])
            (session / "labels" / f"{stem}.txt").write_text(
                "\n".join(lines) + "\n", encoding="utf-8")
            manifest.append({"frame": stem, "lighting": light, "zoom": zoom,
                             "board_id": "fake_board", "inliers": 99,
                             "reproj_err": 0.5})

        (session / "manifest.jsonl").write_text(
            "\n".join(json.dumps(m) for m in manifest) + "\n", encoding="utf-8")
        log(f"🧪 {session.name}: {images_per_session} images générées "
            f"(éclairage={light}, zoom={zoom})")
        created.append(session)

    return created
