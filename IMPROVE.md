# Feuille de route d'améliorations — timestretch-beatmode~

Classé par **impact décroissant** sur la qualité audio. Fais-les dans l'ordre : les premiers points débloquent les suivants (impossible de juger un crossfade si la reconstruction ratio=1 n'est pas propre).

---

## Phase 0 — Préalables de débogage (à faire AVANT de toucher à l'algo)

### 0.1 — Test de reconstruction parfaite à ratio = 1.0
- [ ] Écrire un test automatisé : input = fichier wav, tempo_target == tempo_original, output doit être **bit-à-bit identique** (ou à 1 sample près) à l'input
- [ ] Tant que ce test échoue, ne pas toucher au reste. C'est le canari du code.
- [ ] Raison : si tu ne reconstruis pas parfaitement quand tu ne stretches pas, tu débugues dans le noir

### 0.2 — Mode "dump WAV" dans l'external
- [ ] Ajouter un message `record <filename>` qui écrit la sortie DSP dans un fichier .wav
- [ ] Permet l'analyse offline (spectrogramme, waveform, comparaison Ableton)
- [ ] Indispensable pour tout le reste

### 0.3 — Batterie de signaux de test contrôlés
Générer avec un script Python (librosa/numpy) :
- [ ] Sinus pur 440 Hz, 2 s (détecte clics et discontinuités)
- [ ] Somme 3 sinus (440 + 880 + 1320 Hz) (détecte erreurs de phase)
- [ ] Sinus 440 Hz avec enveloppe exponentielle décroissante (simule note de piano, test sustain)
- [ ] Click unique (1 sample à 1.0) + silence 1 s (test préservation transient)
- [ ] Impulsion + sinus décroissant (kick drum synthétique — cas canonique)
- [ ] Bruit blanc stationnaire 2 s (détecte modulations parasites)
- [ ] Bruit blanc "gaté" (bursts de 100 ms toutes les 500 ms, test détection transients)
- [ ] **Générer aussi le .ana correspondant manuellement** pour contrôler exactement où sont les transients

### 0.4 — Comparaison A/B avec Ableton
- [ ] Pour chaque signal de test, générer la version Ableton beat mode (export audio du résultat warpé)
- [ ] Avoir un outil de switch instantané entre les deux versions (un simple Pd patch avec sélecteur)
- [ ] L'oreille isolée compare mal, l'oreille en A/B immédiat identifie en 5 secondes

---

## Phase 1 — Corrections de bugs algorithmiques

### 1.1 — Reset du play_pos en mode attaque
- [ ] Problème : `if ((int)pos_global == t_start_output)` peut être raté selon les arrondis de tempo_ratio
- [ ] Solution : utiliser un flag `attack_started` qui se déclenche au premier passage dans la zone, plus robuste
- [ ] Tester avec ratio = 0.73, 1.37, etc. (ratios qui tombent mal)

