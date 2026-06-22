#!/usr/bin/env python3
# =============================================================================
#  remap_classes.py — remappe les classes d'un dataset YOLO téléchargé.
#
#  Deux modes (cf. docs/AI_MODEL_DATASETS_PLAN.md) :
#
#   • PISTE B — détecteur de "présence" :
#       python3 scripts/remap_classes.py datasets/smd --presence
#     Toutes les classes source -> une seule classe `component` (id 0).
#     C'est tout ce dont le re-ancrage au niveau composant a besoin
#     (il matche des POSITIONS, pas des types).
#
#   • PISTE A — vers nos 14 classes :
#       python3 scripts/remap_classes.py datasets/smd --map mapping.yaml
#     `mapping.yaml` mappe les noms de classes SOURCE -> nos noms cibles
#     (resistor, capacitor, ... other). Toute classe source absente du
#     mapping tombe en `other` (ou est supprimée avec --drop-unmapped).
#     L'ordre des classes cibles est FIGÉ par pcb_classes.json — ne pas
#     réordonner (les ids YOLO en dépendent).
#
#  Le remap se fait EN PLACE : les .txt de labels et le data.yaml sont
#  réécrits. Travaille sur une COPIE si tu veux garder l'original.
# =============================================================================

import argparse
import sys
from pathlib import Path

# Même liste ordonnée que tools/dataset_studio/config/pcb_classes.json
# et resources/footprint_classes.json — NE PAS RÉORDONNER.
PROJECT_CLASSES = [
    "resistor", "capacitor", "inductor", "diode", "led", "transistor_sot",
    "ic_soic", "ic_qfp", "ic_qfn", "ic_bga", "connector", "crystal",
    "button", "other",
]


def load_yaml(path: Path):
    import yaml
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f)


def dump_yaml(path: Path, data):
    import yaml
    with open(path, "w", encoding="utf-8") as f:
        yaml.safe_dump(data, f, sort_keys=False, allow_unicode=True)


def source_names(data_yaml: dict):
    """Renvoie la liste ordonnée des noms de classes source."""
    names = data_yaml.get("names")
    if isinstance(names, dict):
        # {0: 'a', 1: 'b'} -> ['a', 'b'] dans l'ordre des clés.
        return [names[k] for k in sorted(names, key=int)]
    if isinstance(names, list):
        return names
    raise ValueError("data.yaml: champ 'names' introuvable ou invalide")


def iter_label_files(root: Path):
    for p in root.rglob("*.txt"):
        # On ignore les éventuels README/notes ; les labels YOLO sont sous
        # un dossier 'labels'.
        if "labels" in p.parts:
            yield p


def remap_label_file(path: Path, id_map: dict, drop_unmapped: bool):
    out_lines = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        try:
            src_id = int(parts[0])
        except (ValueError, IndexError):
            continue
        if src_id not in id_map:
            if drop_unmapped:
                continue
            # Ne devrait pas arriver : id_map couvre toutes les classes source.
            continue
        dst_id = id_map[src_id]
        if dst_id is None:  # explicitement supprimé
            continue
        out_lines.append(" ".join([str(dst_id)] + parts[1:]))
    path.write_text("\n".join(out_lines) + ("\n" if out_lines else ""))


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Remappe les classes d'un dataset YOLO (présence ou 14 classes).")
    ap.add_argument("dataset", help="Dossier du dataset (contient data.yaml)")
    ap.add_argument("--presence", action="store_true",
                    help="Mode présence : toutes les classes -> 1 seule (component)")
    ap.add_argument("--map", dest="mapping",
                    help="YAML de mapping {nom_source: nom_cible} vers nos 14 classes")
    ap.add_argument("--drop-unmapped", action="store_true",
                    help="Supprime les boîtes des classes non mappées au lieu de 'other'")
    args = ap.parse_args()

    if args.presence == bool(args.mapping):
        print("ERREUR : choisis EXACTEMENT un mode : --presence OU --map FILE.",
              file=sys.stderr)
        return 2

    root = Path(args.dataset)
    data_yaml_path = root / "data.yaml"
    if not data_yaml_path.exists():
        print(f"ERREUR : {data_yaml_path} introuvable.", file=sys.stderr)
        return 2

    try:
        import yaml  # noqa: F401
    except ImportError:
        print("ERREUR : pyyaml manquant. Fais : pip install pyyaml", file=sys.stderr)
        return 2

    data = load_yaml(data_yaml_path)
    src_names = source_names(data)
    print(f"[remap] {len(src_names)} classes source : {src_names}")

    if args.presence:
        # Toutes les classes -> 0 (component).
        id_map = {i: 0 for i in range(len(src_names))}
        new_names = ["component"]
    else:
        mapping = load_yaml(Path(args.mapping))
        if not isinstance(mapping, dict):
            print("ERREUR : le fichier --map doit être un dict {source: cible}.",
                  file=sys.stderr)
            return 2
        target_index = {name: i for i, name in enumerate(PROJECT_CLASSES)}
        other_id = target_index["other"]
        id_map = {}
        for i, sname in enumerate(src_names):
            tgt = mapping.get(sname)
            if tgt is None:
                id_map[i] = None if args.drop_unmapped else other_id
                if not args.drop_unmapped:
                    print(f"[remap]   '{sname}' non mappé -> other")
                else:
                    print(f"[remap]   '{sname}' non mappé -> supprimé")
            elif tgt not in target_index:
                print(f"ERREUR : cible '{tgt}' (pour '{sname}') hors de nos 14 "
                      f"classes : {PROJECT_CLASSES}", file=sys.stderr)
                return 2
            else:
                id_map[i] = target_index[tgt]
                print(f"[remap]   '{sname}' -> '{tgt}' (id {id_map[i]})")
        new_names = list(PROJECT_CLASSES)

    n_files = 0
    for lf in iter_label_files(root):
        remap_label_file(lf, id_map, args.drop_unmapped)
        n_files += 1

    data["nc"] = len(new_names)
    data["names"] = new_names
    dump_yaml(data_yaml_path, data)

    print(f"[remap] {n_files} fichiers de labels réécrits.")
    print(f"[remap] data.yaml mis à jour : nc={len(new_names)} names={new_names}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
