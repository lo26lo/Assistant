"""Split train/val PAR SESSION — jamais par image.

Deux frames d'une même session de capture sont quasi identiques : un split
aléatoire par image mettrait des quasi-doublons des deux côtés → fuite
train→val et mAP mensongère. On découpe donc au niveau des sessions.

Sortie (dans <workdir>/split/) :
    train.txt / val.txt   — chemins absolus des images (ultralytics retrouve
                            les labels en remplaçant /images/ par /labels/)
    data.yaml             — consommé par YOLO

Cas particulier : une seule session → split contiguë (premiers 1-ratio en
train, fin en val) avec avertissement — mieux que l'aléatoire, mais à
remplacer par de vraies sessions multiples dès que possible.
"""

import json
from dataclasses import dataclass, field
from pathlib import Path

IMG_EXTS = {".jpg", ".jpeg", ".png"}


@dataclass
class SplitResult:
    train_sessions: list[str] = field(default_factory=list)
    val_sessions: list[str] = field(default_factory=list)
    n_train: int = 0
    n_val: int = 0
    data_yaml: Path | None = None
    warnings: list[str] = field(default_factory=list)

    @property
    def val_fraction(self) -> float:
        total = self.n_train + self.n_val
        return self.n_val / total if total else 0.0


def _images(session: Path) -> list[Path]:
    return sorted(p for p in (session / "images").iterdir()
                  if p.suffix.lower() in IMG_EXTS)


def _session_tags(session: Path) -> str:
    """Tag de diversité (éclairage|zoom) depuis manifest.jsonl, sinon ''."""
    manifest = session / "manifest.jsonl"
    if not manifest.exists():
        return ""
    try:
        first = json.loads(manifest.read_text(encoding="utf-8").splitlines()[0])
        return f"{first.get('lighting', '?')}|{first.get('zoom', '?')}"
    except (json.JSONDecodeError, IndexError, OSError):
        return ""


def _pick_val_sessions(sessions: list[Path], val_ratio: float,
                       log=print) -> tuple[list[Path], list[Path], list[str]]:
    """Choisit les sessions de validation : plus petites d'abord (approche le
    ratio en sacrifiant le moins de données train), en évitant si possible de
    mettre en val un tag (éclairage|zoom) absent du train."""
    warnings = []
    counts = {s: len(_images(s)) for s in sessions}
    total = sum(counts.values())
    target = total * val_ratio

    by_size = sorted(sessions, key=lambda s: counts[s])
    val: list[Path] = []
    val_count = 0
    for s in by_size:
        if len(val) >= len(sessions) - 1:
            break  # toujours >= 1 session en train
        if val_count >= target:
            break
        val.append(s)
        val_count += counts[s]

    if not val:  # target trop petit : prendre quand même la plus petite session
        val = [by_size[0]]

    train = [s for s in sessions if s not in val]

    # Diversité : un tag présent uniquement en val = le train ne le voit jamais
    train_tags = {_session_tags(s) for s in train}
    for s in val:
        tag = _session_tags(s)
        if tag and tag not in train_tags:
            warnings.append(
                f"La combinaison '{tag}' (session {s.name}) n'existe qu'en "
                f"validation — le modèle ne s'entraînera jamais dessus")
    return train, val, warnings


def split_project(sessions: list[Path], out_dir: Path, class_names: list[str],
                  val_ratio: float = 0.2, log=print) -> SplitResult:
    result = SplitResult()
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if not sessions:
        raise ValueError("Aucune session dans le projet — importer d'abord (étape 1)")

    if len(sessions) == 1:
        # Split contiguë au sein de l'unique session
        s = sessions[0]
        imgs = _images(s)
        cut = max(1, int(len(imgs) * (1 - val_ratio)))
        train_imgs, val_imgs = imgs[:cut], imgs[cut:]
        result.train_sessions = [f"{s.name} (frames 0-{cut - 1})"]
        result.val_sessions = [f"{s.name} (frames {cut}+)"]
        msg = ("Une seule session : split contiguë dans la session — risque de "
               "fuite train/val réduit mais réel. Capturer d'autres sessions "
               "dès que possible.")
        result.warnings.append(msg)
        log(f"⚠️  {msg}")
    else:
        train_s, val_s, warns = _pick_val_sessions(sessions, val_ratio, log)
        result.warnings += warns
        for w in warns:
            log(f"⚠️  {w}")
        train_imgs = [p for s in train_s for p in _images(s)]
        val_imgs = [p for s in val_s for p in _images(s)]
        result.train_sessions = [s.name for s in train_s]
        result.val_sessions = [s.name for s in val_s]

    if not val_imgs:
        raise ValueError("Le split ne laisse aucune image en validation — "
                         "augmenter le ratio ou importer plus de sessions")

    result.n_train, result.n_val = len(train_imgs), len(val_imgs)

    (out_dir / "train.txt").write_text(
        "\n".join(str(p.resolve()) for p in train_imgs) + "\n", encoding="utf-8")
    (out_dir / "val.txt").write_text(
        "\n".join(str(p.resolve()) for p in val_imgs) + "\n", encoding="utf-8")

    names_block = "\n".join(f"  {i}: {n}" for i, n in enumerate(class_names))
    yaml_text = (
        "# Genere par PCB Dataset Studio — split PAR SESSION (pas par image)\n"
        f"# train: {', '.join(result.train_sessions)}\n"
        f"# val:   {', '.join(result.val_sessions)}\n"
        f"path: {out_dir.resolve()}\n"
        "train: train.txt\n"
        "val: val.txt\n"
        f"names:\n{names_block}\n"
    )
    result.data_yaml = out_dir / "data.yaml"
    result.data_yaml.write_text(yaml_text, encoding="utf-8")

    log(f"✂️  Split : {result.n_train} train / {result.n_val} val "
        f"({100 * result.val_fraction:.0f}% val) → {result.data_yaml}")
    return result
