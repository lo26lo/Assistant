# Alignement par composants (`ComponentReanchor`)

> Comment la pose PCB→image est calculée **à partir des composants**, et non du
> contour de la carte. Complète `AUTO_ALIGN_V2_PLAN.md` (le plan) et
> `BLOB_REANCHOR_JITTER_ANALYSE.md` (l'analyse du jitter qui a motivé le fit
> similarité). Le gate de décision qui filtre le résultat est documenté à part
> dans le code (`src/overlay/ReanchorGate.h`).

Sources : `src/overlay/ComponentReanchor.{h,cpp}`, appelé depuis
`Application::componentReanchor()` (`src/app/Application.cpp:1449`).

---

## 1. Idée générale

Le moteur `overlay::ComponentReanchor` calcule l'homographie PCB→image **sans
jamais regarder le contour de la carte**. Il met en correspondance :

| Côté modèle (attendu) | Côté image (observé) |
|-----------------------|----------------------|
| La **constellation des composants** de l'iBOM (leurs positions en mm) | Les **détections de composants** dans la frame |

Les détections viennent :

- d'un **modèle YOLO** si `models/` en contient un (`detector->detect()`), ou
- de **blobs CV classiques** sinon (`overlay::detectComponentBlobs()`,
  `Application.cpp:1607`) — l'alignement par composants fonctionne donc avec un
  dossier `models/` vide.

C'est le pendant de `BoardLocator` : il marche **précisément là où celui-ci
échoue** — carte en gros plan qui remplit tout le cadre (microscope / D405), où
aucun contour de board n'est visible.

### Point de référence d'un composant

Le centre utilisé est celui de la **bbox** (`componentCenter`,
`ComponentReanchor.cpp:31`), **pas** `Component::position`. Raison : `position`
n'est renseigné que si le footprint iBOM a un champ `center` ; beaucoup ne l'ont
pas, il reste alors à `(0,0)` et effondrerait tous les composants sur l'origine
(« degenerate layout »).

---

## 2. Deux modes selon qu'on a une pose ou non

Le caller (`Application.cpp:1619-1623`) tente toujours `estimate()` d'abord, puis
bascule sur `bootstrap()` en cas d'échec :

```cpp
auto res = overlay::ComponentReanchor::estimate(
    detections, *project, priorPose, activeLayer, {}, rp);
if (!res.found)
    res = overlay::ComponentReanchor::bootstrap(
        detections, *project, activeLayer, scalePrior, rp);
```

### 2.1 `estimate()` — correction d'une pose existante (cas rapide)

On dispose déjà d'une pose (même dérivée). Étapes :

1. **Prédiction** : chaque composant de la couche active est projeté en image
   via la pose courante (`currentPose.pcbToImage`).
2. **Appariement** : chaque détection est reliée au composant prédit le plus
   proche, dans un **rayon de gating** (`maxMatchDistPx = 60 px`, doit dépasser
   confortablement la dérive attendue). Tous les couples candidats sont triés
   par distance puis assignés **gloutonnement**, chacun utilisé une seule fois.
3. **Fit RANSAC** sur les correspondances : `findHomography` 8-DOF, ou
   `estimateAffinePartial2D` 4-DOF (similarité) selon `fitSimilarity`.
4. **Validation** : inliers ≥ `minInliers` (8) **et** erreur de reprojection
   médiane ≤ `maxMedianReprojPx` (8 px). Sinon rejet (`found = false`).

L'appariement est **purement spatial** : un détecteur « présence de composant »
mono-classe suffit. La classe (si un vrai modèle la fournit) n'est qu'un filtre
optionnel (`useClassPrior`) interdisant d'apparier une détection à un composant
de classe différente.

### 2.2 `bootstrap()` — enregistrement global sans prior (board déplacé)

Pas de pose, ou pose trop périmée (carte prise puis reposée) : plus rien ne tombe
dans le rayon de gating, `estimate()` est inutile. `bootstrap()` résout le
**problème global** :

- RANSAC sur des hypothèses **paire→paire** : chaque correspondance (paire de
  détections ↔ paire de composants) détermine entièrement une similarité
  scale + rotation + translation.
- **Consensus** = nombre de composants qui, une fois transformés, tombent sur une
  détection dans une **tolérance physique** (`bootstrapTolMm = 1.2 mm` converti
  en px via l'échelle de l'hypothèse — reste valable de la vue D405 large au fort
  grossissement microscope).
- Filtres : paires trop rapprochées rejetées (baseline courte ⇒ angle/échelle
  noyés dans le bruit) ; fenêtre d'échelle plausible bornée par
  `scalePriorPxPerMm` (~[0.55, 1.8]×).
