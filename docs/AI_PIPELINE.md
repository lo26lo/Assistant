# Pipeline IA — entraînement, export et déploiement du détecteur de composants

> **Statut** : le câblage applicatif est en place (session 2026-06-10). L'app démarre l'IA
> automatiquement **si** un `.onnx` est présent dans `models/` ; sans modèle elle reste
> 100 % fonctionnelle (overlay/tracking). Ce document décrit comment produire ce modèle.

---

## 1. Comment l'app consomme le modèle

Au démarrage (`Application::initializeAI()`), si `ai.enabled` (défaut `true`) :

1. `ModelManager` scanne `ai.models_path` (défaut `models/`) pour les `.onnx`.
2. Le modèle `ai.detector_model` (défaut **`component_detector`**) est choisi ; à défaut, le premier trouvé.
3. Sur un **thread d'init dédié** (la GUI n'est pas bloquée) : session ONNX Runtime
   → TensorRT EP (FP16, cache d'engine sous `$IBOM_DATA_DIR/tensorrt-cache`).
   **Premier lancement = compilation de l'engine TRT (plusieurs minutes, une seule fois.)**
4. Le signal `Application::aiStatusChanged(ready, message)` informe la GUI ;
   `Application::componentDetector()` retourne le détecteur (ou `nullptr` tant que pas prêt).

Contrat d'interface du modèle (imposé par `InferenceEngine::preprocess/postprocess`) :

| Propriété | Valeur attendue |
|-----------|-----------------|
| Entrée | `[1, 3, 640, 640]` FP32, RGB, normalisé /255, **statique** |
| Sortie | format YOLOv8 : `[1, 4+nc, 8400]` |
| Classes | fichier texte même stem que le .onnx (`component_detector.txt`), un nom par ligne |

Le script `scripts/export_yolov8_onnx.py` produit exactement ça.

---

## 2. Constituer le dataset

**Source d'images** : la fonction snapshot de l'app (`$IBOM_DATA_DIR/snapshots/`) — mêmes
caméra, éclairage et optique que la production = le meilleur dataset possible.

Recommandations de capture :
- 300–800 images pour un premier modèle utilisable ; varier PCB, zoom (adaptateur 0.5×–2×), éclairage, angles légers.
- Inclure des cartes **avec défauts** (composants absents, tombstone, rotation 90/180°) si l'objectif est l'inspection, pas seulement la localisation.
- Garder ~10 % des images de côté (jeu de validation jamais vu à l'entraînement).

**Classes de départ suggérées** (simples à annoter, utiles pour le cross-check iBOM) :

```
resistor, capacitor, inductor, diode, led, transistor_sot,
ic_soic, ic_qfp, ic_qfn, ic_bga, connector, crystal, button, other
```

> Commencer grossier (14 classes max). Affiner plus tard (tailles 0402/0603/0805…) demande
> beaucoup plus de données pour un gain limité — la valeur vient du **croisement avec l'iBOM**
> (position attendue vs détectée), pas de la granularité des classes.

**Annotation** : [Label Studio](https://labelstud.io) (self-hosted, gratuit) ou Roboflow
(hébergé, export YOLO direct). Format cible : YOLO (un `.txt` par image, `class cx cy w h` normalisés).

---

## 3. Entraînement (YOLOv8m — machine avec GPU, pas le Jetson)

```bash
pip install ultralytics

# dataset.yaml : chemins train/val + liste des classes (doc ultralytics)
yolo detect train \
    model=yolov8m.pt \
    data=dataset.yaml \
    imgsz=640 epochs=120 batch=16 \
    degrees=180 fliplr=0.5 flipud=0.5 \
    mosaic=1.0 hsv_v=0.4
```

Notes spécifiques PCB :
- `degrees=180`, `flipud` : un PCB sous microscope n'a pas d'orientation privilégiée.
- `yolov8m` = le compromis retenu au plan de migration (cf `JETSON_MIGRATION.md`) ; sur
  l'Orin en FP16 TensorRT, attendre ~15-25 ms/frame à 640² — largement temps réel pour l'inspection.
- Critère d'acceptation suggéré : mAP50 ≥ 0,85 sur le jeu de validation avant de déployer.

---

## 4. Export et déploiement

```bash
# Sur la machine d'entraînement :
python3 scripts/export_yolov8_onnx.py runs/detect/train/weights/best.pt \
    --out models/component_detector.onnx

# Copier sur le Jetson :
scp models/component_detector.onnx models/component_detector.txt \
    jetson:~/Assistant-git/models/

# Sur le Jetson — relancer l'app :
bash scripts/run_local_gui.sh
# 1er lancement : log "AI pipeline: initializing in background" puis compilation
# TRT (minutes). Lancements suivants : engine rechargé depuis le cache (<1 s).
```

Vérifications dans `logs/pcb_inspector.log` :
- `TensorRT execution provider enabled (device 0), engine cache: …/tensorrt-cache`
- `AI pipeline ready: model 'component_detector' loaded (TensorRT=true)`

Pour désactiver l'IA sans retirer le modèle : `"ai": { "enabled": false }` dans `config.json`.

---

## 5. Suite prévue (hors périmètre de ce document)

| Étape | Description | Réf plan |
|-------|-------------|----------|
| GUI détection | Consommer `aiStatusChanged` + bouton « Detect » dans l'InspectionPanel, surligner les détections sur l'overlay, croiser avec les positions iBOM | — |
| Phase 2d | Preprocess zero-copy (UMA) dans `InferenceEngine` — à faire **avec un modèle réel** pour mesurer | JETSON_MIGRATION.md |
| Phase 3 | DLA + INT8 (calibration sur le dataset PCB) | JETSON_MIGRATION.md |
| OCR / soudure | `OCREngine` (marquages composants) et `SolderInspector` — modèles distincts, même mécanique de déploiement | — |
