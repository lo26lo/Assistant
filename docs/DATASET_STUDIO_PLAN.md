# Plan — PCB Dataset Studio (wizard Windows d'entraînement)

> **Date** : 2026-06-10 · **Statut** : PLAN validé à faire approuver — rien d'implémenté
> **Emplacement décidé** : `tools/dataset_studio/` (repo Assistant)
> **Machine cible** : PC fixe Windows + RTX 5070 Ti 16 GB (portable 5070 = plan B)
> **Base réutilisée** : [Pokemon-Dataset-Creator](https://github.com/lo26lo/Pokemon-Dataset-Creator) (analysé en détail le 2026-06-10)
> **Documents liés** : [DATASET_CREATOR_PLAN.md](DATASET_CREATOR_PLAN.md) (capture côté Jetson), [AI_PIPELINE.md](AI_PIPELINE.md)

---

## 1. Rôle dans la chaîne complète

```
JETSON (Phase A, à venir)                WINDOWS — PCB Dataset Studio              JETSON
┌──────────────────────┐    scp/rsync   ┌──────────────────────────────┐   scp   ┌─────────────┐
│ capture + annotation  │ ─────────────▶ │ 1 Import   2 Validation       │ ──────▶ │ models/*.onnx│
│ auto (iBOM+homographie)│               │ 3 Split    4 Entraînement     │         │ → TensorRT   │
│ → sessions YOLO       │               │ 5 Test     6 Export ONNX      │         │   (auto)     │
└──────────────────────┘               └──────────────────────────────┘         └─────────────┘
```

Le Studio est **indépendant de la Phase A** : il fonctionne avec n'importe quel dossier `images/ + labels/` YOLO. On peut donc le développer **maintenant**, avant la capture Jetson, et le tester avec un mini-dataset annoté à la main (ou des données publiques type FPIC) — il sera prêt quand la Phase A produira ses sessions.

---

## 2. Inventaire Pokemon-Dataset-Creator (analyse du code réel)

### ✅ Réutilisé quasi tel quel (modules génériques YOLO, copiés avec en-tête d'attribution)

| Module source | Lignes | Rôle | Adaptation |
|---|---|---|---|
| `core/training_manager.py` | 412 | `TrainingConfig` (dataclass complète : modèle/epochs/batch/lr/augments) + `TrainingManager` (API ultralytics, callback de log pour GUI, métriques) | défauts PCB : `yolov8m.pt`, `name=pcb_detector`, `degrees=180`, `flipud=0.5`, `fliplr=0.5` |
| `core/dataset_validator.py` | 507 | validation YOLO complète (images corrompues, labels manquants, annotations invalides) + **rapport HTML** | aucune (générique) |
| `core/auto_balancer_optimized.py` | 356 | équilibrage de classes par augmentation ciblée (multi-thread) | vérifier l'ajustement bbox au flip (générique) |
| `core/dataset_exporter.py` | 358 | export COCO / VOC / ZIP Roboflow | aucune (optionnel, étape 6) |
| `core/utils.py` | 503 | `safe_print` (encodage Windows), helpers | extraire le nécessaire |
| `INSTALL.bat` / `START.bat` / `install_env.bat` | — | venv + requirements + lancement Windows | adapter chemins/nom |

### ❌ Écarté (spécifique Pokémon)

`image_downloader.py`, `tcgdex_api.py`, `card_mapping.py`, `detection_with_prices.py`, `holographic_augmenter_optimized.py`, `mosaic_optimized.py` (compositing de cartes — notre équivalent est la projection iBOM côté Jetson), `excel/`, `backgrounds/`.

### 🔁 Réécrit (pas adapté)

- **GUI** : `GUI_v3.1_modern.py` fait 8 642 lignes, profondément lié au workflow cartes. On garde **l'UX** (wizard par étapes, thème sombre, log live en pied de page, config JSON persistée) mais on réécrit un wizard compact (~800 lignes Tkinter, zéro dépendance GUI supplémentaire).
- **Split train/val** : le split Pokemon est aléatoire par image → **interdit ici** (frames d'une même session quasi identiques = fuite train→val). On écrit `session_split.py` (split **par session de capture**, stratifié par tags zoom/éclairage du `manifest.jsonl`).

---

## 3. Le wizard — 6 étapes

| # | Étape | Contenu | Module |
|---|-------|---------|--------|
| 0 | **Projet** | dossier de travail, IP/user du Jetson, classes (`footprint_classes.json` importé du repo) | `studio/project.py` |
| 1 | **Import** | A) `scp -r jetson:$IBOM_DATA_DIR/dataset/ …` (OpenSSH intégré à Win10/11, via subprocess) ou B) dossier local choisi à la main. Fusion multi-sessions, lecture des `manifest.jsonl` | `studio/import_manager.py` |
| 2 | **Validation** | `DatasetValidator` + rapport HTML + mosaïque d'aperçus avec bboxes dessinées + stats par classe/session ; liste des classes vides/rares | repris |
| 3 | **Split & équilibrage** | split **par session** → `data.yaml` ; équilibrage optionnel (`auto_balancer`) avec plafonnement des classes majoritaires | `studio/session_split.py` + repris |
| 4 | **Entraînement** | presets (Rapide=yolov8n/50ep · Standard=yolov8m/120ep · Précis=yolov8m/300ep+patience), log live (callback), courbes de métriques, early stopping ; **GPU check au lancement** (cf. §5 piège Blackwell) | repris (`training_manager`) |
| 5 | **Test & export** | détection sur le val set avec aperçu visuel ; si OK → `scripts/export_yolov8_onnx.py` (déjà dans le repo) + `.txt` classes → **bouton "Déployer sur le Jetson"** (scp vers `models/`) | `studio/deploy.py` |

