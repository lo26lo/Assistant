# Plan détaillé — RealSense D405 Phase 3 : inspection 3D

> **Date** : 2026-06-15 · **Statut** : 📋 PLAN (non implémenté)
> **Prérequis** : Phases 1 (couleur dual-backend) ✅ et 2 (profondeur : distance + scale auto) ✅ **validées avec la D405 branchée**.
> **Document parent** : [REALSENSE_D405_PLAN.md](REALSENSE_D405_PLAN.md) · **Calibration** : §11 du parent (OCC sans cible).
> **Objectif** : transformer la profondeur en **fonctionnalités d'inspection** — détecter les défauts qu'une caméra 2D ne peut pas voir (composant manquant/soulevé, tombstoning, pont de soudure), via la hauteur Z par rapport au plan du PCB.

---

## 0. Idée centrale : tout part d'un **plan de référence**

La depth brute donne une distance caméra→surface. Pour l'inspection, ce qui compte c'est la **hauteur d'un point au-dessus du plan du PCB**. Donc :

```
depth (mm, alignée couleur)  ──►  estimation du plan PCB (RANSAC)
                                          │
                       height[y,x] = distance signée au plan
                                          │
        ┌─────────────────┬───────────────┬──────────────────┐
   heatmap hauteur   validation par     détection         export
   (overlay)         composant (iBOM)   tombstoning/pont   PLY
```

Le plan de référence est estimé une fois (carte nue ou zones de cuivre exposé), puis réutilisé. La hauteur devient indépendante de la distance de travail et de l'inclinaison de la carte.

---

## 1. Estimation du plan de référence

**Entrée** : un `DepthFrameRef` (CV_16UC1 mm, déjà aligné couleur — Phase 2).

**Méthode** : ajustement de plan par RANSAC sur un sous-échantillon du nuage.
1. Déprojeter les pixels depth en 3D : `(X,Y,Z)` via les intrinsèques (`rs2_deproject_pixel_to_point`, ou manuellement `X=(x-ppx)*Z/fx`). On a déjà `colorFx()` ; il faut aussi exposer `ppx, ppy, fy` → étendre `RealSenseCapture` avec une petite struct `ColorIntrinsics{fx,fy,ppx,ppy}`.
2. Échantillonner ~2000 points valides (Z>0), RANSAC plan `ax+by+cz+d=0` (seuil ~0.5 mm, ~100 itérations).
3. Stocker le plan + la normale. **height(p) = distance signée de p au plan**.

**Quand l'estimer** :
- Bouton « Définir le plan PCB » (carte nue sous la caméra), ou
- Auto au chargement iBOM si le tracking est verrouillé (zones hors composants = plan).

