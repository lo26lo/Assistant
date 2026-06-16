# Plan — Dual-Camera : microscope USB + caméra macro grand-champ

> **Date** : 2026-06-16 · **Statut** : Planification — non implémenté
> **Objectif** : permettre le guidage de placement et l'inspection de composants 0201 au microscope USB, en s'appuyant sur la D405 (ou toute caméra grand-champ) pour maintenir la position globale sur la carte.
> **Contexte** : la D405 seule ne descend pas au 0201 (0,6 × 0,3 mm). Le microscope USB offre la résolution nécessaire mais son FOV couvre seulement quelques composants — sans contexte de position. La D405 résout le problème de position ; le microscope résout le problème de résolution.

---

## 1. L'idée centrale : séparation des responsabilités

```
D405 (vue macro, carte entière)
  │
  ├──► TrackingWorker (ORB + RANSAC)
  │         │
  │         └──► Homographie globale H_global (PCB → pixels D405)
  │                     │
  │                     ▼
  │              ComponentMap → quels composants sont dans le FOV du micro ?
  │                     │
  ▼                     ▼
Microscope USB ──► MicroscopeView + overlay local (composants visibles)
                         │
                         └──► InferenceEngine (défauts 0201 : manque, décalage, pont)
```

- **D405** : tracking seul. Elle n'est jamais affichée en plein écran pendant le placement — c'est elle qui sait où on est sur la carte.
- **Microscope** : affichage principal. Il reçoit les annotations iBOM du sous-ensemble de composants dans son FOV, calculé à partir de H_global et d'une estimation du FOV micro.
- **Pas de tracking ORB sur le microscope** : trop peu de contexte à fort grossissement. La position vient de la D405 + une localisation FOV (manuelle en Phase A, automatique en Phase B).

---

## 2. Phases d'implémentation

### Phase A — Dual-camera plumbing + FOV manuel (MVP)
*Effort : ~3–4 jours. Permet de travailler immédiatement.*

**A1 — Multi-instance caméra**

L'abstraction `ICameraSource` existe déjà. Il faut :
- Ajouter un 2e slot caméra dans `Application` : `m_microscope` (type `ICameraSource`, distinct de `m_camera`)
- Nouveau `CameraBackend::Microscope` (V4L2/MSMF, pas de profondeur)
- Config : `microscope.enabled`, `microscope.cameraIndex`, `microscope.width`, `microscope.height`
- Selector UI : dans `SettingsDialog` onglet Camera, section "Microscope" (index, résolution, enable)

**A2 — MicroscopeView widget**

Nouveau widget `src/gui/MicroscopeView.{h,cpp}`, calqué sur `CameraView` :
- Reçoit `FrameRef` depuis `m_microscope` via `frameReady`
- Overlay : affiche les composants iBOM dont la bbox PCB projetée via `H_global` intersecte le FOV estimé
- FOV estimé en Phase A : **rectangle centré sur un point cible**, taille configurable en mm (ex. 5 × 3 mm)
- Pas de tracking propre — reçoit `H_global` depuis `TrackingWorker` via signal existant `homographyUpdated`

**A3 — Layout UI dual-view**

```
┌─────────────────────────────────────┬───────────────────────┐
│                                     │   MicroscopeView      │
│        Vue principale               │   (gros plan)         │
│   (CameraView D405 ou point cloud)  │                       │
│                                     │   [composants visibles│
│                                     │    dans ce FOV]       │
└─────────────────────────────────────┴───────────────────────┘
```

Option : `QSplitter` horizontal, taille réglable. MicroscopeView masqué si `microscope.enabled = false`.

**A4 — Localisation FOV manuelle**

Deux façons de dire "le microscope est sur cette zone" :
1. **Clic sur minimap** : miniature de la carte PCB (déjà disponible dans OverlayRenderer) → clic → coordonnées PCB → FOV centré ici
2. **Saisie référence** : champ texte "Cibler composant" → tape "R42" → FOV centré sur bbox de R42

Ces deux méthodes coexistent. L'utilisateur bouge le microscope, tape la référence suivante → overlay se met à jour instantanément.

---

