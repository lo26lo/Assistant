#!/usr/bin/env python3
# =============================================================================
#  train_yolo.py — entraîne un YOLOv8 pour MicroscopeIBOM (Ultralytics).
#
#  À lancer sur une machine GPU (PAS le Jetson — l'entraînement y est lent ;
#  le Jetson ne fait QUE l'inférence via ONNX Runtime + TensorRT EP).
#
#  Usage :
#    pip install ultralytics
#
#    # PISTE B — détecteur de présence (1 classe), rapide, petit modèle :
#    python3 scripts/train_yolo.py datasets/smd/data.yaml \
#        --model yolov8n.pt --epochs 80 --name reanchor_presence
#
#    # PISTE A — détecteur de composants (14 classes), modèle moyen :
#    python3 scripts/train_yolo.py datasets/merged/data.yaml \
#        --model yolov8m.pt --epochs 150 --name component_detector
#
#  En sortie : runs/detect/<name>/weights/best.pt
#  -> exporter ensuite en ONNX :
#       python3 scripts/export_yolov8_onnx.py \
#           runs/detect/<name>/weights/best.pt \
#           --out models/<nom_attendu>.onnx
#
#  Le .onnx doit s'appeler comme `ai.detector_model` du config.json
#  (défaut component_detector). Le .txt des classes est généré par
#  l'export — ModelManager le charge automatiquement (cf. docs/AI_PIPELINE.md).
# =============================================================================

import argparse
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Entraîne un YOLOv8 (présence ou composants) pour MicroscopeIBOM.")
    ap.add_argument("data", help="Chemin du data.yaml du dataset")
    ap.add_argument("--model", default="yolov8n.pt",
                    help="Poids de départ (yolov8n/s/m.pt). Défaut : %(default)s")
    ap.add_argument("--epochs", type=int, default=100)
    ap.add_argument("--imgsz", type=int, default=640,
                    help="Taille d'entrée du modèle. L'app lit cette taille "
                         "dans le .onnx (InferenceEngine::loadModel) et "
                         "letterboxe en conséquence — 1280 marche donc tel "
                         "quel et voit mieux les petits composants "
                         "(0402/0201), au prix d'une inférence ~4x plus "
                         "lente sur l'Orin. Défaut : %(default)s")
    ap.add_argument("--batch", type=int, default=16)
    ap.add_argument("--name", default="train",
                    help="Nom du run (runs/detect/<name>)")
    ap.add_argument("--device", default="0",
                    help="GPU id, 'cpu', ou '0,1' pour multi-GPU. Défaut : %(default)s")
    ap.add_argument("--patience", type=int, default=30,
                    help="Early-stopping : époques sans amélioration. Défaut : %(default)s")
    ap.add_argument("--resume", action="store_true",
                    help="Reprend le dernier run du même --name")
    args = ap.parse_args()

    data_path = Path(args.data)
    if not data_path.exists():
        print(f"ERREUR : {data_path} introuvable.", file=sys.stderr)
        return 2

    if args.imgsz != 640:
        print(f"NOTE : imgsz={args.imgsz} ≠ 640. Supporté : l'app s'adapte à "
              "la taille d'entrée du .onnx (letterbox inclus) — exporte "
              "l'ONNX avec le même --imgsz. Compter une inférence plus "
              "lente sur le Jetson.", file=sys.stderr)

    try:
        from ultralytics import YOLO
    except ImportError:
        print("ERREUR : paquet 'ultralytics' manquant. Fais : pip install ultralytics",
              file=sys.stderr)
        return 2

    print(f"[train] model={args.model} data={data_path} epochs={args.epochs} "
          f"imgsz={args.imgsz} batch={args.batch} device={args.device}")

    model = YOLO(args.model)
    model.train(
        data=str(data_path),
        epochs=args.epochs,
        imgsz=args.imgsz,
        batch=args.batch,
        name=args.name,
        device=args.device,
        patience=args.patience,
        resume=args.resume,
    )

    best = Path("runs") / "detect" / args.name / "weights" / "best.pt"
    print(f"[train] terminé. Meilleur checkpoint attendu : {best}")
    print("[train] Export ONNX : "
          f"python3 scripts/export_yolov8_onnx.py {best} "
          "--out models/component_detector.onnx")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
