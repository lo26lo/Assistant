#!/usr/bin/env python3
# =============================================================================
#  eval_model.py — mesure un modèle contre la vérité terrain iBOM
#  (plan Modèle V2 §3c : chaque itération de modèle devient MESURABLE au lieu
#   de « j'ai l'impression que »).
#
#  À lancer sur la machine GPU (comme train_yolo.py). Deux entrées possibles :
#
#    # 1. Un data.yaml classique (dataset fusionné, split train/val déjà fait) :
#    python3 scripts/eval_model.py runs/detect/component_detector/weights/best.pt \
#        --data datasets/merged/data.yaml
#
#    # 2. Une ou plusieurs SESSIONS du DatasetCreator (labels auto-iBOM) —
#    #    le script fabrique un data.yaml temporaire où TOUT est du val :
#    python3 scripts/eval_model.py best.pt \
#        --sessions data/dataset/session_2026-07-12_* \
#        --classes tools/dataset_studio/config/pcb_classes.json
#
#  Sortie : précision / rappel / mAP50 par classe + verdict global.
#  Repères (plan §4) : un modèle utilisable pour le re-ancrage vise
#  rappel ≥ ~0.7 sur les classes fréquentes ; pour le prior de classes
#  (useClassPrior), c'est la PRÉCISION par classe qui compte (une classe
#  souvent confondue ferait rejeter de bonnes correspondances).
# =============================================================================

import argparse
import json
import sys
import tempfile
from pathlib import Path


def build_sessions_yaml(sessions: list[Path], classes: list[str]) -> Path:
    """data.yaml temporaire : toutes les images des sessions en val."""
    imgs = []
    for s in sessions:
        d = s / "images"
        if not d.is_dir():
            print(f"[eval] ATTENTION : {s} n'a pas de sous-dossier images/ — ignorée")
            continue
        imgs.append(str(d.resolve()))
    if not imgs:
        print("[eval] ERREUR : aucune session valide.")
        sys.exit(2)
    tmp = Path(tempfile.mkdtemp(prefix="ibom_eval_")) / "data.yaml"
    # Ultralytics accepte une liste de dossiers pour val ; train est requis
    # par le parseur mais inutilisé par val() — on pointe sur le même contenu.
    lines = ["train:"] + [f"  - {p}" for p in imgs] + ["val:"] + [f"  - {p}" for p in imgs]
    lines += [f"nc: {len(classes)}", "names:"] + [f"  {i}: {n}" for i, n in enumerate(classes)]
    tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[eval] data.yaml temporaire : {tmp}")
    return tmp


def load_classes(path: Path) -> list[str]:
    j = json.loads(path.read_text(encoding="utf-8"))
    classes = j["classes"] if isinstance(j, dict) else j
    if not isinstance(classes, list) or not classes:
        print(f"[eval] ERREUR : {path} ne contient pas une liste de classes.")
        sys.exit(2)
    return classes


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Évalue un modèle YOLO contre la vérité terrain "
                    "(data.yaml ou sessions du DatasetCreator).")
    ap.add_argument("model", help="Chemin du modèle (.pt de préférence ; .onnx accepté)")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--data", help="data.yaml d'un dataset (split val utilisé)")
    src.add_argument("--sessions", nargs="+",
                     help="Dossiers session_* du DatasetCreator (tout passe en val)")
    ap.add_argument("--classes",
                    default="tools/dataset_studio/config/pcb_classes.json",
                    help="Liste ordonnée des classes (mode --sessions). "
                         "Défaut : %(default)s")
    ap.add_argument("--imgsz", type=int, default=640, help="Taille d'inférence")
    ap.add_argument("--conf", type=float, default=0.25,
                    help="Seuil de confiance pour l'éval (défaut %(default)s)")
    args = ap.parse_args()

    try:
        from ultralytics import YOLO
    except ImportError:
        print("[eval] ERREUR : ultralytics manquant. Installe :  pip install ultralytics")
        return 2

    if args.data:
        data_yaml = Path(args.data)
        if not data_yaml.is_file():
            print(f"[eval] ERREUR : {data_yaml} introuvable.")
            return 2
    else:
        classes = load_classes(Path(args.classes))
        data_yaml = build_sessions_yaml([Path(s) for s in args.sessions], classes)

    print(f"[eval] modèle : {args.model}")
    model = YOLO(args.model)
    metrics = model.val(data=str(data_yaml), imgsz=args.imgsz, conf=args.conf,
                        plots=False, verbose=False)

    names = metrics.names  # {id: name}
    print("\n[eval] ── Résultats par classe ─────────────────────────────")
    print(f"{'classe':<16} {'précision':>10} {'rappel':>8} {'mAP50':>8}")
    # metrics.box.p / .r / .ap50 sont indexés par classe PRÉSENTE dans le val
    # (metrics.box.ap_class_index donne les ids correspondants).
    idx = list(getattr(metrics.box, "ap_class_index", range(len(names))))
    for k, cid in enumerate(idx):
        name = names.get(int(cid), str(cid)) if isinstance(names, dict) else names[int(cid)]
        p = float(metrics.box.p[k]) if k < len(metrics.box.p) else float("nan")
        r = float(metrics.box.r[k]) if k < len(metrics.box.r) else float("nan")
        a = float(metrics.box.ap50[k]) if k < len(metrics.box.ap50) else float("nan")
        flag = "  ⚠ précision basse (mauvais pour useClassPrior)" if p < 0.6 else ""
        print(f"{name:<16} {p:>10.2f} {r:>8.2f} {a:>8.2f}{flag}")

    print("\n[eval] ── Global ──────────────────────────────────────────")
    print(f"précision {metrics.box.mp:.2f}  rappel {metrics.box.mr:.2f}  "
          f"mAP50 {metrics.box.map50:.2f}  mAP50-95 {metrics.box.map:.2f}")
    verdict = ("OK pour le re-ancrage" if metrics.box.mr >= 0.7 else
               "rappel insuffisant pour le re-ancrage (< 0.7) — plus de données "
               "ou plus d'époques")
    print(f"[eval] verdict : {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
