# Plan — Placement & inspection 0201 au microscope (caméra unique 0.3x)

> **Date** : 2026-06-16 · **Statut** : Planification — non implémenté
> **Remplace** : l'approche dual-camera (D405 macro + micro) abandonnée — voir §0.
> **Objectif** : guider le placement manuel et inspecter des composants 0201 (0,6 × 0,3 mm) à travers le microscope, avec overlay iBOM correct malgré un champ étroit et un zoom optique variable.

---

## 0. Décision d'architecture (2026-06-16)

L'idée initiale (D405 grand-champ qui ancre la position + microscope en gros plan) est **abandonnée**, après clarification du montage réel :

| Constat | Conséquence |
|---------|-------------|
| Une **seule caméra** : l'originale, montée sur le microscope via **bague de réduction 0.3x** | Pas de 2e flux. On réutilise le chemin `CameraCapture` existant. |
| La D405 **ne peut pas** rester pointée sur la carte en continu | Pas d'ancre macro continue possible. |
| La D405 est **sortie du workflow placement** | Reste utile pour le 3D/inspection volumétrique, mais pas ici. |
| Le microscope **et** la caméra ont un **zoom continu** (pas de crans) | L'échelle mm/px change en permanence → **estimation d'échelle live depuis l'homographie**, pas de table calibrée. |

**Conséquence positive** : zéro plumbing dual-camera, zéro nouvelle classe caméra. Le gros du travail est dans la **robustesse de la localisation à fort grossissement** et la **gestion du zoom**.

---

## 1. Le vrai problème

Avec une seule caméra au microscope, l'overlay iBOM existe déjà (`OverlayRenderer` + `Homography` + `TrackingWorker` ORB/RANSAC). Mais trois facteurs cassent le tracking actuel à l'échelle 0201 :

1. **Champ étroit** : à fort grossissement, la caméra ne voit que quelques composants. Le matching ORB global contre toute la carte manque de features et peut ne pas verrouiller.
2. **Zoom continu (micro + caméra)** : l'échelle mm/px change en permanence et sans cran repérable. Deux étages de zoom se composent, mais le résultat net est une seule échelle inconnue qu'il faut estimer en continu. ORB tolère un peu le changement d'échelle entre frames proches, mais un gros écart fait échouer le matching.
3. **Motifs répétés** : des rangées de 0201 identiques → matches ORB ambigus → RANSAC peut verrouiller au **mauvais endroit** de la carte.

C'est exactement les trois écueils à résoudre. La bague 0.3x **aide** (elle élargit le champ vs le micro nu), mais ne suffit probablement pas seule.

---

## 2. Stratégie de localisation : ancre manuelle + suivi incrémental

Plutôt qu'un matching global naïf (fragile à cause des motifs répétés), on combine :

```
1. ANCRAGE  (1 fois, ou quand on se perd)
   ┌─────────────────────────────────────────────┐
   │ L'utilisateur indique où il est :            │
   │   • tape la référence ("R42"), OU            │
   │   • clic sur la minimap de la carte, OU      │
   │   • dézoome (0.3x large) → match global 1 fois│
   └─────────────────────────────────────────────┘
                       │
                       ▼  position PCB initiale connue
2. SUIVI INCRÉMENTAL  (en continu pendant qu'on se déplace)
   ┌─────────────────────────────────────────────┐
   │ Matching ORB frame N-1 → frame N             │
   │ (déplacement local, pas global)              │
   │ → composition des homographies               │
   │ → position PCB mise à jour                   │
   │ Matching CONTRAINT à la région attendue      │
   │ (élimine l'ambiguïté des motifs répétés)     │
   └─────────────────────────────────────────────┘
                       │
                       ▼  drift accumulé
3. RE-ANCRAGE  (quand drift trop grand ou perte)
   → retour étape 1 (manuel ou auto si features riches)
```

**Pourquoi incrémental** : entre deux frames, le déplacement est petit → peu d'ambiguïté, matching robuste même avec peu de features. Le coût est le **drift** (erreur cumulée), corrigé par re-ancrage périodique.

