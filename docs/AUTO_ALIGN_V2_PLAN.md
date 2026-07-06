# Auto-Align V2 — architecture « détection-first »

> **Contexte** (2026-07-02, suite 118) : retour utilisateur — « c'est peu d'amélioration pour l'auto-align, je pensais que tu ferais beaucoup plus quitte à tout repenser ; et pour le live, je suis toujours loin d'un yolov8 ». Ce document EST la refonte : l'alignement et la récupération passent de « détection de contour » à « enregistrement de constellation de composants », le mode de fonctionnement des pipelines YOLO-like.
>
> **Documents liés** : [AUTO_ALIGN_PLAN.md](AUTO_ALIGN_PLAN.md) (V1, BoardLocator — reste le fallback sans modèle) · [AI_MODEL_DATASETS_PLAN.md](AI_MODEL_DATASETS_PLAN.md) (pistes A/B du modèle) · [TUTO_MODELE_REANCRAGE.md](TUTO_MODELE_REANCRAGE.md) (produire le .onnx pas-à-pas) · [LIVE_TRACKING_ANALYSE_2026-07.md](LIVE_TRACKING_ANALYSE_2026-07.md)

---

## 1. Pourquoi le contour (V1) a un plafond bas — et pourquoi on ne l'élèvera pas

`BoardLocator` cherche le **rectangle de la carte** (plan de profondeur D405 ou contour Canny), puis désambiguïse l'orientation par recouvrement d'arêtes. Trois limites **structurelles**, pas des bugs :

1. **Carte plein cadre** (D405 en gros plan, microscope) : pas de bords visibles → rien à détecter. Confirmé terrain (suite 98, spam WARN).
2. **Fond encombré / carte brillante** : le score de recouvrement d'arêtes est faiblement discriminant (un quad décalé attrape encore ~1/3 des arêtes) — d'où le seuil de confiance 0.45 et les faux placements.
3. **Information pauvre** : un rectangle nu a 8 ordres de coins possibles ; toute l'information d'orientation vient du scoring, le maillon faible.

Retoucher V1 (multi-frames, meilleurs scores) grappille des % ; ça ne franchit aucune de ces trois murailles. **La refonte change la source d'information : les composants eux-mêmes.**

## 2. L'architecture V2 : une constellation, pas un contour

La carte porte des dizaines de composants dont on connaît **exactement** la position (iBOM). Un détecteur de composants (YOLOv8n exporté ONNX → TensorRT, pipeline déjà câblé dans `ai/`) donne leur position dans l'image. Aligner = **apparier deux nuages de points** — un problème bien posé même carte plein cadre, sans aucun bord visible, insensible au fond.

### Le chaînon qui manquait : `ComponentReanchor::bootstrap()`

`ComponentReanchor::estimate()` (suite 103) exigeait une **pose courante comme prior** (gating à 60 px autour des positions prédites) : il ne savait que *corriger* une pose, jamais en *créer* une — inutilisable pour l'alignement initial, et inutilisable après une perte totale (pose périmée → plus rien dans le rayon). C'était LE trou de l'architecture.

`bootstrap()` (implémenté suite 118, `src/overlay/ComponentReanchor.{h,cpp}`) résout le problème global **sans prior** :

- **Hypothèses RANSAC paire→paire** : une paire de détections appariée à une paire de composants détermine complètement une similitude (échelle + rotation + translation). On tire ~3000 hypothèses (paires éloignées seulement — baseline courte = bruit).
- **Prior d'échelle physique optionnel** : sur D405, `fx / distance_depth` donne le px/mm réel → les hypothèses hors ±~60 % sont rejetées d'office (moins d'itérations utiles perdues, moins d'alias). Sans prior, ça marche aussi (fenêtre large).
- **Consensus** : composants projetés par l'hypothèse → appariement glouton 1-à-1 aux détections dans une tolérance **physique** (1.2 mm convertie en px par l'échelle de l'hypothèse — valable du grand angle au microscope). Seuil : `max(minMatches, 25 % du plus petit nuage)`.
- **Raffinage + validation** : la similitude gagnante devient le prior d'`estimate()` — appariement exact, `findHomography(RANSAC)`, gates inliers ≥ 8 / reproj médiane ≤ 8 px existants.
- **Déterministe** (RNG seedé) : même scène → même pose ; testable.
- Coût : ~20-40 ms sur Orin (thread QtConcurrent, jamais le GUI).

**Limite documentée** : modèle « présence » (1 classe) + layout très répétitif (grille régulière de passifs identiques) peut aliaser vers une pose symétrique. La validation borne les dégâts ; le vrai remède est le modèle à classes (piste A).

### Un seul moteur, quatre usages

| Usage | Chemin | État |
|---|---|---|
| **Alignement initial** (bouton Auto-Align) | détections (modèle **ou blobs**) → `bootstrap()` → raffinage ; **fallback BoardLocator** si pas de lock. Score mappé sur les gates existants (0.45/0.5). | ✅ 118 / model-free 121 |
| **Récupération de perte** (état LOST, chaîne suite 117) | `componentReanchor(silent)` : `estimate()` avec prior, **fallback `bootstrap()`** — détections modèle **ou blobs** → marche même `models/` vide (scène du log utilisateur) | ✅ 118 / model-free 121 |
| **Correction périodique de dérive** (timer Auto re-anchor) | `componentReanchor()` (modèle **ou blobs**) en primaire ; géométrique retiré du chemin | ✅ 121 |
| **Tracking cadencé** (« feel yolov8 ») | détection à 1-2 Hz fusionnée comme correction absolue, flow LK entre deux — voir §4 | ⏳ après validation du modèle |

