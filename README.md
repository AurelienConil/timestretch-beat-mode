# timestretch-beat-mode

External Pure Data pour timestretch léger basé sur la détection d'attaques

## Description

Ce projet consiste à créer un external Pure Data pour faire du timestretch très léger en CPU. Le principe repose sur :

1. **Analyseur Python** : Analyse préalable d'un fichier audio pour détecter les attaques et créer un fichier `.ana`
2. **External Pure Data** : Lecture du fichier audio `.wav` en utilisant les informations d'attaques du fichier `.ana` pour un timestretch optimisé

## Installation pour développeurs

### 1. Clonage du projet

```bash
git clone https://github.com/AurelienConil/timestretch-beat-mode.git
cd timestretch-beat-mode
```

### 2. Initialisation du sous-module

```bash
git submodule update --init --recursive
```

### 3. Compilation de l'external

```bash
make
```

Cela génère le fichier `timestretch_beatmode~.pd_darwin` (sur macOS) ou `.pd_linux` (sur Linux).

### 4. Configuration de l'environnement Python

Créer un environnement virtuel et installer les dépendances :

```bash
cd python-analyzer
python3 -m venv analyzer_env
source analyzer_env/bin/activate  # Sur macOS/Linux
# ou analyzer_env\Scripts\activate sur Windows
pip install librosa soundfile
```

### 5. Utilisation de l'analyseur Python

```bash
cd python-analyzer
python analyze.py votre_fichier.wav
```

Cela génère un fichier `votre_fichier.ana` contenant les informations d'attaques.

### 6. Test dans Pure Data

1. Ouvrez Pure Data
2. Ajoutez le dossier du projet au chemin de recherche de Pure Data
3. Ouvrez le fichier `timestretch_beatmode~-help.pd` pour voir la documentation et les exemples d'utilisation


## Structure du projet

```
timestretch-beat-mode/
├── timestretch_beatmode~.c      # Code source de l'external
├── timestretch_beatmode~-help.pd # Documentation Pure Data
├── python-analyzer/             # Analyseur Python
│   ├── analyze.py              # Script d'analyse principal
│   └── analyzer_env/           # Environnement virtuel Python
├── pd-lib-builder/             # Sous-module pour la compilation
└── Makefile                    # Configuration de compilation
```