Chaque étape a un indicateur ✅/⚠️/❌ et le wizard mémorise l'état (reprendre où on s'était arrêté).

---

## 4. Arborescence cible

```
tools/dataset_studio/
  app.py                      # wizard Tkinter (~800 lignes, 6 étapes)
  studio/
    project.py                # config projet (JSON persisté)
    import_manager.py         # scp/rsync Jetson + fusion sessions + manifest
    session_split.py          # split par session + data.yaml
    deploy.py                 # export ONNX + scp vers Jetson
    vendor/                   # modules repris de Pokemon-Dataset-Creator
      training_manager.py     #   (en-tête d'attribution + adaptations PCB)
      dataset_validator.py
      auto_balancer.py
      dataset_exporter.py
      utils.py
  config/
    defaults.json             # presets d'entraînement, chemins
    pcb_classes.json          # classes (synchronisé avec footprint_classes.json)
  requirements.txt            # ultralytics, opencv-python, pillow, imgaug (balancer)
  INSTALL.bat                 # venv + pip install (adapté de Pokemon)
  START.bat                   # lancement
  README.md                   # quickstart Windows
```

---

## 5. Pièges identifiés en amont

1. **🔴 RTX 5070 Ti = Blackwell (sm_120)** : exige **PyTorch ≥ 2.7 avec CUDA 12.8** (`pip install torch --index-url https://download.pytorch.org/whl/cu128`). Un `pip install torch` standard plus ancien → "no kernel image available". L'INSTALL.bat doit imposer la bonne roue, et l'app fait un check `torch.cuda.get_device_capability()` au démarrage avec message clair.
2. **Split par image interdit** (fuite train/val entre frames d'une même session) — d'où `session_split.py`, jamais le split aléatoire de Pokemon.
3. **Encodage console Windows** : déjà résolu chez Pokemon (`safe_print`) — on reprend.
4. **scp Windows** : OpenSSH client intégré depuis Win10 ; prévoir le fallback "dossier local" si pas de réseau vers le Jetson (clé USB).
5. **Classes** : `pcb_classes.json` doit rester la **même liste ordonnée** que ce que produira la Phase A (sinon ids YOLO incohérents). Source de vérité unique = `resources/footprint_classes.json` du repo (créé en Phase A ; d'ici là le Studio embarque la liste du DATASET_CREATOR_PLAN §A4).

---

## 6. Lots d'implémentation (chacun livrable/testable seul)

| Lot | Contenu | Testable par | Effort |
|-----|---------|--------------|--------|
| **1** | Squelette wizard + Projet + Import (dossier local) + Validation (validator + aperçus bbox) | n'importe quel dataset YOLO de test | 1 session |
| **2** | Split par session + data.yaml + Entraînement (presets, log live) + check GPU Blackwell | mini-dataset → entraînement yolov8n 5 epochs sur le PC fixe | 1 session |
| **3** | Équilibrage + Test visuel + Export ONNX + Déploiement scp Jetson + import scp Jetson | bout-en-bout avec le Jetson | 1 session |

Le développement se fait dans ce repo (je n'ai pas de Windows pour exécuter) → **chaque lot se conclut par un test par l'utilisateur sur le PC fixe**, retours, corrections.

---

## 7. Questions ouvertes (avant Lot 1)

1. **Transfert Jetson** : scp automatique intégré (recommandé) — OK ? (nécessite SSH activé sur le Jetson, user/IP dans la config)
2. **Langue UI** : français (recommandé, comme Pokemon) ?
3. Un mini-dataset de test existe-t-il déjà (quelques images PCB annotées), ou le Lot 1 doit-il inclure un générateur de dataset factice pour les tests à blanc ?