L'activation auto du live tracking après tout alignement (suite 117) fait que le bootstrap réussi enchaîne directement sur le suivi.

### Détecteur model-free (blobs) — le fallback sans `.onnx` (suite 121)

`bootstrap()` consomme des `ai::Detection` (centre + bbox). Rien n'oblige à ce
qu'elles viennent d'un réseau : `overlay::detectComponentBlobs()`
(`BlobComponentDetector.{h,cpp}`, vision classique) produit les mêmes détections
à partir des **blobs de corps de composants** — MSER (régions stables, toutes
polarités) + gating par taille physique (via le prior px/mm : ~0,4–22 mm) + dédup
+ cap. `bootstrap()` étant du consensus RANSAC tolérant beaucoup de faux positifs
et de ratés (le test injecte 25 % de dropouts + 6 fantômes), un détecteur de
blobs bruité suffit à verrouiller une pose sur une **carte peuplée**.

Conséquence : **l'alignement basé composants marche même `models/` vide.** Dans
`autoAlignBoard`, `componentReanchor` et le re-anchor périodique, la source de
détections est : **modèle entraîné si présent, sinon blobs**, puis `bootstrap`,
puis (dernier recours) BoardLocator géométrique. La récupération de perte (chaîne
LOST) passe donc par le composant même sans modèle — exactement la scène du log
utilisateur (carte plein cadre coplanaire) que le géométrique ne pouvait pas.

Limites honnêtes du blob : rate les 0201 minuscules, peut tirer sur les joints de
soudure / la sérigraphie, exige des **corps de composants visibles** (carte nue =
échec). Un modèle entraîné reste **meilleur** (précision, robustesse, cartes
peu peuplées) — le blob enlève juste la **dépendance dure** au modèle.

## 3. Le modèle entraîné reste le meilleur chemin (mais n'est plus obligatoire)

Le détecteur blob (§2) donne un chemin **model-free** qui fonctionne dès
maintenant. Un `models/component_detector.onnx` reste supérieur (précision,
cartes peu peuplées, moins de faux positifs). Le pipeline d'entraînement est prêt
depuis les suites 103-107 ; sur le serveur Ubuntu RTX 5070 Ti, c'est **une
commande** :

```bash
# sur le serveur (clé API Roboflow à coller dans le script) :
bash scripts/train_on_server.sh
# → datasets publics → remap "présence" → YOLOv8n 100 epochs → models/component_detector.onnx
```

Puis copier le `.onnx` dans `models/` sur le Jetson et relancer l'app (`ai.enabled` par défaut, init en arrière-plan, 1er lancement long = build engine TRT). Détail pas-à-pas : [TUTO_MODELE_REANCRAGE.md](TUTO_MODELE_REANCRAGE.md). Piste B (présence) suffit pour **tout le §2** ; la piste A (14 classes, fine-tune sur les captures DatasetCreator) enlève l'ambiguïté d'alias et améliore la précision.

**Sans modèle**, Auto-Align = V1 + retry multi-frames (suite 117) — le plafond du §1 reste.

## 4. Roadmap « feel yolov8 » pour le live tracking

Le suivi actuel (flow LK + ORB) est *relatif* : il suit le mouvement depuis une référence, d'où sa fragilité aux occlusions/manipulations (mitigée suites 109-117, mais structurelle). Un détecteur donne une mesure *absolue* par frame. La cible :

1. **Maintenant** (dès le modèle en place) : Auto re-anchor activé avec le détecteur (Settings → Tracking) à intervalle 1-2 s = correction absolue périodique, flow entre deux. Zéro dérive long terme, récupération automatique partout.
2. **Étape suivante** (petite) : cadence adaptative — détection rapprochée (~500 ms) quand le tracking est incertain (peu d'inliers, post-perte), espacée (3-5 s) quand il est sain.
3. **Option** (si 1-2 ne suffisent pas au ressenti) : la détection devient la source primaire (~5-10 Hz TensorRT), le flow ne fait que lisser entre deux — l'équivalent exact d'un pipeline YOLO+tracker. Coût : brancher `bootstrap`/`estimate` sur la cadence du détecteur, gating déjà en place.

## 5. Validation (Jetson)

1. `ctest` : `test_component_reanchor` (3 cas : sans prior, avec prior d'échelle, constellation étrangère rejetée).
2. Sans modèle : Auto-Align inchangé (fallback direct BoardLocator) — non-régression.
3. Avec modèle : (a) carte plein cadre D405, aucun alignement préalable → Auto-Align place l'overlay correctement (log `bootstrap(N consensus)`) ; (b) soulever/reposer la carte → ré-alignement automatique via la chaîne LOST ; (c) carte tournée de 90/180° → pose correcte (pas d'alias) sur une carte non-répétitive.