**Contrainte régionale** : à chaque frame, on ne cherche les composants que dans une fenêtre autour de la dernière position connue (via `ComponentMap` requête spatiale). Ça tue le faux-positif "rangée de 0201 identiques".

---

## 3. Échelle avec zoom continu (Q1 = zoom continu, micro + caméra)

Pas de crans répétables → **aucune table calibrée possible**. L'échelle mm/px doit être **estimée en continu depuis l'homographie**. Bonne nouvelle : la transformation PCB→pixels que le tracking ajuste contient déjà l'échelle nette des deux étages de zoom — on n'a pas à connaître chaque étage séparément.

### Mécanisme : échelle déduite de la pose
- L'homographie (ou similarité) ajustée entre la vue courante et la géométrie iBOM connue (pads, en mm) **donne directement mm/px**, à chaque frame verrouillée. C'est `scaleMethod = Homography`, mais appliqué en continu.
- Aucune saisie de zoom par l'utilisateur : ça suit le zoom tout seul tant que le tracking tient.

### Le problème d'amorçage (chicken-and-egg)
Pour ajuster une homographie il faut des matches ; pour matcher sans ambiguïté il faut une position approximative. Résolu par l'**ancrage manuel** (§2) :
1. L'utilisateur tape "R42" (ou clic minimap) → on connaît la **géométrie attendue de R42 et ses voisins en mm**
2. On matche les features de la frame courante contre cette région locale
3. L'ajustement d'homographie sur cette région **donne pose + échelle d'un coup**, quel que soit le zoom courant

→ L'échelle est **bootstrappée à l'ancrage**, puis raffinée en continu pendant le suivi.

### Garde-fous
- Si le tracking lâche (zoom brusque, champ vide), l'échelle est figée à sa dernière valeur valide jusqu'au re-ancrage, avec badge "échelle incertaine".
- Estimation lissée (filtre sur quelques frames) pour éviter le jitter de l'overlay quand tu tournes la molette de zoom.

> Le `opticalMultiplier` scalaire actuel (0.5x–2x) n'a plus de sens avec un zoom continu : il devient une simple valeur de repli (dernière échelle connue), pas un réglage manuel principal.

---

## 4. Overlay UX à fort grossissement

Le champ ne montre que 1–5 composants. L'overlay doit être lisible, pas surchargé :

