# Analyse étape par étape du Live Tracking — juillet 2026

> **But** : audit complet du pipeline de live tracking **tel qu'il est réellement écrit** (post PR #20 + suites 92–101), étape par étape, pour identifier ce qui limite encore la qualité perçue et proposer des améliorations priorisées.
>
> **Méthode** : lecture statique du code (pas de toolchain ici — chaque proposition devra être validée au build Jetson). Les références `fichier:ligne` correspondent à l'état du dépôt au 2026-07-01 sur `claude/live-tracking-analysis-tr2h3j`.
>
> **Documents liés** : [LIVE_TRACKING_PLAN.md](LIVE_TRACKING_PLAN.md) (plan Phases 1–3, implémenté en PR #20) · [JETSON_SESSION_LOG.md](JETSON_SESSION_LOG.md) (suites 94–101 : flicker, dé-throttle flow, re-anchor, diagnostic overlay ~1 s) · [JETSON_ERREURS.md](JETSON_ERREURS.md) (#35 masque board, #47 jitter, **#51/#52 ouverts par cette analyse**)

---

## 0. État des lieux — ce que disent les faits

Les logs verbose de la suite 100 (D405, carte plein cadre) montrent que **le cœur d'estimation est excellent** quand le chemin optical-flow est actif : `[track] EMIT inliers=171-200 reproj<0.06px`. Le goulot était le **rendu overlay** (~0,6–1,3 s par rendu, AA software) — fixé en suite 100 (AA off + cap 25 fps), **validation par re-capture encore en attente**.

Conclusion d'orientation : les gros gains restants ne sont **pas** dans l'algorithme d'estimation lui-même, mais dans :
1. **les défauts de config** (le bon chemin n'est pas celui qu'un utilisateur obtient out-of-the-box) ;
2. **la robustesse aux cas limites** (perte / ré-acquisition, estimées aberrantes, outliers persistants) ;
3. **l'architecture d'affichage** (coût du rendu + synchronisation overlay↔frame).

---

## 1. Le pipeline, étape par étape

```
CameraCapture (thread capture, 30 fps)
  └─ frameReady(FrameRef zero-copy)  ──queued──►  GUI thread (Application.cpp:1703)
       ├─ 1. post vers TrackingWorker (1744-1747)   ──queued──►  thread tracking
       │        2. cvtColor gray (+CLAHE opt)                (TrackingWorker.cpp:567-579)
       │        3. fast-path LK optical flow (si activé)     (:584-587 → runOpticalFlow :414)
       │        4. throttle ORB (intervalMs)                 (:595-601)
       │        5. downscale ×0.5                            (:603-607)
       │        6. masque board (dernier H connu, ×1.6)      (:615 → buildBoardMask :137)
       │        7. ORB detect (cap 2×target, GPU si dispo)   (:619 → detectFeatures :367)
       │        8. bucketing 8×6 + cornerSubPix full-res     (:629, :634)
       │        9. match Lowe (knn k=2, one-way)             (matchPoints :512)
       │       10. estimateModel (MAGSAC / Auto sim-vs-homog) (:195-242)
       │       11. emitHomography: gate qualité → gate statique → 1€ → EMIT (:314-346)
       │                └──queued──►  GUI: setMatrix + updateDynamicScale + minimap (1599-1608)
       └─ 12. affichage: undistort → QImage → CameraView ; overlay re-rendu si signature
              change, cap 40 ms (1810-1898 → OverlayRenderer::render)
```

Verdict par étape :

| # | Étape | Verdict | Détail |
|---|-------|---------|--------|
| 1 | Handoff frame → worker | ⚠️ | Aucune backpressure : chaque frame est postée, la file peut s'allonger (**F6**) |
| 2 | Gray + CLAHE | ✅ | Avant throttle : voulu pour le flow ; gaspillage mineur en mode ORB pur throttlé |
| 3 | Fast-path LK | ⚠️ | Sain sur le principe (dé-throttlé suite 95), mais garde les outliers RANSAC (**F3**), pas de contrôle forward-backward (**F9**), re-seed = micro-saut périodique (**F8**) |
| 4 | Throttle ORB | ✅ | Correctement placé après le flow ; 200 ms par défaut = 5 Hz si flow off (**F1**) |
| 5 | Masque board | 🔴 | **Pas de récupération si le masque devient faux** — perte définitive possible (**F2**, ERREUR #51) |
| 6 | ORB detect | ✅ | Cap 2× + fallback GPU→CPU propres |
| 7 | Bucketing + subpix | ✅ | Grille 8×6 + top-up global ; cornerSubPix borné et try/catch |
| 8 | Matching Lowe | ✅ | One-way suffit avec MAGSAC derrière ; RAS |
| 9 | Estimation modèle | ✅/⚠️ | MAGSAC + médiane inliers = bon ; mais le delta incrémental ignore `m_model` (**F10**) |
| 10 | Gates + lissage | ⚠️ | Gate inliers OK, gate statique OK (erreur bornée 0,8 px) ; **aucune garde anti-saut** sur estimée aberrante (**F4**) ; état jamais « Locked » en mode référence (**F5**) |
| 11 | Application GUI | ⚠️ | `updateDynamicScale` à chaque émission, méthode IBomPads boguée + O(pads) (**F7**, ERREUR #52) |
| 12 | Rendu overlay | ⚠️ | Fixes suite 100 en place mais l'architecture reste « re-tessellation vectorielle par frame » (**F11**), transformée point-par-point coûteuse (**F13**), overlay en retard d'1-2 frames sur la vidéo (**F12**) |

---

## 2. Findings détaillés

### P0 — réglage pur, zéro code risqué

**F1 — Les défauts Config laissent le chemin legacy (le plus mauvais) actif.**
`Config.h:366-377` : `trackingIntervalMs=200`, `trackingOpticalFlow=false`, `trackingModel=3` (Homography 8 DOF), `clahe=false`. Autrement dit : **out-of-the-box le suivi tourne à 5 Hz, sans flow, avec le modèle le plus bruité** — exactement le comportement « ça suit mal et pas rapide » de la suite 94. Le chemin validé excellent (suite 100) exige de cocher Optical-flow à la main.
*Proposition* : basculer les défauts vers `opticalFlow=true`, `trackingModel=0` (Auto — en pratique similarité sur carte plane, homographie seulement si la perspective est réelle), `intervalMs=100` (l'ORB ne sert plus que de re-seed ~10 Hz max). Attention : les `config.json` **existants** conservent leurs valeurs — prévoir soit une migration douce (clé `config_version`), soit un bouton « Recommended defaults » dans Settings → Tracking.
*Effort* : XS. *Risque* : faible (comportement déjà validé sur Jetson quand activé manuellement).

### P1 — robustesse, code faible risque

**F2 — Masque board sans récupération = perte définitive possible (🔴 ERREUR #51).**
`buildBoardMask` (`TrackingWorker.cpp:137-171`) projette le polygone carte via `m_lastHomography`, qui n'est mis à jour **que sur estimation réussie** (`:459`, `:681`, `:789`). Si la carte sort de la zone masquée (déplacement rapide entre deux ticks ORB à 5 Hz, main qui déplace la carte, bump de la caméra), l'ORB ne détecte plus que la zone (fausse) du masque → plus jamais de match → `m_lastHomography` ne se met plus jamais à jour → **boucle morte silencieuse**. En mode référence il n'existe **aucun compteur d'échec** (`m_lostFrames` ne sert qu'à l'incrémental) et `processReference` sort par des `return` muets (`:662`, `:667`).
*Proposition* : compter les échecs consécutifs du chemin référence ; après N échecs (ex. 5), repasser en **détection plein cadre** (masque vide) jusqu'à ré-acquisition ; optionnellement élargir la marge 1.6× → 2.5× au premier palier. ~15 lignes localisées.
*Effort* : S. *Risque* : faible.

**F3 — Le set de landmarks optical-flow garde les outliers RANSAC.**
`runOpticalFlow` (`TrackingWorker.cpp:452-455`) conserve pour la frame suivante **tous** les points survivants du LK (`keptPcb/keptImg`), y compris ceux que `estimateModel` vient de classer outliers — le commentaire (« the fit already rejected fliers ») confond *rejeté du fit* et *retiré du set*. Un point qui a glissé sur une feature voisine reste donc dans le set **jusqu'à 30 frames** (prochain re-seed) et vote contre le bon modèle à chaque frame.
*Proposition* : faire retourner le masque d'inliers par `estimateModel` (param de sortie optionnel) et pruner le set à chaque frame ; re-seed anticipé si le set tombe sous ~2×`minMatchCount`.
*Effort* : S. *Risque* : faible.

**F4 — Aucune garde anti-saut sur l'homographie émise.**
`emitHomography` (`TrackingWorker.cpp:314-346`) gate sur `inliers < minMatchCount` et sur la scène statique, mais **pas sur l'amplitude du changement ni sur la santé géométrique de H**. Une estimée dégénérée à 8 inliers (configuration quasi-colinéaire, reflet) passe le gate, le 1€ filter **suit le saut** (sa coupure s'ouvre avec la vitesse — c'est sa fonction) → overlay qui « explose » une frame puis revient. `reprojErr` n'est d'ailleurs pas gaté du tout.
*Proposition* : (a) rejeter si `reprojErr > ransacThreshold` ; (b) si `cornerDisp(raw, m_lastEmittedH)` dépasse un seuil (ex. 15 % de la diagonale image), exiger **2 estimées consécutives concordantes** avant d'accepter (vrai mouvement brusque = confirmé dès la frame suivante ; estimée folle = isolée) ; (c) sanity : `det(H) > 0`, termes perspectifs bornés.
*Effort* : S. *Risque* : faible (le cas nominal ne change pas).

**F5 — Le badge d'état ne passe jamais « Locked » en mode référence pur.**
`processReference` ne fait **aucun** `setState` (seuls `runOpticalFlow:460`, `processIncremental` et le gate low-quality en font). Après `resetReference` → `State::Lost`, un suivi ORB pur qui fonctionne parfaitement laisse l'état sur Lost → la status bar peut afficher « Tracking: LOST — re-anchor » alors que tout va bien.
*Proposition* : `setState(Locked)` sur émission réussie dans `processReference` (2 lignes) — le badge devient fiable dans les 3 modes.
*Effort* : XS. *Risque* : nul.

**F6 — Pas de backpressure sur la file du worker.**
`Application.cpp:1744-1747` poste **chaque frame** en queued connection. Le throttle interne rend les frames en retard bon marché en mode ORB pur, mais avec le flow actif chaque frame en retard subit quand même `cvtColor` full-res (+CLAHE éventuel) + LK + fit. Si le coût par frame dépasse la période caméra (microscope 1080p, CLAHE on, CPU chargé par l'IA), la file s'allonge **sans borne** → latence croissante, jamais résorbée.
*Proposition* : compteur atomique in-flight partagé (incrément au post, décrément en tête de `processFrame`) ; si ≥ 2 en vol, ne pas poster (le flux converge vers « toujours la frame la plus fraîche »). ~10 lignes.
*Effort* : S. *Risque* : faible.

**F7 — `updateDynamicScale` : méthode IBomPads boguée + scan par émission (🔴 ERREUR #52).**
`Application.cpp:3728-3740` : `bestDist` est **remis à 0 à chaque composant** et `padB` écrasé → `padB` = pad le plus éloigné de `padA` *dans le dernier composant à pads*, pas globalement (deux pads voisins possibles → échelle instable/fausse). De plus la fonction est appelée **à chaque émission** (jusqu'à ~30 Hz avec flow) et la branche IBomPads scanne tous les pads de la carte à chaque fois.
*Proposition* : corriger la recherche (max global, calculé **une fois** au chargement de l'iBOM et caché) + throttler `updateDynamicScale` à ~5 Hz. La méthode par défaut (Homography) n'est pas affectée par le bug mais bénéficie du throttle.
*Effort* : S. *Risque* : nul.

### P2 — fluidité / précision fine

**F8 — Re-seed flow toutes les 30 frames = micro-saut périodique (~1 Hz).**
`processFrame:584-587` saute le fast-path quand `m_flowFramesSinceDetect ≥ 30` → passe ORB → `processReference` émet **sa propre** estimée puis `seedFlowLandmarks` remplace tout le set. L'estimée ORB et le fit flow diffèrent de quelques dixièmes de px → petit à-coup visible à cadence fixe sur scène quasi statique (partiellement absorbé par gate statique + 1€).
*Proposition* : piloter le re-seed par l'**attrition du set** (re-seed quand < 2×minMatch survivants) plutôt qu'un compteur fixe ; et lors d'un re-seed sur suivi sain, re-seeder les landmarks **sans émettre** l'estimée ORB (le flow ré-émet dès la frame suivante, continuité préservée). Garder un re-seed forcé long (ex. 120 frames) comme filet anti-dérive.
*Effort* : M. *Risque* : moyen (à valider au Jetson avec les logs `[track]`).

**F9 — LK sans contrôle forward-backward.**
`runOpticalFlow:437-442` filtre sur `status` et `err > 20` (lâche — c'est une différence d'intensité de patch, pas une erreur géométrique). Le mode de défaillance classique du LK — glissement sub-pixel progressif vers une feature voisine — n'est détecté par rien.
*Proposition* : tracker aussi le chemin retour (`fullGray → m_prevGray`) et rejeter si ‖aller-retour − départ‖ > 0,5 px (critère MedianFlow standard). Double le coût LK (~qq ms sur ≤200 pts, budget OK sur Orin) pour une pureté de set nettement meilleure — synergique avec F3.
*Effort* : S. *Risque* : faible.

**F10 — Le delta incrémental est toujours une homographie 8 DOF.**
`processIncremental:776` appelle `cv::findHomography(MAGSAC)` directement, **ignorant `m_model`**. En composition frame→frame, le bruit perspectif de chaque delta se **multiplie** dans `m_cumulativeH` → la dérive microscope est plus rapide qu'elle ne devrait. Une similarité par delta (le mouvement inter-frame au microscope est ~rigide) composera un bruit bien moindre.
*Proposition* : utiliser `estimateModel` pour le delta (respecte `m_model`, Auto → similarité en pratique). Le chemin hybride (anchor snap) reste inchangé et continue d'absorber la dérive résiduelle.
*Effort* : XS (le helper existe déjà). *Risque* : faible.

### P3 — architecture (les vrais « faire mieux » structurels)

**F11 — Rendu overlay : re-tessellation vectorielle à chaque frame → passer au warp d'image.**
Aujourd'hui chaque mise à jour d'homographie re-projette et re-dessine **tous les pads/segments/labels visibles** (`OverlayRenderer.cpp:38-158`), capé à 25 fps, AA off pour tenir le budget. Or l'overlay en **espace carte ne change presque jamais** (seulement sélection / placed / toggles / couleurs). QPainter supporte les `QTransform` **projectifs** : on peut rendre l'overlay **une fois** en espace carte (haute résolution, AA on !), puis à chaque paint le blitter avec `painter.setTransform(H)` — un seul warp d'image par frame, coût quasi constant, **toujours collé à l'homographie courante au moment du paint**.
Gains : GUI thread quasi libéré (plus besoin du cap 40 ms ni de la signature homographie), AA réactivable (qualité visuelle ↑), et suppression structurelle du lag de rendu.
Points à trancher : les labels seraient warpés avec la carte (style AR — probablement mieux au microscope) ou dessinés en passe séparée légère (upright, culling par taille projetée) ; résolution du buffer carte à dimensionner (~2× la taille projetée max).
*Effort* : M-L. *Risque* : moyen (rendu — validation visuelle Jetson obligatoire).

**F12 — Pas de timestamps de capture → ni resync overlay↔frame, ni prédiction possible.**
L'homographie appliquée provient d'une frame **plus vieille** que celle affichée (latence worker + 2 queued hops + prochain frameReady) : en mouvement, l'overlay traîne systématiquement de 1-2 frames (33-66 ms) derrière la vidéo — une part du « ça suit mal » perçu qui ne vient **pas** de l'estimation. `FrameRef = shared_ptr<const cv::Mat>` ne porte aucun horodatage, donc on ne peut ni mesurer cette latence, ni la compenser.
*Proposition (fondation)* : horodater à la capture (side-channel `frameReady(FrameRef, qint64 tCaptureNs)` — évite de toucher le métatype FrameRef). Ensuite, au choix : (a) prédiction vitesse-constante des coins sur la latence mesurée (anti-lag), (b) drop-if-stale dans le worker (renforce F6), (c) alimenter le 1€ avec les **vrais** dt de capture au lieu de `steady_clock::now()` au moment du traitement (`TrackingWorker.cpp:485-486`) — supprime le bruit de latence de traitement injecté dans le filtre.
*Effort* : M (touche CameraCapture + signatures). *Risque* : moyen.

**F13 — `Homography::pcbToImage` point-par-point : 2 vecteurs + `cv::perspectiveTransform` PAR POINT.**
`Homography.cpp:56-64` — et l'overlay l'appelle des **milliers de fois par rendu** (2× par segment silk, 1× par label, via `transformRect` pour chaque pad). Chaque appel = 2 constructions de `std::vector`, wrapping Mat, dispatch OpenCV… pour un produit 3×3.
*Proposition* : inliner la transformée (cacher les 9 coefficients en double au `setMatrix`, produit + division perspective à la main) → ~×50 sur cette portion, profite au rendu overlay, à la minimap, à `updateDynamicScale`, au hit-testing. Trivial à tester unitairement (`test_homography.cpp` existe déjà).
*Effort* : S. *Risque* : faible.

### Notes transverses (pas des findings à part entière)

- **Référence jamais rafraîchie en mode référence** : si l'éclairage/le focus dérivent longtemps, le matching ref→cur se dégrade sans recours automatique. C'est le rôle assumé des re-anchors périodiques (BoardLocator plan B, ComponentReanchor suite 103 — le vrai fix long terme) ; pas d'action worker recommandée pour l'instant.
- `m_staticThreshPx` (0,8) et `m_flowRedetectInterval` (30) sont codés en dur — à exposer en config avancée seulement si le tuning terrain le demande.
- `estimateAffinePartial2D` ne supporte pas USAC (RANSAC classique, seedé) — OK, juste à savoir.
- Gate Auto (`he < se*0.7 && hi >= si`, `TrackingWorker.cpp:236`) : biaisé similarité par construction — comportement voulu (stabilité), s'ouvre à l'homographie quand la perspective est réelle (caméra inclinée).
- Tests : seulement 2 `TEST_CASE` sur le worker. Candidats à haute valeur : gate statique (rien n'est émis sous 0,8 px), gate qualité, récupération masque (F2), choix de modèle Auto sur données planes, flow avec outlier injecté (F3), anti-saut (F4).

---

## 3. Plan d'action recommandé

Par lots compilables/validables en une session Jetson chacun, du meilleur ROI au plus structurel :

| Lot | Contenu | Effet attendu |
|-----|---------|---------------|
| **A (quick wins)** | F1 défauts + F5 badge + F2 masque fallback + F7 fix scale | Le comportement « bon » devient celui par défaut ; plus de perte définitive ; badge fiable |
| **B (robustesse flow)** | F3 prune outliers + F9 FB-check + F4 anti-saut | Suivi flow durablement propre ; plus d'« explosions » d'overlay |
| **C (perf affichage)** | F13 transform inline, puis F11 warp overlay (si la re-capture post-suite-100 montre encore un coût overlay significatif) | GUI thread libéré, AA réactivable, cap 40 ms supprimé |
| **D (fin de polish)** | F8 re-seed piloté attrition + F10 delta similarité + F6 backpressure | Micro-saut 1 Hz éliminé ; dérive microscope ralentie ; latence bornée |
| **E (fondation)** | F12 timestamps capture (+ 1€ sur dt réels, prédiction optionnelle) | Overlay synchronisé/prédit — dernier verrou du « ça suit mal » |

**Protocole de validation** (reprend LIVE_TRACKING_PLAN.md §6, avec l'outillage suite 99 déjà en place) :
1. Dev → Verbose debug logging → re-capture de référence AVANT tout lot (mesurer `[overlay] … in X.Xms` post-suite-100 — décide si F11 est nécessaire).
2. Test immobile 30 s : zéro EMIT attendu après stabilisation (gate statique).
3. Test mouvement lent + rapide : pas de lag perceptible, pas de saut ; après F2 : déplacer brusquement la carte hors masque → ré-acquisition < 1 s.
4. Test perturbation (main, glare) : overlay **fige** puis reprend, ne saute jamais (F4).
5. Microscope incrémental (V4L2) : dérive accumulée avant/après F10 sur un aller-retour de pan identique.
