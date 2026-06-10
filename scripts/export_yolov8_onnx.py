#!/usr/bin/env python3
# =============================================================================
#  export_yolov8_onnx.py — exporte un modèle YOLOv8 (.pt) vers ONNX pour
#  MicroscopeIBOM (ONNX Runtime + TensorRT EP sur Jetson AGX Orin).
#
#  Usage :
#    pip install ultralytics
#    python3 scripts/export_yolov8_onnx.py chemin/vers/best.pt \
#        --out models/component_detector.onnx
#
#  Le nom de sortie doit correspondre à `ai.detector_model` du config.json
#  (défaut : component_detector). Le .txt des classes est généré à côté
#  (même stem) — ModelManager le charge automatiquement.
#
#  Choix d'export (alignés sur InferenceEngine.cpp) :
#    - opset 17        : supporté par ORT 1.19 + TensorRT 10.3 (JetPack 6.2)
#    - imgsz 640       : taille d'entraînement YOLOv8 standard
#    - half=False      : on exporte en FP32 — c'est le TensorRT EP qui passe
#                        en FP16 au build de l'engine (trt_fp16_enable=1).
#                        Un ONNX déjà FP16 limiterait le fallback CUDA/CPU.
#    - dynamic=False   : shapes statiques → engine TRT plus simple et plus
#                        rapide à compiler, suffisant pour un flux caméra fixe.
#    - simplify=True   : onnx-simplifier réduit le graphe (moins de noeuds
#                        non supportés par TRT → moins de fallbacks).
# =============================================================================

import argparse
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Export YOLOv8 .pt -> ONNX pour MicroscopeIBOM (Jetson)")
    parser.add_argument("weights", help="Chemin du checkpoint .pt (ex: runs/detect/train/weights/best.pt)")
    parser.add_argument("--out", default="models/component_detector.onnx",
                        help="Chemin du .onnx de sortie (défaut: %(default)s)")
    parser.add_argument("--imgsz", type=int, default=640,
                        help="Taille d'entrée carrée (défaut: %(default)s)")
    parser.add_argument("--opset", type=int, default=17,
                        help="Opset ONNX (défaut: %(default)s — compatible ORT 1.19 + TRT 10.3)")
    args = parser.parse_args()

    try:
        from ultralytics import YOLO
    except ImportError:
        print("ERREUR: ultralytics non installé. Faire: pip install ultralytics",
              file=sys.stderr)
        return 1

    weights = Path(args.weights)
    if not weights.exists():
        print(f"ERREUR: {weights} introuvable", file=sys.stderr)
        return 1

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    print(f"[export] Chargement {weights} ...")
    model = YOLO(str(weights))

    print(f"[export] Export ONNX (imgsz={args.imgsz}, opset={args.opset}, "
          f"FP32 statique simplifié) ...")
    exported = model.export(
        format="onnx",
        imgsz=args.imgsz,
        opset=args.opset,
        half=False,      # FP16 fait par le TensorRT EP, pas dans l'ONNX
        dynamic=False,   # shapes statiques -> engine TRT simple et rapide
        simplify=True,
    )

    exported = Path(exported)
    exported.replace(out)
    print(f"[export] ONNX : {out}")

    # Classes : un nom par ligne, dans l'ordre des ids — ModelManager charge
    # automatiquement le .txt qui partage le stem du .onnx.
    labels = out.with_suffix(".txt")
    names = model.names  # dict {id: name}
    with open(labels, "w", encoding="utf-8") as f:
        for i in range(len(names)):
            f.write(f"{names[i]}\n")
    print(f"[export] Classes ({len(names)}) : {labels}")

    print()
    print("Déploiement sur le Jetson :")
    print(f"  scp {out} {labels} jetson:~/Assistant-git/models/")
    print("  → relancer l'app ; le 1er lancement compile l'engine TensorRT")
    print("    (minutes, une seule fois — caché ensuite dans IBOM_DATA_DIR/tensorrt-cache)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