> Alternative légère (si RANSAC trop lourd) : prendre la profondeur médiane des zones **sans composant** (d'après les bboxes iBOM projetées) comme plan fronto-parallèle `Z = Z0`. Moins robuste à l'inclinaison mais trivial. **Démarrer par là en Phase 3a**, passer au RANSAC si l'inclinaison pose problème.

---

## 2. Architecture

```
src/features/DepthInspector.{h,cpp}   # cœur : plan de réf, height map, mesures par composant
src/overlay/HeightMapOverlay.{h,cpp}  # rendu heatmap hauteur (ou réutiliser HeatmapRenderer)
src/gui/InspectionPanel (ou nouveau)  # toggles + lecture des résultats
src/export/PointCloudExporter.{h,cpp} # export PLY
```

**Flux** : `RealSenseCapture::depthFrameReady` → `DepthInspector::processDepth(DepthFrameRef, Homography*, IBomProject*)` sur un thread dédié (comme `TrackingWorker`/`DatasetCreator`) → émet :
- `heightMapReady(QImage overlay)` → `CameraView::setOverlayImage` (fusion avec l'overlay iBOM existant)
- `componentHeights(QVector<CompHeight>)` → panneau résultats

**Threading** : réutiliser le pattern `QThread` dédié + `QueuedConnection` (cf. `Application::~Application` qui quitte/join les threads). La depth arrive déjà en QueuedConnection ; `DepthInspector` se met sur son propre thread pour ne pas bloquer le GUI avec le RANSAC + déprojection.

---

## 3. Feature A — Heatmap hauteur (overlay)

1. Pour chaque pixel : `h = height(deproject(x,y,depth))`.
2. Mapper `h ∈ [0, h_max]` (ex. 0–5 mm) sur une LUT couleur (bleu→rouge), alpha selon validité.
3. Rendre un `QImage` ARGB de la taille couleur → fusion dans `CameraView` (même chemin que l'overlay iBOM).
4. Réutiliser `HeatmapRenderer::renderOnMat` n'est pas idéal (il accumule des défauts 2D) → **nouveau `HeightMapOverlay`** plus simple (LUT directe sur la height map), mais s'inspirer de la LUT existante.
5. Toggle `ControlPanel` : « Show height heatmap » (à côté de « Show Defect Heatmap »).

*Livrable 3a* : voir en couleur ce qui dépasse du plan, en live.

---

## 4. Feature B — Validation hauteur par composant (iBOM)

Pour chaque `Component` (a `bbox` en coords PCB) :
1. Projeter la bbox PCB → pixels couleur via `Homography::pcbToImage` (déjà utilisé pour l'overlay).
2. Échantillonner la height map dans cette ROI → hauteur médiane `h_med` + écart-type.
3. Comparer à une **hauteur attendue** :
   - **Mode appris** : enregistrer `h_med` sur une carte de référence « bonne » → tolérance ±Δ.
   - **Mode par footprint** : table `footprint → hauteur nominale` (réutiliser `resources/footprint_classes.json` comme base, ajouter une colonne hauteur).
4. Statut par composant : `OK` / `MANQUANT` (h≈0) / `TROP_HAUT` / `TROP_BAS` / `INCERTAIN` (peu de points).
5. Remonter au `StatsPanel` (compteur) + colorer l'overlay du composant (rouge si hors tolérance) + ligne dans l'Event Log.

*Livrable 3b* : « R12 manquant », « C5 trop haut (1.8 mm vs 0.9) ».

> **Synergie iBOM** : exactement la même projection bbox→image que le `DatasetCreator` (Phase A) — on peut factoriser la projection.

---

## 5. Feature C — Détection tombstoning

Cible : composants 2 pins (0402/0603/0805 — `comp.pads.size()==2`).
1. Projeter les 2 pads → 2 ROIs depth.
2. Calculer la hauteur médiane sous chaque pad : `h1`, `h2`.
3. **Tombstoning** si `|h1 - h2| > seuil` (ex. > moitié de la hauteur du composant) **et** un côté ≈ plan : un pad au sol, l'autre dressé.
4. Statut `TOMBSTONE` + marqueur overlay.

---

## 6. Feature D — Composant soulevé / pont de soudure

- **Soulevé** : hauteur du corps du composant uniformément > nominal + Δ.
- **Pont de soudure** : pic de hauteur **localisé entre deux pads adjacents** là où le plan devrait être nu. Détection : dans l'inter-pad, `h > seuil_pont` sur une zone connexe.
- Plus bruité → marquer `SUSPECT` (pas un rejet ferme), à confirmer visuellement.

---

## 7. Feature E — Export nuage de points PLY

1. Déprojeter toute la frame (ou la ROI carte) → points `(X,Y,Z)` + couleur (RGB de la frame alignée).
2. Écrire un `.ply` ASCII/binaire sous `$IBOM_DATA_DIR/exports/`.
3. Bouton dans `InspectionPanel` (« Export 3D (PLY) ») à côté des exports existants (CSV/JSON/Report).
4. Utile pour rapport / archivage / analyse hors-ligne (MeshLab, CloudCompare).

---

## 8. Config (nouveaux paramètres)

```jsonc
"depth_inspect": {
  "enabled": false,
  "plane_method": "median",      // "median" | "ransac"
  "plane_ransac_thresh_mm": 0.5,
  "height_max_mm": 5.0,          // échelle heatmap
  "comp_height_tol_mm": 0.5,     // tolérance validation
  "tombstone_delta_mm": 0.4,
  "bridge_height_mm": 0.3,
  "min_valid_frac": 0.5          // fraction mini de pixels valides dans une ROI
}
```

---

## 9. UI

- `ControlPanel` : case « Show height heatmap ».
- `InspectionPanel` : bouton « Définir plan PCB », bouton « Export 3D (PLY) », et un récap (#OK / #hors-tolérance / #tombstone).
- `StatsPanel` : compteur défauts 3D (réutiliser `addDefectEntry` → Event Log).
- `SettingsDialog` : onglet (ou section) « Depth inspection » avec les tolérances.

> Les features 3D ne s'activent que si `m_camera->hasDepth()` — masquées/grisées pour le microscope USB.

---

## 10. Calibration & limites

- **Pas de cible imprimée** : OCC (Phase 2 §11) suffit pour une depth fiable ; la hauteur est **relative au plan estimé**, donc robuste aux petits biais d'échelle.
- **Bruit depth D405** : ~quelques dixièmes de mm à 10–20 cm. Les seuils (tolérance ≥ 0.4–0.5 mm) tiennent compte de ce bruit ; un filtrage temporel (moyenne sur N frames immobiles) améliore la stabilité.
- **Occlusions / réflexions** : composants brillants (capots métal, connecteurs) → trous depth. Gérer via `min_valid_frac` → statut `INCERTAIN` plutôt que faux positif.
- **Alignement** : dépend de la qualité de l'homographie iBOM (tracking) pour la projection bbox. Mauvais tracking = mauvaises ROIs → ne valider que si `homographyAge` récent + inliers suffisants (mêmes gates que le `DatasetCreator`).

---

## 11. Découpage interne Phase 3

| Sous-phase | Contenu | Effort |
|---|---|---|
| **3a** | `DepthInspector` + plan médian + height map + heatmap overlay + toggle | ~4 h |
| **3b** | Validation hauteur par composant (iBOM) + statuts + Event Log | ~4 h |
| **3c** | Tombstoning + soulevé/pont (features C/D) | ~3 h |
| **3d** | Export PLY + plan RANSAC (robustesse inclinaison) | ~3 h |

> Recommandation : livrer **3a d'abord** (visuel immédiat, valide le pipeline depth→plan→overlay), puis 3b (la vraie valeur inspection), puis 3c/3d.

---

## 12. Checklist 3a (point de départ)

- [ ] `RealSenseCapture` : exposer `ColorIntrinsics{fx,fy,ppx,ppy}` (déprojection)
- [ ] `features/DepthInspector.{h,cpp}` (QThread dédié) — plan médian + height map
- [ ] `overlay/HeightMapOverlay.{h,cpp}` — LUT hauteur → QImage
- [ ] `Application` : instancier `DepthInspector` sur thread, brancher `depthFrameReady` → overlay
- [ ] `ControlPanel` : toggle « Show height heatmap » (visible si `hasDepth()`)
- [ ] `Config` : bloc `depth_inspect` + defaults
- [ ] Fusion overlay hauteur ↔ overlay iBOM dans `CameraView` (ordre de rendu)
- [ ] Test Jetson + D405 + entrée `JETSON_SESSION_LOG.md`
