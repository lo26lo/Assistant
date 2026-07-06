# Analyse — pourquoi la pose blob est précise en one-shot (3 px) mais pas répétable (~30 px)

> **Date** : 2026-07-06 · **Contexte** : suite 123 du [journal](JETSON_SESSION_LOG.md) — l'alignement model-free par blobs a verrouillé sur la vraie carte (35/123 composants, médiane 3,3 px), mais le re-anchor **périodique** par blobs secouait l'overlay de 13-63 px toutes les 3 s. Le fix de la suite 123 (périodique → géométrique seul sans modèle) a **contourné** le symptôme ; ce document creuse la **cause** et chiffre les remèdes.
>
> Simulation reproductible : [`tools/analysis/blob_jitter_sim.py`](../tools/analysis/blob_jitter_sim.py).

---

## 1. Le paradoxe apparent

Le log terrain donne simultanément :
- `median 3.3px` de reprojection sur les inliers (précis) ;
- 13-63 px de déplacement des **coins de la carte** d'un tick à l'autre (pas répétable) — chaque tick franchissait le gate de dérive de 12 px (`kReanchorMinShiftPx`, `Application.cpp`), d'où la ré-application permanente + `resetReference` qui ballottait l'overlay.

Ces deux chiffres ne se contredisent pas : ils ne mesurent **pas la même chose**.
- La médiane de reprojection est mesurée **sur les inliers du fit lui-même** (auto-cohérente, systématiquement optimiste) et **à l'intérieur** du nuage de composants.
- Le gate de dérive mesure le **max sur les 4 coins de la bbox carte** — c'est-à-dire une **extrapolation** hors du nuage, avec la statistique la plus amplificatrice possible (max de 4 valeurs bruitées).

Entre les deux, il y a le fit : `ComponentReanchor::estimate()` termine **toujours** par `cv::findHomography` — un modèle **8 DOF** dont les 2 termes perspectifs (h31, h32) sont, sur cette scène quasi fronto-parallèle, estimés à partir de **bruit pur**. Un chouïa de perspective fantôme ne coûte presque rien au centre du nuage (là où la reprojection est mesurée) mais fait levier énorme aux coins.

## 2. Les trois sources de variance, chiffrées

Simulation calée sur la scène terrain (848×480, ~35 correspondances/tick sur un pool de 60 matchables, biais fixe par composant σ=2,5 px [centre de bbox MSER ≠ centre du corps : corps seul vs corps+ombre vs reflet], bruit frais σ=1 px, sous-ensemble retiré à chaque tick [loterie MSER : 26-41 matchés sur 103-124 détections]). La config « état actuel » reproduit le terrain : medReproj 2,7 px (terrain : 3,3), jitter tick-à-tick 12/21 px méd/p90 (terrain : 13-63).

| Config | medReproj | Coins vs vérité (méd/p90) | Tick-à-tick (méd/p90) |
|---|---|---|---|
| **État actuel** : H 8-DOF, subset rebrassé | 2,7 px | 8,2 / 16,2 px | **11,9 / 20,8 px** |
| H 8-DOF, **même** subset à chaque tick | 2,3 px | 6,5 / 10,9 px | 7,4 / 12,4 px |
| **Similarité**, subset rebrassé | 2,7 px | 2,7 / 3,9 px | **3,0 / 4,9 px** |
| Similarité, même subset | 2,7 px | 3,1 / 4,5 px | 2,1 / 3,4 px |
| H 8-DOF, K=70 matches (meilleur détecteur) | 2,9 px | 7,7 / 15,1 px | 10,6 / 19,3 px |
| Similarité, K=70 | 2,6 px | 1,9 / 3,3 px | 2,4 / 4,3 px |
| H 8-DOF, sans biais par composant (fix centroïde) | 1,8 px | 3,3 / 7,1 px | 4,9 / 10,4 px |
| Similarité, sans biais | 1,8 px | 1,0 / 1,7 px | **1,5 / 2,3 px** |

Lecture, par ordre d'importance :