### Phase B — Localisation FOV automatique
*Effort : ~1 semaine. Élimine la saisie manuelle.*

**Principe** : à chaque frame micro, extraire des features ORB, les matcher contre la tuile D405 correspondant à la zone H_global. La tuile D405 est découpée dynamiquement depuis la dernière frame D405 haute résolution.

```
frame_micro ──► ORB extraction
                    │
frame_D405 ──► découpe tuile (zone centrale 20% de la carte)
                    │ ORB extraction
                    ▼
            BFMatcher + Lowe ratio
                    │
            Homographie locale H_micro→tuile
                    │
            Position micro dans coords PCB (composée avec H_global)
```

Contraintes :
- Ne marche que si le microscope voit des features (pads, silkscreen) — peut échouer sur des zones vides ou fortement dépoli
- Fallback : revenir au mode manuel si le matching rate < `minMatchCount`
- Worker dédié `MicroscopeTrackingWorker` (même pattern que `TrackingWorker`, QThread dédié)

**Configuration** : `microscope.autoLocate` (bool), `microscope.autoLocateMinInliers` (défaut 6)

---

### Phase C — AI sur flux microscope
*Effort : ~2 jours. Rebranche InferenceEngine sur la 2e source.*

`InferenceEngine` / `ComponentDetector` tournent déjà sur le flux D405. Il faut :
- Permettre une 2e instance `ComponentDetector` alimentée par `m_microscope`
- Classes pertinentes au microscope : composant_manquant, composant_décalé, pont_soudure, composant_retourné
- Le modèle peut être différent (`ai.microscope_model`, défaut : même que `ai.detector_model`)
- Résultats affichés dans `MicroscopeView` (bboxes + labels)

---

### Phase D — Dataset microscope (optionnel, ultérieur)
*Logique identique à DatasetCreator (cf. DATASET_CREATOR_PLAN.md) mais avec :*
- FOV automatique (Phase B) comme source de vérité terrain pour les bboxes
- Résolution micro (~10–30 µm/px vs ~100 µm/px D405)
- Modèle séparé entraîné sur images microscope

---

## 3. Fichiers à créer / modifier

### Nouveaux fichiers

| Fichier | Rôle |
|---------|------|
| `src/camera/MicroscopeCapture.h/.cpp` | Wrapper V4L2/MSMF simple, sans depth. Peut réutiliser `GenericV4L2Capture` si existant, sinon dériver de `ICameraSource` |
| `src/gui/MicroscopeView.h/.cpp` | Widget gros plan, overlay local, reçoit H_global |
| `src/overlay/MicroscopeOverlay.h/.cpp` | Calcul sous-ensemble composants dans FOV, rendu annotation (valeur + orientation + status) |
| `src/features/MicroscopeTrackingWorker.h/.cpp` | Phase B seulement — localisation FOV automatique |

### Fichiers modifiés

| Fichier | Modification |
|---------|-------------|
| `src/app/Application.h/.cpp` | Ajouter `m_microscope`, `m_microscopeView`, `initializeMicroscope()`, `wireMicroscopeSignals()` |
| `src/app/Config.h/.cpp` | Section `microscope` : `enabled`, `cameraIndex`, `width`, `height`, `fovWidthMm`, `fovHeightMm`, `autoLocate` |
| `src/gui/MainWindow.h/.cpp` | Layout dual-view (`QSplitter`), toggle affichage microscope, champ "Cibler composant" |
| `src/gui/SettingsDialog.cpp` | Onglet Camera : section microscope (index, résolution, taille FOV mm, autoLocate) |
| `CMakeLists.txt` | Ajouter les nouveaux `.cpp`/`.h` |

---

## 4. Structures de données clés

```cpp
// Config ajouts
struct MicroscopeConfig {
    bool    enabled       = false;
    int     cameraIndex   = 1;       // différent de la D405 (index 0)
    int     width         = 1920;
    int     height        = 1080;
    double  fovWidthMm    = 5.0;     // FOV estimé en mm
    double  fovHeightMm   = 3.0;
    bool    autoLocate    = false;   // Phase B
};

// Transmis de TrackingWorker → MicroscopeView via signal existant
// homographyUpdated(cv::Mat H, int inliers, double reprojErr)
// MicroscopeView stocke H_global et recalcule le FOV à chaque frame

// Point cible (mode manuel)
struct MicroscopeFocusPoint {
    double pcbX, pcbY;           // coords PCB (mm)
    std::string targetRef;       // "R42" ou vide si clic direct
};
```

