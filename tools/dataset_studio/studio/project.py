"""Projet PCB Dataset Studio — configuration persistée en JSON.

Le projet pointe vers un dossier de travail (workdir) qui contient :
    dataset/<session>/images/*.jpg + labels/*.txt   (sessions importées)
    reports/                                        (rapports de validation)
    runs/                                           (entraînements, Lot 2)
"""

import json
from pathlib import Path

CONFIG_DIR = Path.home() / ".pcb_dataset_studio"
CONFIG_FILE = CONFIG_DIR / "project.json"

DEFAULTS = {
    "workdir": str(Path.home() / "PCB-Dataset"),
    "jetson_host": "",          # ex: 192.168.1.42 (import scp — Lot 3)
    "jetson_user": "lololo",
    "jetson_data_dir": "/opt/microscope-ibom/data",
    "last_step": 0,
}


class Project:
    def __init__(self):
        self.data = dict(DEFAULTS)
        self.load()

    def load(self):
        if CONFIG_FILE.exists():
            try:
                self.data.update(json.loads(CONFIG_FILE.read_text(encoding="utf-8")))
            except (json.JSONDecodeError, OSError):
                pass  # config corrompue → on repart des défauts

    def save(self):
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(
            json.dumps(self.data, indent=2, ensure_ascii=False), encoding="utf-8")

    # --- accès typés -------------------------------------------------------
    @property
    def workdir(self) -> Path:
        return Path(self.data["workdir"])

    @property
    def dataset_dir(self) -> Path:
        return self.workdir / "dataset"

    @property
    def reports_dir(self) -> Path:
        return self.workdir / "reports"

    def ensure_dirs(self):
        self.dataset_dir.mkdir(parents=True, exist_ok=True)
        self.reports_dir.mkdir(parents=True, exist_ok=True)

    def sessions(self) -> list[Path]:
        """Sessions présentes dans le projet (dossiers avec images/ + labels/)."""
        if not self.dataset_dir.exists():
            return []
        return sorted(
            p for p in self.dataset_dir.iterdir()
            if p.is_dir() and (p / "images").is_dir() and (p / "labels").is_dir()
        )
