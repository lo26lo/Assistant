# Plan — Modèle V2 : d'un détecteur générique inutile à un modèle qui sert vraiment

> **Date** : 2026-07-10 · **Statut** : plan validé à écrire → exécution par phases
> **Contexte** : le modèle Piste B du [TUTO_MODELE_REANCRAGE.md](TUTO_MODELE_REANCRAGE.md)
> est chargé et tourne (TensorRT, 85 ms/frame — vérifié via `Dev → Test AI detection`,
> suite 149) mais est **inutile dans l'état** : détecteur de *présence de composants
> génériques* (dataset Roboflow SMD étranger) → 1 détection sur la carte nue FOC_Slim.
> **Documents liés** : [DATASET_CREATOR_PLAN.md](DATASET_CREATOR_PLAN.md) (la capture
> auto-annotée, Phase A implémentée), [AI_PIPELINE.md](AI_PIPELINE.md),
> [COMPONENT_REANCHOR_ALIGNMENT.md](COMPONENT_REANCHOR_ALIGNMENT.md) (l'alignement qui
> consommera le modèle).

---

## 1. Pourquoi le modèle actuel est inutile (3 raisons précises)

1. **Carte nue** : il détecte des *corps de composants* — l'assemblage main commence
   carte nue, il n'y a rien à détecter. (Le chemin pads CV couvre ce cas depuis les
   suites 140-142 ; le fix suite 148 garantit que le modèle ne le désactive plus.)
2. **Domaine étranger** : entraîné sur des images Roboflow (autres cartes, autres
   éclairages, autres caméras) — jamais vu une scène D405 848×480 sombre.