1. **Cause n°1 — le fit 8-DOF lui-même** (structurel). Passer en similarité divise le jitter par ~4 (11,9 → 3,0 px) **à détections identiques** : le jitter passe **sous le gate de 12 px**, donc le périodique se mettrait à *skipper* naturellement au lieu de secouer. Notable : **plus de matches n'aide presque pas** le 8-DOF (K=70 → 10,6 px) — ce n'est pas un problème de quantité de données, c'est le modèle qui sur-paramètre.
2. **Cause n°2 — le biais par composant × loterie de subset**. Le centre de la **bbox** MSER est décalé du vrai centre d'une quantité *fixe par composant mais différente d'un composant à l'autre* ; quand le sous-ensemble matché change (MSER retrouve 26-41 composants différents à chaque frame), ces biais ne se moyennent pas pareil → la pose bouge. Supprimer ce biais (centroïde de région au lieu de bbox) divise encore par 2.
3. **Cause n°3 — la loterie MSER seule** : réelle mais non dominante (lignes « même subset »).

La déterminisation du RNG de `bootstrap()` (seed fixe, suite 118) n'y peut rien : les **détections d'entrée** changent à chaque frame, donc le consensus change, et de toute façon le raffinage final passe par le RANSAC non-seedé de `findHomography`.

## 3. Remèdes possibles, classés

### A. Fit similarité pour les poses blob ← **recommandé, premier geste**
`ComponentReanchor::Params` gagne un flag (ex. `fitSimilarity`) ; `estimate()` utilise alors `cv::estimateAffinePartial2D` (RANSAC/MAGSAC, même seuil) plongé en 3×3 au lieu de `findHomography`. Activé quand la source = blobs (le chemin modèle peut garder le 8-DOF, ou suivre `trackingModel` comme le worker — dont le mode Auto choisit *déjà* la similarité sur carte plane, exactement pour cette raison).
- **Gain** : jitter tick-à-tick 12 → 3 px (sous le gate) **et** précision one-shot aux coins 8 → 3 px — l'alignement initial blob devient visiblement meilleur, pas seulement le périodique.
- **Coût** : ~20 lignes. **Risque** : si la caméra vise la carte avec un vrai angle, la similarité sous-fitte (résidu aux coins). Mitigation simple : fitter les deux et ne garder le 8-DOF que s'il réduit *significativement* la reprojection médiane.
- Débloque potentiellement la **ré-activation du périodique sans modèle** (annulerait le contournement de la suite 123).

### B. Centroïde de région MSER au lieu du centre de bbox
`cv::MSER::detectRegions` renvoie déjà les pixels des régions ; le centroïde de masse est bien plus stable que le centre d'une bbox (pilotée par les pixels extrêmes : ombre, reflet, silk accolé). S'attaque à la cause n°2. ~15 lignes dans `BlobComponentDetector.cpp` (attention : le dédup par proximité doit alors fusionner les centroïdes, p.ex. garder la région la plus grande).

### C. Confirmation à 2 ticks pour le périodique (ceinture)
Même pattern que le jump gate du `TrackingWorker` : n'appliquer une correction silencieuse que si **2 poses blob consécutives concordent entre elles** (coins à < ~8 px) *et* s'écartent toutes deux de la pose courante. Transforme un estimateur bruité en détecteur de dérive fiable — utile même après A/B, contre les ticks aberrants (main dans le champ, reflet).

### D. Ne pas `resetReference` sur petite correction
Le « reference captured » à chaque application ajoutait son propre à-coup. Rebaser la pose sans réamorcer l'ORB serait plus doux mais demande de toucher le protocole worker↔Application — à garder pour plus tard.

### E. (pis-aller) Monter le gate à ~40 px si on garde le 8-DOF
Rendrait le périodique blob non-secouant mais aveugle aux dérives < 40 px — autant le laisser désactivé (état actuel post-suite 123).

## 4. Limites honnêtes
- C'est une **simulation calée**, pas une mesure : les σ (biais 2,5 px, bruit 1 px) sont choisis pour reproduire les observables terrain (medReproj ≈ 3 px, jitter ≈ 12-21 px ; les pics à 63 px du log = churn d'inliers RANSAC / mauvais ticks, non modélisé finement). La conclusion **structurelle** (8-DOF amplifie aux coins, similarité non ; plus de matches ne sauve pas le 8-DOF) est robuste au choix des paramètres.
- La validation réelle de A/B exige le même protocole que la suite 123 : log verbose, carte plein cadre, Auto re-anchor coché, sans modèle — chercher la fréquence des `Periodic re-anchor: pose within 12px … skipping` (bon signe) vs `correcting drift` (mauvais signe s'il est permanent).
- Le modèle entraîné reste la vraie sortie haut de gamme (détections répétables par construction) ; A/B rendent le chemin blob **utilisable en continu**, pas parfait.