- **Composant ciblé** mis en évidence (contour épais, couleur d'accent) + label complet (ref, value, footprint, orientation attendue)
- **Voisins** dans le champ : contour fin + ref seulement
- **Indicateur de position** : minimap permanente (coin) montrant où on est sur la carte entière, FOV courant surligné
- **État tracking** : badge "Verrouillé / Drift / Perdu — ré-ancrer" (réutilise la logique d'état de `TrackingWorker`)
- **Pip1 / polarité** : marquer le pin 1 / la polarité attendue (déjà dans `Pad::isPin1`)

---

## 5. Inspection & IA (post-placement)

Une fois le composant posé :
- L'overlay compare attendu (iBOM) vs observé
- `ComponentDetector` (ONNX) tourne sur le flux micro → détecte : composant manquant, décalé, tombstone, mauvaise valeur (si OCR), pont de soudure
- Classes adaptées au 0201 (à entraîner — cf. DATASET_CREATOR_PLAN.md, dataset au grossissement micro)
- Affichage des défauts en surimpression dans la vue micro

---

## 6. Fichiers à créer / modifier

### Réutilisation (pas de nouveau pipeline caméra)
- `CameraCapture` : inchangé — c'est déjà la caméra micro
- `OverlayRenderer`, `Homography`, `ComponentMap` : réutilisés tels quels

### Modifications

| Fichier | Modification |
|---------|-------------|
| `src/overlay/TrackingWorker.h/.cpp` | Ajouter **mode incrémental** (match frame→frame) + **matching contraint à la région** ; exposer état drift/perdu |
| `src/overlay/Homography.h/.cpp` | Support composition d'homographies (suivi incrémental) + ré-ancrage |
| `src/app/Config.h/.cpp` | Section `microscope` : lissage échelle, repli mm/px, seuil drift ; déprécier `opticalMultiplier` (devient simple repli) |
| `src/gui/CameraView.cpp` | Overlay fort-grossissement : composant ciblé vs voisins, badge tracking, minimap permanente |
| `src/gui/MainWindow.h/.cpp` | Champ "Cibler composant" (ancrage par ref) + clic minimap → ancrage ; affichage échelle live mm/px |
| `src/gui/SettingsDialog.cpp` | Onglet Camera : lissage échelle, repli mm/px, seuil re-ancrage |

### Nouveaux fichiers (légers)

| Fichier | Rôle |
|---------|------|
| `src/overlay/MicroscopeLocator.h/.cpp` | Orchestration ancrage manuel + suivi incrémental + re-ancrage ; détient la position PCB courante et l'échelle |
| `src/gui/BoardMinimap.h/.cpp` | Mini-vue de la carte entière, FOV surligné, clic → ancrage (réutilise rendu OverlayRenderer en réduit) |

---

## 7. Structures de données

```cpp
// Config — zoom continu : pas de table, échelle estimée live
struct MicroscopeConfig {
    double scaleSmoothing   = 0.2;   // lissage EMA de mm/px (anti-jitter molette)
    double fallbackMmPerPx  = 0.05;  // repli si jamais ancré (ex-opticalMultiplier)
    double reanchorDriftPx  = 40.0;  // seuil drift → invite re-ancrage
};

// MicroscopeLocator — état courant
struct MicroscopeState {
    enum class Status { Anchored, Drifting, Lost };
    Status      status = Status::Lost;
    Point2D     centerPcb;        // centre du FOV en coords PCB (mm)
    double      mmPerPx;          // échelle courante
    cv::Mat     Hcumulative;      // homographie cumulée depuis dernier ancrage
    double      driftPx = 0.0;    // estimation drift accumulé
};
```

---

## 8. Ordre d'implémentation

```
Étape 1 — Socle ancrage manuel (utilisable tout de suite)   [~2 jours]
  ├─ MicroscopeLocator : ancrage par ref + clic minimap
  ├─ BoardMinimap : vue carte + FOV + clic
  ├─ Config zoom levels + sélecteur de cran
  └─ Overlay ciblé/voisins dans CameraView
      → tu peux déjà placer : tape "R42", l'overlay te montre R42 au bon endroit

Étape 2 — Suivi incrémental                                  [~3 jours]
  ├─ TrackingWorker mode frame→frame + composition homographie
  ├─ Matching contraint à la région (ComponentMap spatial)
  └─ Badge état Verrouillé/Drift/Perdu + invite re-ancrage
      → l'overlay suit tes déplacements sans re-taper la ref

Étape 3 — Échelle live + amorçage à l'ancrage                [~1 jour]
  └─ mm/px déduit de l'homographie en continu (lissé) + bootstrap à l'ancrage

Étape 4 — IA inspection 0201                                 [~2 jours + dataset]
  └─ ComponentDetector sur flux micro (cf. DATASET_CREATOR_PLAN.md)
```

L'**Étape 1** seule rend l'outil exploitable (ancrage manuel = fiable). Les étapes 2–4 réduisent la friction.

---

## 9. Questions ouvertes restantes

| # | Question | Pourquoi ça compte |
|---|----------|-------------------|
| Q1 | ~~Crans répétables ou continu ?~~ | ✅ **Résolu** : zoom continu (micro + caméra) → échelle estimée live, pas de table (§3) |
| Q2 | Champ réel (en mm) avec la bague 0.3x, au grossissement typique de placement 0201 ? | Dimensionne la fenêtre de matching et l'overlay |
| Q3 | Combien de composants visibles à l'écran à ce grossissement (ordre de grandeur) ? | Si < 3, le matching incrémental devient prioritaire ; si > 10, le global peut suffire — **à mesurer sur le matériel** |
| Q4 | La carte reste-t-elle **immobile** pendant que tu déplaces le microscope ? | Si la carte bouge aussi, le suivi incrémental devient plus difficile |
| Q5 | Index V4L2 de la caméra micro stable au reboot Jetson ? | Auto-détection vs index figé en config |
