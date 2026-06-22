#!/usr/bin/env python3
# =============================================================================
#  fetch_roboflow_dataset.py — télécharge un dataset Roboflow Universe au
#  format YOLOv8 pour MicroscopeIBOM (piste B : détecteur de présence, et
#  piste A : pré-entraînement du détecteur de composants).
#
#  Usage :
#    pip install roboflow
#    export ROBOFLOW_API_KEY=xxxxxxxx        # clé perso (Settings > API)
#    python3 scripts/fetch_roboflow_dataset.py \
#        https://universe.roboflow.com/marco-filippozzi-siwjn/smd-component-detection \
#        --out datasets/smd
#
#  La clé API est lue depuis --api-key, sinon $ROBOFLOW_API_KEY.
#  Si --version n'est pas donné, la dernière version du projet est utilisée.
#
#  Datasets évalués dans docs/AI_MODEL_DATASETS_PLAN.md :
#    - marco-filippozzi-siwjn/smd-component-detection   (meilleur pour nous)
#    - pcb-components-tqghw/pcb-component-detection-v2-zcun5
#    - roboflow-100/printed-circuit-board               (à écarter, benchmark)
#
#  ⚠️ Vérifier la LICENCE de chaque dataset sur sa page Roboflow avant de
#     diffuser des poids entraînés dessus.
# =============================================================================

import argparse
import os
import re
import sys
from pathlib import Path


def parse_slug(url_or_slug: str):
    """Extrait (workspace, project) d'une URL Universe ou d'un 'ws/proj'."""
    s = url_or_slug.strip().rstrip("/")
    # Retire le schéma + l'hôte si une URL complète est fournie.
    s = re.sub(r"^https?://(universe|app)\.roboflow\.com/", "", s)
    parts = [p for p in s.split("/") if p]
    if len(parts) < 2:
        raise ValueError(
            f"Impossible d'extraire workspace/project de '{url_or_slug}'. "
            "Attendu : 'workspace/project' ou l'URL Universe complète.")
    return parts[0], parts[1]


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Télécharge un dataset Roboflow Universe (format YOLOv8).")
    ap.add_argument("url", help="URL Universe ou slug 'workspace/project'")
    ap.add_argument("--out", default=None,
                    help="Dossier de sortie (défaut : datasets/<project>)")
    ap.add_argument("--version", type=int, default=None,
                    help="Numéro de version (défaut : la plus récente)")
    ap.add_argument("--format", default="yolov8",
                    help="Format d'export Roboflow (défaut : %(default)s)")
    ap.add_argument("--api-key", default=os.environ.get("ROBOFLOW_API_KEY"),
                    help="Clé API Roboflow (défaut : $ROBOFLOW_API_KEY)")
    args = ap.parse_args()

    if not args.api_key:
        print("ERREUR : aucune clé API. Passe --api-key ou exporte "
              "ROBOFLOW_API_KEY.", file=sys.stderr)
        return 2

    try:
        from roboflow import Roboflow
    except ImportError:
        print("ERREUR : paquet 'roboflow' manquant. Fais : pip install roboflow",
              file=sys.stderr)
        return 2

    try:
        workspace, project_id = parse_slug(args.url)
    except ValueError as e:
        print(f"ERREUR : {e}", file=sys.stderr)
        return 2

    out = Path(args.out) if args.out else Path("datasets") / project_id
    out.parent.mkdir(parents=True, exist_ok=True)

    print(f"[fetch] workspace={workspace} project={project_id} "
          f"format={args.format} -> {out}")

    rf = Roboflow(api_key=args.api_key)
    project = rf.workspace(workspace).project(project_id)

    version = args.version
    if version is None:
        # project.versions() renvoie la liste des versions ; on prend la plus
        # grande (la plus récente). L'API expose .version sous forme d'URL.
        versions = project.versions()
        if not versions:
            print("ERREUR : aucune version publiée pour ce projet.", file=sys.stderr)
            return 1
        nums = []
        for v in versions:
            m = re.search(r"/(\d+)$", str(getattr(v, "version", "")))
            if m:
                nums.append(int(m.group(1)))
        version = max(nums) if nums else 1
        print(f"[fetch] version non précisée -> dernière = {version}")

    dataset = project.version(version).download(args.format, location=str(out))
    loc = getattr(dataset, "location", str(out))
    print(f"[fetch] OK -> {loc}")
    print("[fetch] Prochaine étape : remap des classes "
          "(scripts/remap_classes.py) puis entraînement (scripts/train_yolo.py).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
