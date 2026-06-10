"""Import de sessions de capture dans le projet.

Lot 1 : import depuis un dossier local (copie). L'import scp direct depuis le
Jetson arrive au Lot 3 — en attendant : scp/clé USB vers un dossier local,
puis import ici.

Une "session" = un dossier contenant images/ et labels/ (layout produit par la
Phase A côté Jetson, cf docs/DATASET_CREATOR_PLAN.md). Le dossier source peut
être une session unique ou un parent contenant plusieurs sessions.
"""

import shutil
from dataclasses import dataclass
from pathlib import Path

IMG_EXTS = {".jpg", ".jpeg", ".png"}


@dataclass
class SessionInfo:
    path: Path
    name: str
    n_images: int
    n_labels: int

    @property
    def summary(self) -> str:
        return f"{self.name}: {self.n_images} images, {self.n_labels} labels"


def _count(session: Path) -> SessionInfo:
    images = [p for p in (session / "images").iterdir()
              if p.suffix.lower() in IMG_EXTS]
    labels = list((session / "labels").glob("*.txt"))
    return SessionInfo(session, session.name, len(images), len(labels))


def scan_source(source: Path) -> list[SessionInfo]:
    """Liste les sessions trouvées dans `source` (lui-même ou ses enfants)."""
    source = Path(source)
    if not source.is_dir():
        return []
    if (source / "images").is_dir() and (source / "labels").is_dir():
        return [_count(source)]
    found = []
    for child in sorted(source.iterdir()):
        if child.is_dir() and (child / "images").is_dir() and (child / "labels").is_dir():
            found.append(_count(child))
    return found


def import_sessions(sessions: list[SessionInfo], dataset_dir: Path,
                    log=print) -> list[SessionInfo]:
    """Copie les sessions dans le projet. Une session déjà présente (même nom)
    est ignorée — pas d'écrasement silencieux."""
    dataset_dir.mkdir(parents=True, exist_ok=True)
    imported = []
    for s in sessions:
        dest = dataset_dir / s.name
        if dest.exists():
            log(f"⚠️  Session '{s.name}' déjà dans le projet — ignorée "
                f"(supprimer le dossier pour réimporter)")
            continue
        log(f"📥 Import de {s.summary} …")
        shutil.copytree(s.path, dest)
        imported.append(_count(dest))
    return imported
