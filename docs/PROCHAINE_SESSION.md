# Prochaine session — MicroscopeIBOM (au 2026-06-11)

> ⚠️ **DOCUMENT OBSOLÈTE (ère Windows / début de migration) — non tenu à jour.**
> L'état courant du projet vit dans [docs/JETSON_SESSION_LOG.md](../docs/JETSON_SESSION_LOG.md)
> (bloc « État actuel » en tête). Conservé pour l'historique uniquement.

> **Lire ce fichier en premier** pour reprendre sans relire toute l'histoire.
> Mis à jour après chaque session. Lire aussi l'entrée la plus récente de
> [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) pour le détail technique.

---

## État actuel — `main` @ `7a4fe1d`

| Composant | Statut |
|-----------|--------|
| Build Jetson AGX Orin JP6.2 | ✅ 31/31, binaire 1.3 MB |
| ctest 7/7 | ✅ tous passent |
| DatasetCreator (Phase A) | ✅ implémenté + validé |
| DatasetPanel (dock GUI) | ✅ implémenté |
| footprint_classes.json | ✅ 14 classes, 30+ règles |
| Dataset Studio (tools/dataset_studio) | ✅ Lots 1+2 |
| Pipeline IA (InferenceEngine + ComponentDetector) | ✅ câblé, init auto si .onnx présent |
| Calibration caméra | ✅ code existant — **jamais faite sur cette caméra** |
| Première capture dataset réelle | ❌ pas encore |
| Modèle entraîné | ❌ pas encore (besoin dataset d'abord) |

---

## Prochaine action immédiate : calibration + première capture

### 1. Caméra dans le container Docker

`docker/compose.local.yml` : la caméra n'est **pas** mappée en dur (évite erreur #6
si elle n'est pas branchée). Le script `scripts/run_local_gui.sh` génère l'override
dynamique. Vérifier avec :
```bash
ls /dev/video*          # sur l'hôte Jetson
bash scripts/run_local_gui.sh   # lance le container dev avec la caméra
```

### 2. Calibration de la caméra

**Patron** : `resources/calibration_patterns_tiled.pdf` — imprimer sur papier rigide.
Défauts config : 7×5 coins intérieurs, carreaux 5 mm.

**Workflow dans l'app** :
1. Lancer l'app : `./build/bin/MicroscopeIBOM` (dans le container)
2. Démarrer la caméra (bouton Start)
3. Clic **"Calibrate Camera"** dans le Control Panel → mode calibration
4. Bouger le patron sous l'objectif (positions + angles variés)
5. Clic **"Capture Calibration Image"** × **5** fois
6. Calibration automatique → résultat dans la barre de statut
7. Sauvegardé dans `$IBOM_DATA_DIR/calibration.yml` (rechargé au prochain démarrage)

**Critères** : erreur reprojection < 1.0 px = excellent, 1–2 px = acceptable, > 2 px = recommencer.

> ⚠️ Calibrer à la distance/zoom réels d'utilisation. Si tu changes le zoom optique, recalibre.

### 3. Première capture dataset

Prérequis : calibration faite + carte PCB avec son iBOM HTML.

1. Charger l'iBOM (bouton Load iBOM)
2. Aligner la carte (homographie 4 points ou tracking auto)
3. Ouvrir l'onglet **Dataset** (dock gauche, tabifié avec Inspection)
4. Renseigner : nom de carte + tag éclairage (ring / lateral / ambient)
5. Clic **Start capture**
6. Observer les 5 gates (doivent être verts) — si rouge, corriger la cause affichée
7. Bouger lentement la carte sous l'objectif (~15-20 min)
8. Objectif : **300-600 images** pour une première session

**Sortie** : `$IBOM_DATA_DIR/dataset/session_<date>_<board>_<lighting>/`
```
images/  frame_000001.jpg  …
labels/  frame_000001.txt  …   ← YOLO : class cx cy w h normalisés
manifest.jsonl               ← métadonnées par frame (inliers, blur, H, tags)
```

**Après la capture** : vérifier les logs pour les footprints non mappés
(loggés en `[warning] ClassMapper: no rule for footprint '…'`).
Les ajouter dans `resources/footprint_classes.json` si nécessaire.

---

## Ce qui reste à implémenter (par ordre de priorité)

### A. Overlay preview en live (Phase A §A5) — non implémenté

**Quoi** : pendant la capture, dessiner les boxes YOLO projetées sur le flux caméra
(dans CameraView), pour que l'utilisateur voie visuellement si les labels collent.
Si ça glisse → réaligner avant de continuer.

**Où coder** :
- `DatasetCreator` émet déjà les labels via `statusUpdated(DatasetStatus)`.
  Ajouter un signal `labelsReady(std::vector<YoloLabel>, cv::Size)` ou passer les
  boxes dans `DatasetStatus`.
- `Application` : connecter ce signal à `CameraView::setDatasetOverlay(…)`.
- `CameraView` : dessiner les rectangles en overlay (couleur distincte de l'iBOM,
  ex. cyan semi-transparent), activés seulement quand la session est active.

**Effort estimé** : ½ session.

---

### B. Phase B — Assistant de variété

**Quoi** : guider l'utilisateur pour diversifier le dataset (pas juste du volume).

**Composants à créer** :
- **Carte de couverture** : grille sur le plan PCB (réutilise `HeatmapRenderer`)
  montrant les zones capturées + niveaux de zoom → l'utilisateur voit où passer.
- **Check-list** dans `DatasetPanel` :
  - ☐ Zoom ×0.5  ☐ ×1  ☐ ×2
  - ☐ Éclairage annulaire  ☐ latéral  ☐ faible
  - ☐ Face avant  ☐ arrière
  - ☐ Légères rotations (±15°)
- **Quotas** : objectif d'images par combinaison (zoom × éclairage), compteurs visibles.

Le `manifest.jsonl` enregistre déjà le tag éclairage — ajouter le zoom (ratio
`homography scale` déjà calculé dans l'app via `m_currentPixelsPerMm`).

**Effort estimé** : 1 session de dev.

---

### C. Scripts standalone Python (Phase C)

> Note : le Studio (`tools/dataset_studio`) couvre déjà `session_split.py`
> (split par session) et `validation.py`. Ces scripts standalone ciblent un
> usage CLI direct sans passer par le Studio.

À créer dans `scripts/dataset_tools/` :

| Script | Quoi |
|--------|------|
| `validate_dataset.py` | Mosaïque HTML/PNG avec boxes dessinées sur N images aléatoires, stats par classe (count, taille médiane), anomalies (boxes hors image, classes vides) |
| `split_dataset.py` | Split train/val **par session** (jamais par image — fuite train→val sinon), génère `dataset.yaml` |
| `review_sample.py` | Exporte 5-10 % des images vers Label Studio pour mesurer le taux d'erreur de l'auto-annotation (objectif < 2-3 % de boxes fausses) |

**Effort estimé** : ½ session.

---

### D. Entraînement v1

**Sur le PC Ubuntu RTX 5070 Ti** (ou Windows avec même GPU) :
```bash
cd tools/dataset_studio
./install.sh && ./start.sh
# Étape 3 : split par session → dataset.yaml
# Étape 4 : entraîner YOLOv8m
```

Ou en ligne de commande :
```bash
cd tools/dataset_studio
source .venv/bin/activate
python -m studio.gpu_check           # vérifier sm_120 dans get_arch_list
yolo train model=yolov8m.pt data=dataset.yaml epochs=100 imgsz=640 batch=16
```

**Exporter en ONNX pour Jetson** :
```bash
python scripts/export_yolov8_onnx.py --weights runs/detect/train/weights/best.pt \
    --output models/component_detector.onnx --opset 17
```

**Déployer sur le Jetson** :
```bash
scp models/component_detector.onnx jetson:~/ibom-data/models/
# L'app charge automatiquement si ai.enabled=true dans la config
# et models/component_detector.onnx est présent dans $IBOM_DATA_DIR/models/
```

Voir [docs/AI_PIPELINE.md](AI_PIPELINE.md) pour tous les détails.

---

### E. Phase 2d — InferenceEngine zero-copy preprocess

**Quoi** : le resize/pad avant l'inférence ONNX alloue actuellement un `cv::Mat`
intermédiaire. Avec l'UMA (cudaMallocManaged), on peut éviter la copie
host→device en faisant le resize directement en CUDA.

**À faire seulement quand un `.onnx` réel est disponible** pour mesurer le gain.
Fichier : `src/ai/InferenceEngine.cpp` — chercher `preprocess`.

---

### F. Affichage détections live dans CameraView

**Quoi** : une fois un modèle chargé, dessiner les boxes de détection sur le flux
caméra (en plus de l'overlay iBOM). `ComponentDetector` émet déjà `detectionsReady`.
À connecter à `CameraView` (signal/slot existant à vérifier).

---

### G. Phase D — Hard-example mining

**Quoi** : quand le modèle v1 tourne en live, comparer ses détections avec les
projections iBOM. Les frames où ils **divergent** (composant manqué, classe fausse)
sont les plus utiles pour le dataset v2. Marquer ces frames `priority: hard` dans
`manifest.jsonl`.

**Dépend de** : modèle v1 entraîné + détections live (F ci-dessus).

---

## Infrastructure / plomberie restante

| Tâche | Détail |
|-------|--------|
| Écran tactile Minix SF16T | Brancher en USB-C, règle udev sur hôte, tester X11 (`xhost +local:docker`) |
| `nsys profile` | Valider <1 ms copies host↔device sur une frame (critère Phase 2) |
| Phase 2.5 V4L2 DMABUF | Gros morceau optionnel — capture caméra zero-copy sans cv::VideoCapture. À ne faire que si la caméra supporte DMABUF et que le profiling montre un goulot. |
| Phase 3 DLA INT8 | Optionnel — quantification ComponentDetector pour le DLA Jetson. Nécessite un calibration dataset ONNX. |

---

## Contexte technique clé à garder en tête

### Fichiers critiques
| Fichier | Rôle |
|---------|------|
| `src/features/DatasetCreator.h/.cpp` | Cœur Phase A — gates, projection bboxes, écriture YOLO |
| `src/gui/DatasetPanel.h/.cpp` | UI onglet Dataset |
| `resources/footprint_classes.json` | Mapping footprint/ref → classe YOLO. **Doit rester synchronisé** avec `tools/dataset_studio/config/pcb_classes.json` (même ordre = mêmes IDs de classe) |
| `src/overlay/TrackingWorker.h` | Signal `homographyUpdated(cv::Mat, int inliers, double reprojErrPx)` |
| `src/app/Config.h/.cpp` | Section `dataset.*` — 10 seuils configurables |
| `scripts/build_jetson.sh` | Build Release dans le container dev |
| `docker/run-dev.sh` | Lance le container dev avec GPU + display + devices |

### Pièges à ne pas réintroduire
- **ONNX n'est jamais optionnel** — pas de build no-GPU, pas de fallback CPU silencieux.
- **Split dataset par session, jamais par image** — deux frames de la même session sont quasi identiques → fuite train→val.
- **`footprint_classes.json` et `pcb_classes.json` doivent rester dans le même ordre** — les IDs de classe YOLO dépendent de l'ordre.
- **Qt metatypes** : types pleinement qualifiés dans `Q_DECLARE_METATYPE` + `qRegisterMetaType` (piège CLAUDE.md #17).
- **Docker sur Jetson** : toujours `--network host` (kernel Tegra 5.15 sans `iptable_raw` — erreur #3).
- **Git dans le container** : `git config --global --add safe.directory /opt/microscope-ibom` (ownership root vs user).

### Commandes de reprise sur le Jetson
```bash
# 1. Entrer dans le container dev
bash docker/run-dev.sh            # ou : bash scripts/run_local_gui.sh

# 2. Pull + build
git config --global --add safe.directory /opt/microscope-ibom
git pull origin main
bash scripts/build_jetson.sh

# 3. Tests
cd build && ctest --output-on-failure

# 4. Lancer l'app
./build/bin/MicroscopeIBOM
```

### Variables d'environnement importantes
| Variable | Valeur typique Jetson | Rôle |
|----------|----------------------|------|
| `IBOM_DATA_DIR` | `/home/user/ibom-data` | Racine unifiée données (calibration, dataset, models, snapshots) |
| `DISPLAY` | `:0` | Affiché par `run-dev.sh` |
| `QT_QPA_PLATFORM` | `xcb` | Forcé dans le container |

---

## Résumé en 3 lignes

**Fait** : Phase A complète (DatasetCreator + Studio Lots 1+2), build + 7/7 ctest validés sur Jetson, dans `main`.

**Prochain acte** : calibration caméra → première capture réelle (300-600 images annotées auto) → entraînement v1 sur PC Ubuntu/Windows.

**Après le premier modèle** : overlay preview live, hard-example mining, Phase D boucle d'amélioration continue.
