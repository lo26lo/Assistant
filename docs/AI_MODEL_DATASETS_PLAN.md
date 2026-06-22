# Plan — Datasets publics & modèle de détection de composants

> **But du document** : décider quoi faire des datasets PCB publics (Roboflow) partagés
> par l'utilisateur, et tracer le chemin concret vers (A) un `ComponentDetector` utilisable
> et (B) un **re-ancrage auto robuste** au niveau composant.
>
> **Complète** (ne remplace pas) :
> - [docs/AI_PIPELINE.md](AI_PIPELINE.md) — comment l'app consomme le `.onnx`, export, entraînement.
> - [docs/DATASET_CREATOR_PLAN.md](DATASET_CREATOR_PLAN.md) — auto-annotation depuis l'iBOM (Phase A).
>
> **Statut** : 🟡 plan, rien d'entraîné. Aucune compilation/entraînement fait ici (pas de toolchain local).

---

## 1. Les deux objectifs (ne pas les confondre)

| | Objectif A — `ComponentDetector` | Objectif B — Re-ancrage auto robuste |
|--|--|--|
| **Sert à** | Overlay/inspection : nommer/typer les composants détectés | Récupérer l'homographie quand le tracking décroche |
| **A besoin de** | Classes **fines** (boîtier) correctes | **Positions** des composants ; la classe = a priori faible |
| **Tolérance taxonomie** | Faible (il faut `ic_qfn` ≠ `ic_soic`) | **Élevée** (un détecteur « présence » suffit) |
| **Meilleure source de données** | `DatasetCreator` (nos cartes, 14 classes, notre caméra) | N'importe quoi de raisonnable, y compris public grossier |
| **Coût pour démarrer** | Élevé (capturer + annoter nos cartes) | **Faible** (un modèle public → ONNX) |

> Conséquence directe : **B est le gain rapide**, A est le travail de fond.

---

## 2. Le problème central : granularité des classes

Notre taxonomie (`resources/footprint_classes.json`, **même ordre** que
`tools/dataset_studio/config/pcb_classes.json` — ne **jamais** réordonner, les ids YOLO en dépendent) :

```
0 resistor      5 transistor_sot   10 connector
1 capacitor     6 ic_soic          11 crystal
2 inductor      7 ic_qfp           12 button
3 diode         8 ic_qfn           13 other
4 led           9 ic_bga
```

Particularité : **les CI sont séparés par boîtier** (`ic_soic/qfp/qfn/bga`). Cette distinction
vient des **footprints KiCad** (règles du JSON), pas de l'apparence. Les datasets publics
ne la font presque jamais → leurs labels ne peuvent pas reconstruire nos 4 classes IC.

**Implication** : un dataset public n'entre dans notre espace 14 classes qu'**après remap**, et
la finesse boîtier reste perdue. Acceptable pour B, pénalisant pour A.

---

## 3. Évaluation des 3 datasets partagés

| Dataset | Type | Verdict | Usage retenu |
|---|---|---|---|
| `roboflow-100/printed-circuit-board` | Benchmark RF100, grossier / niveau carte | ❌ Faible pour nous | Écarter (au mieux pré-entraînement) |
| `pcb-components-tqghw/pcb-component-detection-v2` | Détection composants communautaire | 🟡 Moyen | Détecteur de **présence** (objectif B), repli |
| `marco-filippozzi-siwjn/smd-component-detection` | Centré SMD | ✅ Meilleur des 3 | Pré-entraînement (objectif A), B |

> ⚠️ **Licences** : à vérifier sur chaque page Roboflow **avant de diffuser des poids**.
> RF100 est généralement CC BY 4.0 ; les jeux communautaires varient. (Pages non lisibles
> automatiquement ici : Roboflow renvoie 403 au fetch — à ouvrir manuellement.)

---

## 4. Stratégie recommandée — deux pistes

### Piste B (rapide) — Re-ancrage au niveau composant
1. Récupérer **un** dataset public SMD/composants (Filippozzi en priorité).
2. Entraîner un **YOLOv8n** (petit, rapide sur Orin) en **mode « présence »** :
   toutes classes → fusionnées en 1 ou 2 classes (`component`, éventuellement `ic`).
3. Exporter en ONNX via `scripts/export_yolov8_onnx.py` (contrat d'interface déjà câblé).
4. Côté app : nouvelle stratégie de re-ancrage qui **met en correspondance boîtes détectées ↔
   positions iBOM** (au lieu du `BoardLocator` géométrique, inutile quand la carte remplit le cadre).
   → débloque le D405 gros plan et le microscope zoomé.

### Piste A (fond) — `ComponentDetector` fin (14 classes)
1. **Pré-entraîner** sur le dataset SMD public (remap → nos 14 classes au mieux, le reste en `other`).
2. **Fine-tuner** sur la sortie de `DatasetCreator` (nos cartes, vraies classes, notre caméra/éclairage).
3. Exporter `component_detector.onnx` + `component_detector.txt` → `models/`.
4. L'app le charge automatiquement au démarrage (cf. AI_PIPELINE.md §1).

> Règle d'or : **le public bootstrappe, nos données décident**. La valeur finale vient du
> croisement position iBOM ↔ détection, pas de la finesse des classes publiques.

---

## 5. Outils à écrire (aucun encore fait)

| Outil | Rôle | Statut |
|---|---|---|
| `scripts/fetch_roboflow_dataset.py` | Télécharger un dataset Roboflow (clé API) au format YOLO | ✅ fait (B) |
| `scripts/remap_classes.py` | Remapper les classes : `--presence` (B) ou `--map` → nos 14 (A) | ✅ fait |
| `scripts/class_mapping.example.yaml` | Gabarit de mapping source→14 classes pour `--map` | ✅ fait |
| `scripts/train_yolo.py` | Wrapper d'entraînement Ultralytics (n/s/m, imgsz 640) | ✅ fait |
| `scripts/export_yolov8_onnx.py` | `.pt` → `.onnx` (opset 17, 640, FP32) | ✅ existe |
| `src/overlay/ComponentReanchor.{h,cpp}` | Boîtes détectées ↔ positions iBOM → homographie RANSAC | ✅ fait (B) |
| Câblage app (`componentReanchor()` + timer + menu Dev) | Re-ancrage périodique IA + déclenchement manuel | ✅ fait (B) |
| `scripts/merge_datasets.py` | Fusionner public + sortie `DatasetCreator` (splits propres) | ☐ à faire (A) |

---

## 6. Risques / points de vigilance

- **Licences** des datasets (cf. §3) avant toute diffusion de poids.
- **Domain gap** : éclairage/optique du public ≠ notre caméra → un modèle 100 % public marchera
  moins bien que prévu. D'où le fine-tune sur `DatasetCreator`.
- **Remap imparfait** : les classes IC publiques tomberont souvent en `other` → normal, ne pas
  forcer un mapping faux (`footprint_classes.json` loggue tout composant non classé, jamais en silence).
- **Re-ancrage** : nécessite que des composants soient **visibles et détectés** ; sur une zone
  uniforme (plan de masse) il échouera → garder un fallback (tenir la dernière homographie).

---

## 7. Prochaine décision (utilisateur)

Choisir le débouché à attaquer en premier :
- **Piste B** (re-ancrage rapide, modèle grossier) — recommandé pour débloquer le D405 vite.
- **Piste A** (modèle fin complet) — meilleure qualité overlay, dépend de nos propres captures.

Une fois choisi, je commence par les scripts de la §5 correspondants.
