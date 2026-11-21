# NDM Event Cleaner (nom de travail) — plugin AU de nettoyage de voix par événements

Plugin Audio Unit pour **Logic Pro X 10.4 / macOS High Sierra (Intel x86_64)**.
Il traite la voix **événement par événement** (une sibilance, une respiration) au lieu d'un seuil continu :
chaque événement est détecté, affiché comme un marqueur, et réglable individuellement.

Conçu à partir des principes publics de ce type d'outil (détection d'événements, Capture, marqueurs, réduction par
événement, Tame Peaks). Tout le code est original : aucune reprise de code, d'interface ni de nom d'un produit existant.

## Statut honnête

| Élément | État |
|---|---|
| Détection (sibilance, respiration) | Écrite en C++, **testée hors plugin** sur une voix de rap réelle (100 % des sibilances C++ confirmées par le prototype Python ; 94 % des sibilances du prototype retrouvées). Seuils = points de départ, à régler à l'oreille. |
| Moteur temps réel (retard + filtre + gains) | **Transparent au bit près** sans événement (latence fixe 5,8 ms à 44,1 kHz) ; testé sous AddressSanitizer/UBSan. Rendu comparé à la version « B » validée à l'écoute : corrélation 0,97 sur le signal retiré. |
| Code JUCE (processeur, éditeur) | **Syntaxe vérifiée contre les en-têtes JUCE 8.0.4** mais **jamais compilé en vrai ni lancé dans Logic**. |
| Écoute en conditions réelles dans Logic | **Non faite.** |

## Utilisation dans Logic

1. Insère le plugin sur la piste de voix (insert).
2. Clique **Capture**, lance la lecture de la voix **du début à la fin** dans Logic, puis **Stop + Analyser**
   (analyse hors temps réel, quelques secondes à quelques dizaines de secondes selon la durée).
3. Les marqueurs apparaissent. Comme dans IcePick, **un bouton par type d'événement** (SIBILANCE, HARD CONS, BREATH) :
   un clic isole ce type (ses marqueurs et ses réglages seulement), **Maj-clic combine** plusieurs types. Rejoue : les événements sont traités.
   Chaque type a sa propre réduction, indépendante des autres. (HARD CONS est grisé : prévu en v1.1.)
4. Réglages du type actif — Sibilance : bouton **Tame Peaks** (actif/inactif), **SENS** (combien : % des sibilances les plus fortes touchées, 30 % par défaut) et **STR** (intensité : 100 % = 12 dB de réduction max, 67 % ≈ 8 dB par défaut) ; Consonnes dures (HARD CONS, v1.1) : mêmes **Tame Peaks / SENS / STR** (STR 100 % = 12 dB, creux large bande de quelques ms sur le burst) ; Respiration : **Breath Reduction** (dB). La ligne d'état indique « N/M marqueurs réduits » ;
   Breath : **Reduction** ; toujours : **Playhead Offset** (recalage manuel en ms). **Solo ce qui est retiré** fait entendre la différence.
5. Clique un marqueur pour modifier cet événement seul (offset en dB, actif/inactif). Molette = zoom, glisser = défiler.

Les événements sont enregistrés dans la session Logic. La capture se refait si tu changes le montage de la voix.

## Build

Comme PopVocal : pousse ce dépôt sur GitHub (branche `main`), l'onglet **Actions** compile et publie l'artifact
`NdmEventCleaner-AU` (le `.component`, Intel x86_64, cible macOS 10.13). Copie-le dans
`~/Library/Audio/Plug-Ins/Components/` puis, dans Logic : *Réanalyser les plug-ins*.
Le token GitHub doit avoir les scopes `repo` **et** `workflow`.

Tests du cœur DSP en local (sans JUCE) :
```bash
g++ -O2 -std=c++17 -o render_test tests/render_test.cpp
./render_test voix_mono_float32.wav sortie.wav 40 80
```

## À faire dans l'ordre (leçons de PopVocal)

1. Le workflow compile-t-il ? (corriger les erreurs réelles, il y en aura peut-être)
2. `auval -v aufx Nevc Dstd` passe ?
3. Charge dans Logic sans crash ? Latence compensée (la piste ne doit pas bouger quand tu bypasses) ?
4. **Écoute avant/après** sur une voix réelle. Ne pas empiler plus de 2-3 versions sans réécoute.
5. Test de silence total et de boucle Logic (saut de position).

## Feuille de route

- v1.1 (ce dépôt) : Capture, sibilances, respirations, consonnes dures (bursts > 2,5 kHz), Tame Peaks SENS/STR par type, édition par événement. Pas encore : EQ par événement, pops graves (< 150 Hz).
- v1.1 : Hard Consonants (plosives : la voix testée en avait peu), EQ par événement.
- v1.2 : Shift de formants, Swap (remplacer une sibilance par une plus propre de la même prise ; la prise testée contient des passages répétés).
- v1.3 : bruits ponctuels dans les pauses (pas, vêtements) en option.

## Limites connues

- Pas d'ARA sous Logic 10.4 : la Capture est obligatoire.
- L'analyse est lente sur un vieux Mac (≈10 s pour 150 s d'audio sur ma machine de test) : optimisable (FFT réelle, float).
- Le seuil SENS (percentile) est calculé sur la prise capturée : recapturer si la prise change.
- La détection a été réglée sur du rap ; chant et parlé à vérifier.