3. **Mono-classe** : « component » partout → n'apporte **rien** contre l'ambiguïté
   d'orientation 180° (le problème n°1 du terrain), car toutes les correspondances
   se valent. La plomberie classe-par-classe existe pourtant déjà côté matching
   (`ComponentReanchor::Params::useClassPrior` + `classOfComponent`) — **jamais
   branchée** (`{}` aux deux sites d'appel).

## 2. Objectif du modèle V2 (par ordre de valeur)

| # | Capacité | Ce que ça débloque |
|---|----------|--------------------|
| a | Détecter **les composants de TES cartes** dans TES conditions | Re-anchor/alignement robuste carte peuplée (mieux que MSER) |
| b | **Multi-classes** (liste `resources/footprint_classes.json`, déjà ordonnée) | Matching classe-à-classe → **tue l'alias 180°** (un connecteur ne peut pas matcher une résistance) |
| c | (Option) classe **« pad »** apprise | Remplace le top-hat CV sur les scènes difficiles (sombre/reflets), carte nue comprise |
| d | Base pour l'inspection (Piste A du tuto) | Présence/absence par référence, à terme |

## 3. Le levier central : l'app fabrique le dataset toute seule

Le **DatasetCreator** (Phase A, déjà dans l'app — dock « Dataset ») capture des
frames et génère les **labels YOLO automatiquement** en projetant les bboxes iBOM
via l'homographie live, avec gates qualité (inliers ≥ 25, reproj ≤ 3 px, netteté,
exposition, fraîcheur). L'alignement carte nue étant maintenant fiable
(contour+pads, suites 140-144), la chaîne complète est : **aligner → poser des
composants → capturer → labels gratuits**. Zéro annotation manuelle.
⚠️ Phase A « à valider sur Jetson » : ce plan est sa première vraie utilisation —
prévoir une session de rodage.

## 4. Les phases

### Phase 0 — Évaluer l'existant (banc, ~15 min) — MAINTENANT
- `Dev → Test AI detection` sur une **carte peuplée** (ou poser 5-10 composants
  sur la FOC_Slim). Noter : nb de détections / composants visibles, confiances,
  faux positifs. Refaire à confiance 0.30 (slider AI).
- **Décision** : si le modèle voit ≥ ~50 % des composants → base de fine-tuning
  valable (Phase 2 partira de son `best.pt`). Sinon → repartir de `yolov8n.pt`.

### Phase 1 — Dataset de TES cartes (banc, 2-3 sessions de ~20 min)
- Carte **peuplée** + iBOM chargé + alignement verrouillé → `Dataset : Start`.
- Varier entre sessions : éclairage (2-3 réglages), rotation de la carte
  (0/90/180/270), hauteur/zoom, les deux faces si double-face.
- Cible : **300-800 frames** réparties sur ≥ 3 sessions (le split train/val se
  fait **par session**, jamais par image — sinon fuite de données).
- Sortie : `$IBOM_DATA_DIR/dataset/session_*/` au format attendu par
  `tools/dataset_studio/` (validation visuelle des boxes incluse).
- (Option c) 1 session carte **nue** en plus — inutilisable tant que le
  DatasetCreator ne labellise pas les pads (voir Phase 3b), mais gratuite à
  capturer maintenant.

### Phase 2 — Entraînement V2 (PC RTX 5070, 1-2 h de GPU)
- **V2b-public — disponible SANS attendre la Phase 1** : les datasets publics
  (TUTO_DATASETS §1) suffisent pour apprendre les **types** de composants
  (résistance/condo/IC/connecteur… généralisent bien d'une carte à l'autre,
  avec leurs formes de pads). Chemin : `fetch_roboflow_dataset.py` × 2-3 →
  `remap_classes.py --map` (projette les noms de classes source sur nos
  **14 classes canoniques** — modèle : `scripts/class_mapping.example.yaml`)
  → `merge_datasets.py` → `train_yolo.py`. C'est la réponse à « peut-on
  utiliser ces datasets » : oui, pour le multi-classes, dès maintenant.
- `merge_datasets.py` : Roboflow SMD (généralisation) + tes sessions
  (spécifique, quand la Phase 1 les aura produites).
- **V2a (rapide)** : rester presence-1-classe, fine-tune depuis le `best.pt`
  actuel (`train_yolo.py … --model best.pt`) → gain immédiat carte peuplée.
- **V2b (le vrai gain)** : multi-classes via le mapping
  `resources/footprint_classes.json` (⚠️ liste ordonnée — ne jamais réordonner,
  les ids YOLO en dépendent). Le DatasetCreator émet déjà ces classes.
- Export : `export_yolov8_onnx.py … --out models/component_detector.onnx`
  (letterbox déjà géré app-côté depuis la suite 129) → copier `.onnx` + `.txt`
  sur le Jetson, vider `data/tensorrt-cache/` si le modèle change de forme.

### Phase 3 — Code (sessions Claude, en parallèle de 1-2)
- **3a. Brancher `useClassPrior` — ✅ FAIT (suite 151)** : `buildClassPrior()`
  (Application) mappe chaque composant iBOM → classe canonique (via le
  `ClassMapper` partagé avec le DatasetCreator) → **id de classe du MODÈLE**
  (lookup par nom dans le `.txt` du modèle, robuste à l'ordre) ; passé aux
  `estimate()`/`bootstrap()` des tentatives composants, activé seulement si
  modèle multi-classes chargé. Test : un alias 180° **géométriquement
  parfait** (layout symétrique) est verrouillé sans le prior, refusé avec —
  et la vraie pose locke à 20/20 sous la même contrainte. S'active tout seul
  dès qu'un modèle V2b est déposé.
- **3b. (Option c) Labels « pad » dans le DatasetCreator** : projeter aussi les
  pads iBOM (la constellation existe — suite 136) pour les sessions carte nue →
  permet d'entraîner la classe « pad ». Petit ajout, même mécanique que les bboxes.
- **3c. Éval automatisée** : script comparant détections ↔ vérité iBOM projetée
  sur les sessions capturées (précision/rappel par classe) — chaque itération de
  modèle devient mesurable au lieu de « j'ai l'impression que ».

### Phase 4 — Validation terrain (critères de succès mesurables)
- Carte **peuplée** + modèle V2 : locks `components(model)` à ratio ≥ ~0.8,
  re-anchor stable 1 min sans saut.
- **180°** : carte tournée puis Auto-Align → bonne orientation à chaque essai
  (grâce à 3a + V2b) — le test qui échoue aujourd'hui.
- Carte **nue** : comportement inchangé (pads CV) ou meilleur (3b).
- Latence : ≤ ~100 ms/inférence (85 ms mesurés aujourd'hui → OK).

## 5. Ordre recommandé & qui fait quoi

| Étape | Qui | Quand |
|-------|-----|-------|
| Phase 0 (éval 15 min) | Toi, au banc | Maintenant |
| Phase 1 session 1 (rodage DatasetCreator) | Toi, au banc | Ensuite |
| 3a (useClassPrior) + 3c (éval) | Claude | En parallèle, dès validation du plan |
| Phase 2 V2a puis V2b | Toi (PC RTX) | Après 2-3 sessions capturées |
| 3b (labels pads) | Claude | Si l'option c est retenue |
| Phase 4 | Ensemble | Après chaque modèle |

## 6. Pièges connus (déjà payés une fois — ne pas re-payer)

- Split **par session**, pas par image (fuite train/val).
- `footprint_classes.json` et `tools/dataset_studio/config/pcb_classes.json` :
  **même liste ordonnée**, ne pas réordonner.
- Premier lancement d'un nouveau `.onnx` = compilation TensorRT (minutes) ;
  cache sous `data/tensorrt-cache/` à vider si la forme du modèle change.
- Ne pas entraîner sur le Jetson (tuto : PC RTX 5070 ou Colab).
- Le `.txt` de classes doit accompagner le `.onnx` (même nom).