---

## 5. Calcul du FOV (Phase A)

Données disponibles :
- `H_global` : matrice 3×3 PCB→pixels D405 (maintenue par TrackingWorker)
- Point cible `(pcbX, pcbY)` en coords PCB
- `fovWidthMm`, `fovHeightMm` (config)
- Échelle px/mm issue de l'homographie (`scaleMethod = Homography`)

```
FOV_rect_PCB = Rect(pcbX - fovW/2, pcbY - fovH/2, fovW, fovH)

Composants dans FOV = { c ∈ ComponentMap | c.bbox ∩ FOV_rect_PCB ≠ ∅ }

Pour l'overlay MicroscopeView :
  bbox_micro = H_micro ⊗ c.bbox_PCB
  où H_micro mappe PCB → pixels microscope
  (Phase A : H_micro = scale factor mm→px microscope, centré sur FOV)
  (Phase B : H_micro déduit du matching features)
```

En Phase A, `H_micro` est une homographie affine pure (translation + échelle), calculable depuis `fovWidthMm` et la résolution du microscope.

---

## 6. UX — Workflow de placement

1. **Démarrer** : activer microscope dans Settings → MicroscopeView apparaît à droite
2. **Orienter** : la D405 maintient le tracking en arrière-plan
3. **Cibler** : taper "C3" dans le champ → overlay micro affiche C3 et ses voisins avec valeur/orientation
4. **Placer** : l'overlay montre le pad, l'orientation attendue, la valeur — en direct au grossissement micro
5. **Inspecter** : après placement, l'IA (Phase C) détecte manques/décalages sur le flux micro
6. **Suivant** : taper "C4" → le FOV se déplace

Futur (Phase B) : le FOV se déplace **automatiquement** en suivant le déplacement du microscope.

---

## 7. Dépendances et prérequis

- La D405 doit être trackée (homographie valide, `minMatchCount` atteint) pour que l'overlay micro soit pertinent
- Si H_global perdu : MicroscopeView affiche "Tracking perdu — pointer la D405 sur la carte"
- Aucune dépendance matérielle spécifique pour le microscope : tout microscope USB V4L2/UVC fonctionne

---

## 8. Ordre d'implémentation recommandé

```
Semaine 1 :
  ├─ A1 : MicroscopeCapture + config + sélecteur Settings      [1 jour]
  ├─ A2 : MicroscopeView (affichage flux brut + H_global recv) [1 jour]
  ├─ A3 : Layout dual-view QSplitter dans MainWindow           [½ jour]
  └─ A4 : MicroscopeOverlay + localisation manuelle (ref/clic) [1–2 jours]

Semaine 2 (si Phase A validée sur Jetson) :
  └─ B  : MicroscopeTrackingWorker (localisation automatique)   [4–5 jours]

Ultérieur :
  └─ C  : AI sur flux micro                                     [2 jours]
  └─ D  : Dataset microscope                                    [référence DATASET_CREATOR_PLAN.md]
```

---

## 9. Questions ouvertes

| # | Question | Impact |
|---|----------|--------|
| Q1 | Index V4L2 du microscope sur Jetson : stable ou change au reboot ? | Config vs auto-détection |
| Q2 | Résolution micro utile : 1080p suffit ou faut-il 4K ? | Choix caméra, latence |
| Q3 | L'échelle mm/px du microscope est-elle fixe ou variable (zoom optique) ? | Si variable → calibration zoom obligatoire en Phase A |
| Q4 | FOV en mm du microscope au grossissement typique de placement 0201 ? | Paramètre clé pour `fovWidthMm`/`fovHeightMm` |
| Q5 | La D405 peut-elle rester pointée sur la carte pendant que tu utilises le micro sans bouger la carte ? | Si oui : tracking D405 reste stable → Phase A suffit |