### 1.2 — Transition attaque → sustain
- [ ] Problème : passage sec d'une lecture 1:1 à une lecture bouclée fenêtrée → clic systématique
- [ ] Solution : crossfade de 5-10 ms entre les deux modes (garder en mémoire les derniers samples de l'attaque, les mixer avec les premiers samples du loop)

### 1.3 — Crossfade entre fin de segment et début du segment suivant
- [ ] Problème : quand un segment se termine (`pos_global == t_end_output`), on saute directement au début du segment suivant → discontinuité
- [ ] Solution : crossfade equal-power de 5-10 ms entre la queue du segment courant et l'attaque du segment suivant
- [ ] Attention : cela ne doit PAS retarder le transient suivant (le crossfade doit commencer AVANT l'attaque)

### 1.4 — Safety sur pos_global avec samples flottants
- [ ] Actuellement `(int)pos_global` compare avec des positions calculées en float via tempo_ratio → drift possible sur longues durées
- [ ] Passer toute la logique de position en double, ou accumuler un offset fractionnaire

---

## Phase 2 — Améliorations du sustain (cœur du problème)

### 2.1 — Point de début de loop sur zero-crossing
- [ ] Problème actuel : `release_start_source = t_start + (t_end - t_start) / 2` tombe n'importe où, y compris en plein milieu d'un cycle
- [ ] Solution : chercher le zero-crossing le plus proche du milieu, dans une fenêtre de ± 50 ms
- [ ] Préférer les zero-crossings avec la même pente (ascendante → ascendante) pour éviter les inversions de phase
- [ ] À faire idéalement dans l'analyseur Python (pré-calcul, pas de coût CPU temps réel) → stocker dans le .ana

### 2.2 — Vrai crossfade entre les cycles du loop
- [ ] Problème actuel : ton Hann window s'applique dans CHAQUE cycle (fade in au début, fade out à la fin) → chaque cycle a un "trou" au milieu et surtout la jointure entre cycle N et cycle N+1 n'est pas crossfadée, seulement concaténée après atténuation
- [ ] Solution : crossfade additif equal-power entre la fin du cycle N (qui continue au-delà du point de loop) et le début du cycle N+1
- [ ] Longueur recommandée : 10-20 ms, ou 5-10% de la longueur du loop (selon ce qui est le plus petit)

### 2.3 — Mode Back-and-Forth (ping-pong loop)
- [ ] Implémenter comme alternative au forward loop
- [ ] Alterner direction de lecture à chaque cycle : forward → backward → forward → ...
- [ ] Bénéfice : sur des sons avec décroissance naturelle (piano, cymbales, pads), élimine 80% de l'effet "répétition audible"
- [ ] Pas besoin de crossfade aussi agressif car les jointures se font naturellement à des points symétriques
- [ ] Ajouter un argument `loop_mode` (0 = off, 1 = forward, 2 = back-and-forth)

### 2.4 — Transient Envelope (fade-out de fin de segment)
- [ ] C'est le paramètre "Envelope" d'Ableton
- [ ] Appliquer une enveloppe de fade-out exponentielle sur chaque segment, avant le prochain transient
- [ ] Paramètre utilisateur de 0 à 100 : 100 = pas de fade, 0 = gate très rapide après le transient
- [ ] Masque très efficacement les artefacts de jointure et les loops trop audibles

### 2.5 — Mode Loop Off
- [ ] Implémenter le cas "pas de loop du tout" : on joue le segment jusqu'à sa fin naturelle, puis silence jusqu'au transient suivant
- [ ] Utile pour des sons très secs (claps, rimshots) où le silence est préférable à un loop
- [ ] Cas le plus simple à implémenter, utile comme mode "baseline" pour déboguer

---

## Phase 3 — Améliorations de l'analyseur Python

### 3.1 — Stocker plus d'infos dans le .ana
Le .ana ne contient que les positions de transients. À enrichir :
- [ ] Pour chaque transient : position du zero-crossing le plus proche (pour le début de l'attaque)
- [ ] Pour chaque segment : position du point de début de loop (zero-crossing proche du milieu, avec bonne pente)
- [ ] Pour chaque segment : estimation de la nature (percussif / tonal / mixte) pour adapter les paramètres
- [ ] Optionnel : estimation de la durée d'attaque réelle (via enveloppe d'énergie)

### 3.2 — Détection d'attaque plus fine
- [ ] librosa.onset.onset_detect est bien mais général
- [ ] Tester aussi : HFC (High Frequency Content), complex domain, spectral flux
- [ ] Pour du matériel percussif, ajouter un post-traitement : vérifier qu'il y a un vrai pic d'énergie locale autour du transient détecté
- [ ] Paramètres exposés : seuil, pre-max, post-max (pour éviter double-détection)

### 3.3 — Détection du début réel de l'attaque (pre-transient)
- [ ] Le transient détecté correspond au pic, mais l'attaque commence quelques ms avant
- [ ] Reculer de 5-10 ms pour englober la montée complète
- [ ] Important pour ne pas couper le "snap" initial d'un transient

---

## Phase 4 — Méthodologie de mesure en continu

### 4.1 — Script de comparaison spectrogramme
- [ ] Python script qui prend 2 fichiers wav et affiche les spectrogrammes côte à côte
- [ ] Ajouter un 3e panneau : spectrogramme de différence (mode perceptuel log)
- [ ] À lancer systématiquement après chaque modif de l'algo

### 4.2 — Métrique objective de qualité
- [ ] Implémenter ou utiliser ViSQOL (https://github.com/google/visqol) pour avoir un score MOS
- [ ] Tracker ce score sur la batterie de tests, regression-testing automatique
- [ ] Alternative plus simple : SNR par bande (PEAQ-like) avec scipy

### 4.3 — Visualisation waveform aux points de jointure
- [ ] Script qui détecte automatiquement les points de jointure (début de loop, fin de segment) et zoome à l'échelle sample
- [ ] Affiche ± 5 ms autour de chaque jointure
- [ ] Une discontinuité visible = un clic audible

### 4.4 — Test de préservation des transients
- [ ] Mesurer la position des transients dans l'input et dans l'output
- [ ] Elle doit correspondre à la position attendue après tempo_ratio
- [ ] Si décalage > 5 ms, problème de timing

---

## Phase 5 — Raffinements (une fois le reste solide)

### 5.1 — Interpolation pour le tempo_ratio non entier
- [ ] Actuellement, `pos_audio = (unsigned long)(pos_global * tempo_ratio)` fait un simple cast → aliasing potentiel
- [ ] Utiliser interpolation linéaire (ou mieux : sinc tronqué) sur la position de lecture
- [ ] Impact audible surtout sur matériel aigu

### 5.2 — Paramètres utilisateurs exposés
Depuis Pd, messages à ajouter :
- [ ] `loop_mode <0/1/2>` : off / forward / back-and-forth
- [ ] `envelope <0-100>` : transient envelope decay
- [ ] `loop_crossfade_ms <n>` : longueur du crossfade de loop
- [ ] `min_attack_ms <n>` / `max_attack_ms <n>` : bornes de la durée d'attaque
- [ ] `attack_ratio <0-1>` : proportion du segment dédiée à l'attaque (actuellement hardcodé à 0.5)

### 5.3 — Gestion des segments très courts
- [ ] Si un segment fait moins de ~30 ms (deux transients très rapprochés), l'algo actuel va dégénérer
- [ ] Cas spécial : pas de loop, lecture directe, éventuellement fusionner avec le segment suivant

### 5.4 — Support stéréo
- [ ] Actuellement mono uniquement. Pour stéréo, garder les mêmes positions de loop sur L et R (sinon ça élargit/rétrécit l'image)
- [ ] Les zero-crossings doivent être calculés sur mid (L+R)/2 pour cohérence

---

## Références utiles

- **Driedger & Müller, "TSM Toolbox"** (DAFx 2014) : code MATLAB de référence, implémente WSOLA, phase vocoder, etc.
- **Ableton Manual — Audio Clips, Tempo, and Warping** : décrit les Loop Modes et le Transient Envelope
- **Sound on Sound — "Ableton Live: Warping Revisited"** : explication claire du beat mode et du loop fill
- **Shuriken** (github.com/rock-hopper/shuriken) : beat slicer open source, aubio + rubber band
- **Signalsmith Stretch** : pas la même philosophie mais excellent doc sur les artefacts
- **ITU-R BS.1534 (MUSHRA)** : protocole standard pour tests subjectifs de qualité audio
- **PEAQ (ITU-R BS.1387)** : standard pour mesure objective

---

## Ordre d'attaque recommandé

1. **Phase 0 entière** avant de toucher au code (1 journée)
2. **1.1 + 1.4** (robustesse du code existant)
3. **2.1 + 2.2** (zero-crossing + vrai crossfade) → gros gain audible
4. **2.4** (transient envelope) → masque ce qui reste
5. **2.3** (back-and-forth) → complète la qualité sur sons tonaux
6. **1.2 + 1.3** (crossfades de transition) → polish
7. **3.x** (enrichir le .ana) → nécessite refactor de l'analyseur
8. **Le reste selon les besoins**

À chaque étape : lancer la batterie de tests de la Phase 0, comparer spectrogrammes et A/B avec Ableton. Si ça ne s'améliore pas mesurablement, ne pas garder le changement.