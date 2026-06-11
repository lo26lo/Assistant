# =============================================================================
#  Vendorise depuis Pokemon-Dataset-Creator (github.com/lo26lo/Pokemon-Dataset-Creator)
#  core/training_manager.py — gestionnaire d'entrainement YOLO generique.
#
#  Adaptations PCB (marquees [PCB] dans le code) :
#    1. Defauts TrainingConfig : nom pcb_detector, rotations 180° + flip
#       vertical actives (un PCB sous microscope n'a pas d'orientation
#       privilegiee), modele yolov8m.
#    2. Callback ultralytics on_fit_epoch_end → progression par epoch
#       remontee dans le log GUI.
# =============================================================================
"""
Training Manager - Gestion de l'entraînement YOLO
==================================================
"""

import sys
from pathlib import Path

try:
    from .utils import safe_print
except ImportError:
    from utils import safe_print

from typing import Optional, Callable, Dict
from dataclasses import dataclass
import logging

logger = logging.getLogger(__name__)


@dataclass
class TrainingConfig:
    """Configuration pour l'entraînement YOLO"""
    # Modèle de base
    model_name: str = "yolov8m.pt"  # [PCB] n, s, m, l, x

    # Hyperparamètres
    epochs: int = 120               # [PCB]
    batch_size: int = 16
    image_size: int = 640
    device: str = "0"  # "0", "cpu", "0,1,2,3"
    workers: int = 4
    cache: str = "False"  # False, ram, disk

    # Chemins
    data_yaml: Path = Path("split/data.yaml")
    project_dir: Path = Path("runs/train")
    name: str = "pcb_detector"      # [PCB]

    # Options avancées
    patience: int = 50
    save_period: int = -1
    exist_ok: bool = True
    pretrained: bool = True
    optimizer: str = "SGD"
    lr0: float = 0.01
    lrf: float = 0.01
    cos_lr: bool = False

    # Augmentation — [PCB] un PCB n'a pas d'orientation privilégiée
    augment: bool = True
    hsv_h: float = 0.015
    hsv_s: float = 0.7
    hsv_v: float = 0.4
    degrees: float = 180.0          # [PCB] était 0.0
    translate: float = 0.1
    scale: float = 0.5
    shear: float = 0.0
    perspective: float = 0.0
    flipud: float = 0.5             # [PCB] était 0.0
    fliplr: float = 0.5
    mosaic: float = 1.0
    mixup: float = 0.0

    def __post_init__(self):
        """Validation après initialisation"""
        if self.epochs < 1:
            raise ValueError("epochs doit être >= 1")
        if self.batch_size < 1:
            raise ValueError("batch_size doit être >= 1")
        if not Path(self.data_yaml).exists():
            raise FileNotFoundError(f"data.yaml non trouvé: {self.data_yaml}")


