# INSTALLATION
#pip install librosa soundfile

# USAGE : 
# python3 analyze_transients.py piano.wav
#
import librosa
import json
import sys
from pathlib import Path

def analyze_file(audio_path, output_prefix=None):
    # Charger le fichier en mono, forcer le sample rate à 48 kHz
    y, sr = librosa.load(audio_path, sr=48000, mono=True)

    # Détection des transitoires (onsets) en échantillons
    onsets = librosa.onset.onset_detect(y=y, sr=sr, units="samples")

    # Préparer sortie
    if output_prefix is None:
        output_prefix = Path(audio_path).with_suffix("")

    ana_path = str(output_prefix) + ".ana"
    json_path = str(output_prefix) + ".json"

    # Export .ana (texte brut)
    with open(ana_path, "w") as f:
        f.write(f"# sample_rate {sr}\n")
        for onset in onsets:
            f.write(f"{onset}\n")

    # Export JSON
    data = {"sample_rate": sr, "onsets": onsets.tolist()}
    with open(json_path, "w") as f:
        json.dump(data, f, indent=2)

    print(f"Analyse terminée ✅\n - {ana_path}\n - {json_path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python analyze_transients.py input.wav [output_prefix]")
    else:
        audio_path = sys.argv[1]
        output_prefix = sys.argv[2] if len(sys.argv) > 2 else None
        analyze_file(audio_path, output_prefix)