- **RNG déterministe** (seed `0x5EED`) : même scène → même pose, aucune loterie
  image par image.
- La similarité gagnante est ensuite passée à `estimate()` comme prior, pour
  bénéficier du fit précis et de la validation inliers/reprojection.

C'est ce qui transforme l'Auto-Align en « pose la carte sous la caméra et elle
s'aligne toute seule » dès qu'un détecteur est disponible.

---

## 3. Deux subtilités importantes

### 3.1 Face arrière (miroir)

Au dos, la caméra voit le layout **miroité**. Une similarité (et les hypothèses
paire→paire du bootstrap) ne peut pas représenter un miroir. Les fits tournent
donc dans une **« view frame »** — `x` négatif pour `Layer::Back`
(`viewPoint()`) — et le miroir est **recomposé** dans la matrice retournée
(`H * mirrorX()`). La convention app-wide « l'homographie mappe toujours mm PCB
brut → px image » est préservée (déterminant négatif quand on regarde le dos).

### 3.2 Similarité 4-DOF vs homographie 8-DOF (`fitSimilarity`)

Avec les blobs CV (centres bruités) sur une scène **fronto-parallèle**, les deux
termes perspectifs d'une homographie 8-DOF sont fittés sur du bruit pur. Ils ne
bougent presque pas l'erreur intérieure mais font **levier de dizaines de px aux
coins du board** — exactement ce que le gate de dérive mesure, d'où le jitter
13-63 px de juillet 2026 (`BLOB_REANCHOR_JITTER_ANALYSE.md`).

On force donc la similarité 4-DOF quand il n'y a pas de vrai modèle
(`rp.fitSimilarity = (detector == nullptr)`, `Application.cpp:1614`), ce qui
divise ce jitter de coin par ~4. Avec un modèle entraîné (centres répétables),
on garde l'homographie complète, qui bénéficie alors d'un vrai tilt caméra.

---

## 4. Où le gate des coins recolle

Une fois `estimate`/`bootstrap` a rendu une homographie, le caller projette les
**4 coins du board** avec, et c'est **là seulement** que `ReanchorGate`
intervient (`Application.cpp:1549`) : il compare coins-nouveaux vs coins-actuels
pour décider **Skip / Hold / Apply**.

> **Les composants calculent la pose ; les coins ne servent que de métrique de
> décision** — ne pas perturber un tracking sain avec une micro-correction, ni
> laisser un tick aberrant (main, reflet) tirer l'overlay. Détail des règles du
> gate : `src/overlay/ReanchorGate.h` + `tests/test_reanchor_gate.cpp`.

Chemin complet d'un re-anchor silencieux périodique :

```
timer périodique ─▶ componentReanchor(silent=true)         [Application.cpp:540]
   │
   ├─ QtConcurrent::run (thread worker) :
   │     détections = detector->detect()  |  detectComponentBlobs()
   │     estimate(prior) ──(échec)──▶ bootstrap(global) ──▶ estimate(refine)
   │
   └─ finished (thread GUI) :
         projette les 4 coins avec result.homography
         ReanchorGate.evaluate(newCorners, curCorners, lost, now)
           ├─ Skip  → return (tracking sain, on ne touche pas)
           ├─ Hold  → return (attend un 2ᵉ tick concordant)
           └─ Apply → setMatrix + refresh px/mm + resetReference tracking
```

---

## 5. Paramètres (`ComponentReanchor::Params`)

| Paramètre | Défaut | Rôle |
|-----------|--------|------|
| `maxMatchDistPx` | 60.0 | Rayon de gating autour de la position prédite (`estimate`) |
| `minMatches` | 8 | Correspondances minimales avant `findHomography` |
| `ransacThreshPx` | 6.0 | Seuil de reprojection RANSAC |
| `minInliers` | 8 | Inliers RANSAC minimaux pour accepter |
| `maxMedianReprojPx` | 8.0 | Rejet si l'erreur médiane inlier dépasse ce seuil |
| `useClassPrior` | false | N'apparier qu'à classe égale (nécessite `classOfComponent`) |
| `fitSimilarity` | false | Fit 4-DOF au lieu de 8-DOF (blobs bruités) |
| `bootstrapIterations` | 3000 | Hypothèses paire→paire RANSAC (`bootstrap`) |
| `bootstrapTolMm` | 1.2 | Tolérance de consensus physique en mm (`bootstrap`) |

Le prior d'échelle (`scalePriorPxPerMm`) provient, dans l'ordre : pinhole D405
(`fx / distance`) si disponible, sinon le px/mm courant, sinon 0 (espace
d'hypothèses plus large mais fonctionnel).