class TrainingManager:
    """
    Gestionnaire d'entraînement YOLO

    Exemple:
        >>> config = TrainingConfig(epochs=100, batch_size=32)
        >>> manager = TrainingManager(config)
        >>> manager.set_log_callback(lambda msg: print(msg))
        >>> success = manager.train()
    """

    def __init__(self, config: TrainingConfig):
        self.config = config
        self._log_callback: Optional[Callable[[str], None]] = None
        self._model = None
        self._results = None

    def set_log_callback(self, callback: Callable[[str], None]) -> None:
        self._log_callback = callback

    def _log(self, message: str) -> None:
        logger.info(message)
        if self._log_callback:
            self._log_callback(message)

    def train(self) -> bool:
        """Lance l'entraînement. True si succès."""
        try:
            from ultralytics import YOLO

            self._log("🎓 Démarrage de l'entraînement YOLO")
            self._log(f"   Modèle: {self.config.model_name}")
            self._log(f"   Epochs: {self.config.epochs}")
            self._log(f"   Batch: {self.config.batch_size}")
            self._log(f"   Image Size: {self.config.image_size}")
            self._log(f"   Device: {self.config.device}")
            self._log(f"   Data: {self.config.data_yaml}")

            self._model = YOLO(self.config.model_name)

            # [PCB] progression par epoch dans le log GUI
            def _on_epoch_end(trainer):
                m = getattr(trainer, "metrics", None) or {}
                self._log(
                    f"   epoch {getattr(trainer, 'epoch', 0) + 1}"
                    f"/{getattr(trainer, 'epochs', self.config.epochs)} — "
                    f"mAP50={m.get('metrics/mAP50(B)', 0):.3f} "
                    f"mAP50-95={m.get('metrics/mAP50-95(B)', 0):.3f}")
            try:
                self._model.add_callback("on_fit_epoch_end", _on_epoch_end)
            except Exception:
                pass  # vieux ultralytics sans add_callback : log final seulement

            self._results = self._model.train(
                data=str(self.config.data_yaml),
                epochs=self.config.epochs,
                imgsz=self.config.image_size,
                batch=self.config.batch_size,
                device=self.config.device,
                workers=self.config.workers,
                cache=self.config.cache if self.config.cache != "False" else False,
                patience=self.config.patience,
                save=True,
                save_period=self.config.save_period,
                project=str(self.config.project_dir),
                name=self.config.name,
                exist_ok=self.config.exist_ok,
                pretrained=self.config.pretrained,
                optimizer=self.config.optimizer,
                lr0=self.config.lr0,
                lrf=self.config.lrf,
                cos_lr=self.config.cos_lr,
                augment=self.config.augment,
                hsv_h=self.config.hsv_h,
                hsv_s=self.config.hsv_s,
                hsv_v=self.config.hsv_v,
                degrees=self.config.degrees,
                translate=self.config.translate,
                scale=self.config.scale,
                shear=self.config.shear,
                perspective=self.config.perspective,
                flipud=self.config.flipud,
                fliplr=self.config.fliplr,
                mosaic=self.config.mosaic,
                mixup=self.config.mixup,
                verbose=True
            )

            best_path = self.get_best_model_path()
            self._log("✅ Entraînement terminé!")
            self._log(f"📊 Modèle sauvegardé: {best_path}")

            metrics = self.get_metrics()
            if metrics:
                self._log("📈 Métriques finales:")
                self._log(f"   mAP50: {metrics.get('mAP50', 0):.3f}")
                self._log(f"   mAP50-95: {metrics.get('mAP50-95', 0):.3f}")
                self._log(f"   Precision: {metrics.get('precision', 0):.3f}")
                self._log(f"   Recall: {metrics.get('recall', 0):.3f}")

            return True

        except ImportError:
            self._log("❌ Package ultralytics non installé!")
            self._log("   Lancer install_training.bat (torch cu128 + ultralytics)")
            return False

        except Exception as e:
            self._log(f"❌ Erreur lors de l'entraînement: {e}")
            import traceback
            self._log(traceback.format_exc())
            return False

    def get_best_model_path(self) -> Path:
        return Path(self.config.project_dir) / self.config.name / "weights" / "best.pt"

    def get_last_model_path(self) -> Path:
        return Path(self.config.project_dir) / self.config.name / "weights" / "last.pt"

    def get_metrics(self) -> Optional[Dict[str, float]]:
        if self._results is None:
            return None
        try:
            metrics = {}
            if hasattr(self._results, 'results_dict'):
                rd = self._results.results_dict
                metrics['mAP50'] = rd.get('metrics/mAP50(B)', 0)
                metrics['mAP50-95'] = rd.get('metrics/mAP50-95(B)', 0)
                metrics['precision'] = rd.get('metrics/precision(B)', 0)
                metrics['recall'] = rd.get('metrics/recall(B)', 0)
            return metrics if metrics else None
        except Exception as e:
            self._log(f"⚠️ Impossible de récupérer les métriques: {e}")
            return None

    def validate(self, model_path: Optional[Path] = None) -> Optional[Dict[str, float]]:
        """Valide un modèle sur le set de validation."""
        try:
            from ultralytics import YOLO

            if model_path is None:
                model_path = self.get_best_model_path()
            if not model_path.exists():
                self._log(f"❌ Modèle non trouvé: {model_path}")
                return None

            self._log(f"🔍 Validation du modèle: {model_path}")
            model = YOLO(str(model_path))
            results = model.val(data=str(self.config.data_yaml))
            metrics = {
                'mAP50': results.box.map50,
                'mAP50-95': results.box.map,
                'precision': results.box.mp,
                'recall': results.box.mr,
            }
            self._log(f"✅ Validation: mAP50={metrics['mAP50']:.3f} "
                      f"mAP50-95={metrics['mAP50-95']:.3f}")
            return metrics
        except Exception as e:
            self._log(f"❌ Erreur validation: {e}")
            return None

    def export_model(self, model_path: Optional[Path] = None,
                     format: str = "onnx") -> Optional[Path]:
        """Exporte le modèle (onnx, torchscript, …). Utilisé au Lot 3."""
        try:
            from ultralytics import YOLO

            if model_path is None:
                model_path = self.get_best_model_path()
            if not model_path.exists():
                self._log(f"❌ Modèle non trouvé: {model_path}")
                return None

            self._log(f"📦 Export du modèle vers {format}...")
            model = YOLO(str(model_path))
            export_path = model.export(format=format)
            self._log(f"✅ Modèle exporté: {export_path}")
            return Path(export_path)
        except Exception as e:
            self._log(f"❌ Erreur export: {e}")
            return None
