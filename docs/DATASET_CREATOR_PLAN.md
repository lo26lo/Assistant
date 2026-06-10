# Plan — Dataset Creator : capture & annotation automatiques pour le détecteur de composants

> **Date** : 2026-06-10 · **Statut** : PLAN (rien d'implémenté)
> **Objectif** : produire 800+ images annotées **en quelques sessions de manipulation**, sans annotation manuelle, en réutilisant ce que MicroscopeIBOM sait déjà faire — et en s'inspirant du workflow de [Pokemon-Dataset-Creator](https://github.com/lo26lo/Pokemon-Dataset-Creator) (cf. §8).
> **Document lié** : [AI_PIPELINE.md](AI_PIPELINE.md) (consommation du modèle entraîné).

---

## 1. L'idée centrale : l'iBOM est une vérité terrain gratuite

Annoter 800 images × ~100 composants à la main = ~2 semaines de travail. **On n'en a pas besoin** : l'application connaît déjà, à chaque instant où le tracking est verrouillé :

- la **position et la bbox de chaque composant** en coordonnées PCB (`ComponentMap::allComponents()` → `Component{reference, value, footprint, layer, bbox}`) ;
- la **transformation PCB → image caméra** (`Homography::transformRect()` / `pcbToImage()`, maintenue en live par le TrackingWorker ORB+RANSAC).

Donc : **chaque frame où le tracking est bon = une image annotée gratuite** (projection des bboxes iBOM → labels YOLO). Une session de 20 minutes à bouger la carte sous la caméra produit des centaines d'images parfaitement annotées. C'est exactement le même principe que Pokemon-Dataset-Creator (génération automatique de labels à partir d'une connaissance a priori), mais avec une vérité terrain encore plus forte.

```
   iBOM (positions exactes)          frame caméra
            │                              │
            └──── Homographie (live) ──────┘
                        │
            bboxes projetées dans l'image
                        │
        gates qualité (netteté, inliers, …)
                        │
            images/ + labels/ (YOLO)
```

---

## 2. Architecture retenue

**Mode "Dataset Capture" intégré à l'app C++** (pas un outil séparé) — parce que l'homographie live, les gates qualité et le retour visuel immédiat n'existent que là. L'outillage périphérique (validation, split, augmentation) reste en Python.

```
src/features/DatasetCreator.{h,cpp}     # logique : gates, projection, écriture YOLO
src/gui/DatasetPanel.{h,cpp}            # UI : start/stop, compteurs, état des gates
scripts/dataset_tools/
    validate_dataset.py                 # relit images+labels, dessine les boxes, stats
    split_dataset.py                    # train/val (par session, pas par image !)
    review_sample.py                    # export d'un échantillon vers Label Studio
    augment_offline.py                  # augmentations hors-ligne (optionnel)
resources/footprint_classes.json        # règles de mapping footprint/ref → classe
```

Sortie sur disque (sous `$IBOM_DATA_DIR/dataset/`) :

```
dataset/
  session_2026-06-12_carteX_ledring/    # 1 dossier par session de capture
    images/  frame_000123.jpg
    labels/  frame_000123.txt           # YOLO: class cx cy w h (normalisés)
    manifest.jsonl                      # 1 ligne/frame: homographie, inliers, blur,
                                        #   tag éclairage, zoom, board_id
  dataset.yaml                          # généré par split_dataset.py
```

---

## 3. Phase A — Capture + auto-annotation (le cœur, à faire en premier)

### A1. `DatasetCreator` (features/)
- S'abonne à `frameReady(FrameRef)` (zero-copy, comme CameraView) et au signal du TrackingWorker.
- **Cadence** : sauvegarde max ~2 img/s quand toutes les gates passent (throttle), pour ne pas remplir le disque de quasi-doublons.
- **Anti-doublon de pose** : ne sauvegarder que si l'homographie a "assez bougé" depuis la dernière frame retenue (distance entre les coins projetés de la carte > seuil, ex. 15 px) → garantit la variété des points de vue.

### A2. Gates qualité (toutes doivent passer)
| Gate | Mesure | Seuil initial | Pourquoi |
|------|--------|---------------|----------|
| Tracking verrouillé | inliers RANSAC | ≥ 25 | labels faux si l'homographie dérive |
| Erreur de reprojection | erreur médiane des inliers | ≤ 3 px | idem |
| Netteté | variance du Laplacien | ≥ seuil calibré | flou de bougé = mauvais dataset |
| Exposition | % pixels saturés/noirs | ≤ 5 % | reflets/sous-ex inutilisables |
| Fraîcheur homographie | âge de la dernière update | ≤ 300 ms | pas de label sur tracking périmé |

> ⚠️ Préalable technique : le TrackingWorker ne **publie** pas le nombre d'inliers ni l'erreur de reprojection (seulement en log debug). Étendre le signal `homographyUpdated(cv::Mat)` → `homographyUpdated(cv::Mat, int inliers, double reprojErr)`.

### A3. Projection des annotations
Pour chaque composant de la couche visible (`Layer::Front` ou `Back` selon la face alignée) :
1. Projeter les **4 coins** de `bbox` via l'homographie (`transformRect`) → rectangle englobant axis-aligned dans l'image.
2. **Clipping** au cadre image ; rejeter si la fraction visible < 60 % (composant coupé au bord).
3. Rejeter les boxes < 12×12 px (trop petites pour être apprises à ce zoom).
4. Option `bbox_shrink` par classe (la bbox iBOM inclut parfois la sérigraphie/courtyard, plus large que le composant visible — facteur 0.85 par défaut, à régler visuellement).
5. Écrire la ligne YOLO `class cx cy w h` normalisée.

### A4. Mapping footprint/référence → classe (`footprint_classes.json`)
Règles évaluées dans l'ordre (première qui matche) :

| Règle (regex sur footprint, puis fallback préfixe ref) | Classe |
|---|---|
| `^R_`, ref `R\d+` | `resistor` |
| `^C_`, ref `C\d+` | `capacitor` |
| `^L_`, ref `L\d+` | `inductor` |
| `LED`, ref `D\d+` + value contient "LED" | `led` |
| `^D_`, `SOD`, ref `D\d+` | `diode` |
| `SOT`, ref `Q\d+` | `transistor_sot` |
| `SOIC`, `SO-8` | `ic_soic` |
| `QFP`, `LQFP`, `TQFP` | `ic_qfp` |
| `QFN`, `DFN` | `ic_qfn` |
| `BGA` | `ic_bga` |
| ref `J\d+`, `CN\d+`, `USB`, `PinHeader`, `Conn` | `connector` |
| `Crystal`, ref `Y\d+`/`X\d+` | `crystal` |
| ref `SW\d+`, `Button` | `button` |
| _(défaut)_ | `other` |

Les composants non mappés sont **loggés avec leur footprint** (pas silencieusement mis dans `other`) pour enrichir le fichier de règles au fil des cartes.

### A5. `DatasetPanel` (gui/)
- Bouton Start/Stop session + champs : nom de carte, tag éclairage (`ring`, `lateral`, `ambiant`…).
- Affichage live : état des 5 gates (vert/rouge), images retenues / rejetées (par cause), nb de labels de la dernière frame.
- **Aperçu superposé** : dessiner les boxes projetées sur le flux caméra (réutilise OverlayRenderer) → l'utilisateur **voit** si les labels collent ; si ça glisse, il réaligne avant de continuer.

**Livrable Phase A** : une session de 15-20 min sur une carte → 300-600 images annotées valides.
**Effort estimé** : 1-2 sessions de dev. **C'est 80 % de la valeur du projet.**

---

## 4. Phase B — Assistant de variété (pour atteindre la diversité, pas juste le volume)

800 images quasi identiques ≈ 50 images utiles. Le panel guide l'utilisateur :

- **Carte de couverture** : grille sur le plan PCB (réutilise HeatmapRenderer) montrant quelles zones de la carte ont été capturées, à quels niveaux de zoom → l'utilisateur voit où passer ensuite.
- **Check-list de session** affichée : ☐ zoom ×0.5 ☐ ×1 ☐ ×2 · ☐ éclairage annulaire ☐ latéral ☐ faible · ☐ face avant ☐ arrière · ☐ légères rotations.
- **Quotas** : objectif d'images par combinaison (zoom × éclairage), compteurs visibles.
- `manifest.jsonl` enregistre ces tags → permet de mesurer la diversité réelle a posteriori et de stratifier le split train/val.

**Effort** : ~1 session de dev. Peut être livré après un premier entraînement si pressé.

---

## 5. Phase C — Outillage Python (validation avant d'entraîner)

1. **`validate_dataset.py`** : redessine les boxes sur N images aléatoires (mosaïque HTML/PNG), stats par classe (compte, taille médiane des boxes, par session), détection d'anomalies (boxes hors image, classes vides).
2. **`split_dataset.py`** : split train/val **par session** (jamais par image : deux frames de la même session sont quasi identiques → fuite train→val et mAP mensongère). Génère `dataset.yaml`.
3. **`review_sample.py`** : exporte 5-10 % des images vers Label Studio pour contrôle humain par échantillonnage — on ne corrige pas tout, on **mesure** le taux d'erreur de l'auto-annotation (objectif < 2-3 % de boxes fausses).
4. **`augment_offline.py`** (optionnel) : YOLOv8 augmente déjà à l'entraînement (mosaic, HSV, flips, rotations configurées dans AI_PIPELINE.md). N'ajouter de l'augmentation hors-ligne que si un manque précis est mesuré (ex. reflets spéculaires synthétiques).

**Effort** : ~1 session de dev (scripts simples).

---

## 6. Phase D — Boucle d'amélioration continue (après le 1er modèle)

1. Entraîner v1 (cf. AI_PIPELINE.md), évaluer sur le val set.
2. **Hard-example mining gratuit** : en mode capture, quand le modèle v1 tourne, comparer ses détections aux projections iBOM ; les frames où elles **divergent** (composant manqué, classe fausse) sont les plus précieuses → les marquer prioritaires pour le dataset v2. C'est la même mécanique que la future fonction d'inspection — le code sert deux fois.
3. **Cartes sans iBOM** : pré-annotation par le modèle (model-assisted labeling dans Label Studio), correction manuelle rapide → élargit le dataset au-delà des cartes dont on a l'iBOM.
4. Ré-entraîner, mesurer, répéter.

---

## 7. Risques & parades

| Risque | Impact | Parade |
|--------|--------|--------|
| Dérive d'homographie → labels décalés | Bruit de labels, plafond de mAP | Gates stricts (A2) + aperçu live (A5) + review 5-10 % (C3) |
| Une seule carte → sur-apprentissage | Modèle inutilisable sur d'autres PCB | **≥ 3-5 designs de PCB différents** (chacun avec son iBOM) ; le split par session le rendra visible |
| bbox iBOM ≠ emprise visuelle | Boxes trop larges | `bbox_shrink` par classe (A3.4), réglé via validate_dataset.py |
| Déséquilibre de classes (1000 R/C, 3 BGA) | Classes rares mal apprises | Stats C1 ; plafonner les classes majoritaires au split ; viser des cartes riches en ICs |
| Composants sous silkscreen dense / reflets | Labels présents mais invisibles | Gate exposition + tag éclairage ; accepter un peu de bruit (YOLO y est tolérant) |
| Face arrière : composants de l'autre couche | Labels fantômes | Filtrage strict par `Layer` (A3) — l'alignement indique la face |

---

## 8. Réutilisation de Pokemon-Dataset-Creator — à confirmer

> ⚠️ Le repo est privé et **inaccessible depuis cette session** (accès GitHub limité à `lo26lo/assistant`). Pour que je puisse l'examiner : l'ajouter aux sources de la session Claude Code (ou le rendre temporairement public). En attendant, hypothèses à valider :

| Brique probable du projet Pokemon | Réutilisable ici ? |
|---|---|
| Writer YOLO (images/ + labels/ + dataset.yaml) | ✅ directement (Phase C) |
| Pipeline d'augmentation (transforms, bruit, fonds) | 🟡 en partie — ici YOLOv8 augmente à l'entraînement ; utile si compositing synthétique |
| Compositing cutout→fond avec labels auto | 🟡 équivalent conceptuel = projection iBOM (plus précis) ; pourrait servir pour les classes rares (coller des crops de BGA sur d'autres fonds de PCB) |
| Workflow GUI de session de capture | ✅ comme référence UX pour DatasetPanel |
| Scripts split/validation | ✅ si présents |

---

## 9. Ordre d'exécution & estimation

| # | Quoi | Dépend de | Effort | Valeur |
|---|------|-----------|--------|--------|
| 1 | Phase A (capture + auto-annotation + panel minimal) | build Jetson validé | 1-2 sessions | ★★★★★ |
| 2 | Phase C1-C2 (validation + split) | A | ~½ session | ★★★★ |
| 3 | Première capture réelle (1 carte, 2 éclairages) + review C3 | 1+2, **caméra branchée** | 1 h de manip | ★★★★★ |
| 4 | Entraînement v1 (PC Windows RTX 5070) | 3 | 1 soirée GPU | ★★★★ |
| 5 | Phase B (assistant de variété) | A | 1 session | ★★★ |
| 6 | Phase D (mining, multi-cartes) | 4 | continu | ★★★★ |

**Prérequis matériels avant l'étape 3** : caméra microscope branchée (décommenter `/dev/video0` dans `compose.local.yml`), au moins une carte avec son iBOM HTML, idéalement 2 sources d'éclairage.

---

## 10. Questions ouvertes (à trancher avant la Phase A)

1. **Boxes orientées ?** YOLOv8 supporte l'OBB (oriented bounding boxes) — l'iBOM connaît la rotation de chaque composant. v1 = boxes droites (plus simple, suffisant) ; OBB en v2 si le besoin d'orientation fine se confirme.
2. **JPEG ou PNG ?** JPEG qualité 95 (taille ÷10, artefacts négligeables à cette qualité) — recommandé.
3. **Résolution sauvegardée** : frame native 1920×1080 (recommandé — YOLO redimensionne lui-même) ou crop 640² autour des zones denses ?
4. Le nettoyage des règles `footprint_classes.json` se fera sur **tes** cartes réelles — prévoir 30 min de revue des "non mappés" après la première session.
