/* timestretch_beatmode~.c - Pure Data external template for timestretch beat mode */

#include "m_pd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static t_class *timestretch_beatmode_tilde_class;

typedef struct _timestretch_beatmode_tilde {
    t_object x_obj;
    t_float f;

    // Outlets
    t_outlet *x_audio_out;
    t_outlet *x_bang_out;
    t_outlet *x_print_out;

    // Audio file and analysis
    char wav_filename[256];
    char ana_filename[256];
    int sample_rate;
    int tempo_original;
    int tempo_target;

    // Audio buffer
    float *audio_buffer;
    int audio_length;

    // Transients
    int *transients;
    int num_transients;

    // Point de début de loop (zero-crossing proche du milieu) pré-calculé par segment
    int *release_starts;

    // Playback state
    int playing;
    int current_transient;
    int sustain_ms;
    int play_pos;
    int sustain_pos;
    int sustain_samples;
    int in_sustain;
    int attack_samples;
    double tempo_ratio;
    double inv_tempo_ratio;
    unsigned long sample_counter;
    double global_pos; // position globale en samples (double pour éviter le drift)
    int attack_started; // flag : attaque amorcée pour le transient courant
    int attack_preroll; // nb de samples déjà pré-lus pour la prochaine attaque (via crossfade sustain→next)
    int envelope; // 0..100 : 100 = pas de fade-out, 0 = gate rapide après le transient
    int loop_mode; // 0 = off (silence), 1 = forward, 2 = ping-pong (back-and-forth)
    // ... autres paramètres ...
} t_timestretch_beatmode_tilde;

// Forward declarations (helpers définis plus bas)
static int find_zero_crossing_near(const float *buf, int buf_len, int target, int window);
static float compute_sustain_sample(const t_timestretch_beatmode_tilde *x,
                                     int release_start, int loop_len,
                                     int sustain_pos, int xfade_len,
                                     int loop_mode);
// Helper: read mono wav file (PCM 16 bits)
#include <stdint.h>
static int read_wav_file(const char *filename, float **buffer, int *length) {
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;
    char chunk_id[5] = {0};
    int chunk_size = 0;
    int format = 0;
    int audio_format = 0, num_channels = 0, sample_rate = 0, byte_rate = 0, block_align = 0, bits_per_sample = 0;
    int data_size = 0;
    // RIFF header
    fread(chunk_id, 1, 4, f); // "RIFF"
    fread(&chunk_size, 4, 1, f);
    fread(chunk_id, 1, 4, f); // "WAVE"
    // fmt chunk
    fread(chunk_id, 1, 4, f); // "fmt "
    fread(&chunk_size, 4, 1, f);
    fread(&audio_format, 2, 1, f);
    fread(&num_channels, 2, 1, f);
    fread(&sample_rate, 4, 1, f);
    fread(&byte_rate, 4, 1, f);
    fread(&block_align, 2, 1, f);
    fread(&bits_per_sample, 2, 1, f);
    // Skip extra fmt bytes if present
    if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
    // Find data chunk
    while (1) {
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return -2; }
        fread(&chunk_size, 4, 1, f);
        if (strncmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            break;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }
    // Debug header info
    post("WAV: format=%d, channels=%d, sr=%d, bits=%d, data_size=%d", audio_format, num_channels, sample_rate, bits_per_sample, data_size);
    if (audio_format != 1 || num_channels != 1 || bits_per_sample != 16) {
        post("WAV format not supported (must be mono PCM 16 bits)");
        fclose(f);
        return -4;
    }
    int num_samples = data_size / 2;
    int16_t *tmp = (int16_t *)malloc(data_size);
    if (fread(tmp, 1, data_size, f) != data_size) { free(tmp); fclose(f); return -3; }
    *buffer = (float *)malloc(sizeof(float) * num_samples);
    for (int i = 0; i < num_samples; i++) {
        (*buffer)[i] = tmp[i] / 32768.0f;
    }
    *length = num_samples;
    free(tmp);
    fclose(f);
    return 0;
}


// Helper: read .ana file
static void read_ana_file(t_timestretch_beatmode_tilde *x, const char *ana_filename) {
    FILE *f = fopen(ana_filename, "r");
    if (!f) {
        outlet_anything(x->x_print_out, gensym("error"), 0, NULL);
        return;
    }
    char line[256];
    int transient_capacity = 128;
    x->transients = (int *)malloc(sizeof(int) * transient_capacity);
    x->num_transients = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "# sample_rate", 13) == 0) {
            sscanf(line, "# sample_rate %d", &x->sample_rate);
        } else if (strncmp(line, "# tempo", 7) == 0) {
            sscanf(line, "# tempo %d", &x->tempo_original);
        } else if (line[0] == '#' || strlen(line) < 2) {
            continue;
        } else {
            int pos = atoi(line);
            if (x->num_transients >= transient_capacity) {
                transient_capacity *= 2;
                x->transients = (int *)realloc(x->transients, sizeof(int) * transient_capacity);
            }
            x->transients[x->num_transients++] = pos;
        }
    }
    fclose(f);
}

void timestretch_beatmode_tilde_play(t_timestretch_beatmode_tilde *x, t_symbol *s, int argc, t_atom *argv) {
    if (argc < 2) {
        outlet_anything(x->x_print_out, gensym("error"), 0, NULL);
        return;
    }
    // Get filename and tempo
    if (argv[0].a_type == A_SYMBOL && argv[1].a_type == A_FLOAT) {
        t_symbol *sym = atom_getsymbol(&argv[0]);
        strncpy(x->wav_filename, sym->s_name, 255);
        x->wav_filename[255] = '\0';
        x->tempo_target = (int)atom_getfloat(&argv[1]);
        // Compose ana filename (remplace .wav par .ana)
        strncpy(x->ana_filename, x->wav_filename, 255);
        x->ana_filename[255] = '\0';
        char *dot = strrchr(x->ana_filename, '.');
        if (dot) strcpy(dot, ".ana");
        // Read .ana file
        read_ana_file(x, x->ana_filename);
        // Read wav file
        if (x->audio_buffer) { free(x->audio_buffer); x->audio_buffer = NULL; }
        int ret = read_wav_file(x->wav_filename, &x->audio_buffer, &x->audio_length);
        if (ret != 0) {
            outlet_anything(x->x_print_out, gensym("error_wav"), 0, NULL);
            x->playing = 0;
            return;
        }
        // Pré-calcul des points de début de loop : zero-crossing proche du milieu
        // du segment (fenêtre ±50 ms). Évite que le loop démarre en plein cycle,
        // ce qui complète 2.2 (le crossfade masque moins de delta à la jointure).
        if (x->release_starts) { free(x->release_starts); x->release_starts = NULL; }
        if (x->num_transients > 0 && x->audio_buffer) {
            x->release_starts = (int *)malloc(sizeof(int) * x->num_transients);
            int window = (int)(x->sample_rate * 0.05f); // ±50 ms
            for (int i = 0; i < x->num_transients; i++) {
                int t_start = x->transients[i];
                int t_end = (i + 1 < x->num_transients) ? x->transients[i+1] : x->audio_length;
                int midpoint = t_start + (t_end - t_start) / 2;
                int rs = find_zero_crossing_near(x->audio_buffer, x->audio_length, midpoint, window);
                // Sécurité : doit rester strictement à l'intérieur du segment.
                if (rs < t_start + 1) rs = t_start + 1;
                if (rs > t_end - 1)   rs = t_end - 1;
                x->release_starts[i] = rs;
            }
        }
        x->playing = 1;
        x->current_transient = 0;
        x->play_pos = 0;
        x->sustain_samples = (int)(x->sample_rate * (x->sustain_ms / 1000.0f));
        x->attack_samples = (int)(x->sample_rate * 0.03f); // 30ms attaque par défaut
        x->in_sustain = 0;
        x->sustain_pos = 0;
    x->tempo_ratio = (double)x->tempo_target / (double)x->tempo_original;
    x->inv_tempo_ratio = 1.0 / x->tempo_ratio;
    x->sample_counter = 0;
    x->global_pos = 0.0;
    x->attack_started = 0;
    x->attack_preroll = 0;
    t_atom infos[3];
    SETFLOAT(&infos[0], (float)x->audio_length);
    SETFLOAT(&infos[1], (float)x->num_transients);
    SETFLOAT(&infos[2], (float)x->sample_rate);
    outlet_anything(x->x_print_out, gensym("info"), 3, infos);
    }
}

void *timestretch_beatmode_tilde_new(void) {
    t_timestretch_beatmode_tilde *x = (t_timestretch_beatmode_tilde *)pd_new(timestretch_beatmode_tilde_class);
    // 1 inlet (messages)
    // Outlets : audio doit être le premier
    x->x_audio_out = outlet_new(&x->x_obj, &s_signal);
    x->x_bang_out = outlet_new(&x->x_obj, &s_bang);
    x->x_print_out = outlet_new(&x->x_obj, &s_anything);
    x->playing = 0;
    x->audio_buffer = NULL;
    x->transients = NULL;
    x->num_transients = 0;
    x->release_starts = NULL;
    x->current_transient = 0;
    x->sustain_ms = 200; // valeur par défaut
    x->envelope = 100;   // pas de fade-out par défaut
    x->loop_mode = 1;    // forward loop par défaut
    return (void *)x;
}

// Message `loop_mode <0|1|2>` :
//   0 = off (silence après la phase d'attaque, pas de loop)
//   1 = forward (loop avant, comportement par défaut)
//   2 = back-and-forth (ping-pong : alterne forward/backward à chaque cycle)
void timestretch_beatmode_tilde_loop_mode(t_timestretch_beatmode_tilde *x, t_floatarg f) {
    int v = (int)f;
    if (v < 0) v = 0;
    if (v > 2) v = 2;
    x->loop_mode = v;
}

// Message `envelope <0..100>` : règle l'enveloppe de fade-out appliquée à chaque segment.
// 100 = aucune atténuation (loop complet). 0 ≈ gate quasi immédiat après le transient.
// L'amplitude suit (envelope/100)^(t/durée_segment) : exponentielle, donc au milieu du
// segment on est à sqrt(envelope/100) de l'original.
void timestretch_beatmode_tilde_envelope(t_timestretch_beatmode_tilde *x, t_floatarg f) {
    int v = (int)f;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    x->envelope = v;
}

// Cherche le zero-crossing le plus proche de `target` dans une fenêtre ± `window`.
// Si aucun trouvé, retourne `target`.
static int find_zero_crossing_near(const float *buf, int buf_len, int target, int window) {
    if (buf_len < 2) return target;
    int start = target - window;
    int end   = target + window;
    if (start < 1) start = 1;
    if (end >= buf_len) end = buf_len - 1;
    int best = target;
    int best_dist = window + 1;
    for (int i = start; i < end; i++) {
        float a = buf[i-1], b = buf[i];
        if ((a <= 0.f && b > 0.f) || (a >= 0.f && b < 0.f)) {
            int dist = (i > target) ? (i - target) : (target - i);
            if (dist < best_dist) {
                best_dist = dist;
                best = i;
            }
        }
    }
    return best;
}

// Lit un sample du loop de sustain en overlap-add (modèle deux grains) :
// - grain A actif : lit [release_start, release_start + cycle_period)
// - grain B (précédent) : lit la queue [release_start + cycle_period, release_start + loop_len)
//                         pendant les `xfade_len` premiers samples du grain A suivant.
// Chaque grain a une enveloppe equal-power (sin/cos) sur ses `xfade_len` bords.
// Au lieu d'une fenêtre de Hann qui creuse chaque cycle, la somme des deux grains
// est à puissance constante sur la jonction de loop.
//
// loop_mode :
//   1 = forward (tous les grains lisent de release_start vers release_start+loop_len-1)
//   2 = ping-pong (grain d'indice pair forward, impair backward).
//       En backward, le grain lit les mêmes samples que forward mais en miroir :
//       src = release_start + (loop_len - 1 - local_pos). La jonction se fait naturellement
//       sur la même zone audio (la fin du forward et le début du backward lisent tous deux
//       autour de release_start+loop_len-1 → pas de discontinuité brutale).
static float compute_sustain_sample(const t_timestretch_beatmode_tilde *x,
                                     int release_start, int loop_len,
                                     int sustain_pos, int xfade_len,
                                     int loop_mode) {
    if (loop_len < 1 || !x->audio_buffer) return 0.f;

    int cycle_period = loop_len - xfade_len;
    int ping_pong = (loop_mode == 2);

    if (cycle_period < 1 || xfade_len < 1) {
        // Dégénéré : loop trop court pour un crossfade, fallback loop simple.
        int sp_mod = sustain_pos % loop_len;
        int pp_idx = ping_pong ? (sustain_pos / loop_len) & 1 : 0;
        int local  = pp_idx ? (loop_len - 1 - sp_mod) : sp_mod;
        int src = release_start + local;
        return (src >= 0 && src < x->audio_length) ? x->audio_buffer[src] : 0.f;
    }

    int grain_idx = sustain_pos / cycle_period;
    int grain_pos = sustain_pos % cycle_period;
    int a_reverse = ping_pong && (grain_idx & 1);
    int b_reverse = ping_pong && !(grain_idx & 1); // grain B = précédent, parité opposée

    // Grain A (courant) : fade-in equal-power sur ses xfade_len premiers samples
    int a_local = grain_pos;
    int src_a = a_reverse
                ? release_start + (loop_len - 1 - a_local)
                : release_start + a_local;
    float sample_a = (src_a >= 0 && src_a < x->audio_length) ? x->audio_buffer[src_a] : 0.f;
    float env_a;
    if (grain_pos < xfade_len) {
        float theta = (float)(M_PI * 0.5) * ((float)grain_pos / (float)xfade_len);
        env_a = sinf(theta);
    } else {
        env_a = 1.0f;
    }
    float out = sample_a * env_a;

    // Grain B (précédent) : dans sa queue fade-out pendant le début du grain A
    if (grain_idx > 0 && grain_pos < xfade_len) {
        int b_local = cycle_period + grain_pos;
        int src_b = b_reverse
                    ? release_start + (loop_len - 1 - b_local)
                    : release_start + b_local;
        float sample_b = (src_b >= 0 && src_b < x->audio_length) ? x->audio_buffer[src_b] : 0.f;
        float theta = (float)(M_PI * 0.5) * ((float)grain_pos / (float)xfade_len);
        float env_b = cosf(theta);
        out += sample_b * env_b;
    }

    return out;
}

// DSP perform routine (à compléter avec la logique de timestretch)
static t_int *timestretch_beatmode_tilde_perform(t_int *w) {
    t_timestretch_beatmode_tilde *x = (t_timestretch_beatmode_tilde *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int n = (int)(w[3]);
    // Calcul de la durée totale de lecture en sortie (corrigé)
    unsigned long duration_out = (unsigned long)((double)x->audio_length * x->tempo_ratio);
    for (int i = 0; i < n; i++) {
        // Arrêter la lecture si pas de lecture active ou pas de buffer
        if (!x->playing || !x->audio_buffer || x->audio_length <= 0) {
            out[i] = 0;
            continue;
        }
        x->sample_counter++;
        // Position globale en samples (horloge, en double pour éviter le drift)
        double pos_global = x->global_pos;
        x->global_pos += 1.0;
        // Calcul de la position dans le buffer audio selon le tempo
        unsigned long pos_audio = (unsigned long)(pos_global * x->tempo_ratio);
        
        // Vérifier si on a dépassé la fin du fichier audio original
        if (pos_audio >= (unsigned long)x->audio_length) {
            out[i] = 0;
            if (x->playing) {
                float seconds = (float)(x->global_pos / (double)x->sample_rate);
                t_atom a;
                SETFLOAT(&a, seconds);
                outlet_anything(x->x_print_out, gensym("duration"), 1, &a);
                x->playing = 0;
            }
            continue;
        }
        
        // Vérifier si on a dépassé la durée totale prévue (tous les transients traités)
        if (x->current_transient >= x->num_transients && pos_audio >= (unsigned long)x->audio_length) {
            out[i] = 0;
            if (x->playing) {
                float seconds = (float)(x->global_pos / (double)x->sample_rate);
                t_atom a;
                SETFLOAT(&a, seconds);
                outlet_anything(x->x_print_out, gensym("duration"), 1, &a);
                x->playing = 0;
            }
            continue;
        }
        // Transient courant avec gestion séparée attaque/sustain
        int t_start = x->transients[x->current_transient];
        int t_end = (x->current_transient + 1 < x->num_transients) ? x->transients[x->current_transient + 1] : x->audio_length;
        
        // Bornes de segment dans le temps de sortie (en double pour éviter le drift)
        double t_start_output_d    = (double)t_start * x->inv_tempo_ratio;
        double t_end_output_d      = (double)t_end   * x->inv_tempo_ratio;
        double interval_duration_d = t_end_output_d - t_start_output_d;

        // Calcul des durées d'attaque adaptatives (en samples discrets)
        int attack_duration_output = (int)(interval_duration_d * 0.5);
        // Limites de sécurité pour l'attaque
        int min_attack = (int)(x->sample_rate * 0.01f); // 10ms
        int max_attack = (int)(x->sample_rate * 0.2f);  // 200ms
        if (attack_duration_output < min_attack) attack_duration_output = min_attack;
        if (attack_duration_output > max_attack) attack_duration_output = max_attack;

        double attack_end_output_d = t_start_output_d + (double)attack_duration_output;

        // Crossfade equal-power : 5 ms par défaut, clampé pour ne pas excéder
        // 1/3 de l'attaque ni 1/3 du sustain du segment courant.
        int xfade_len = (int)(x->sample_rate * 0.005f);
        int sustain_duration_output = (int)(t_end_output_d - attack_end_output_d);
        int max_xfade = attack_duration_output < sustain_duration_output
                        ? attack_duration_output : sustain_duration_output;
        max_xfade /= 3;
        int local_xfade = xfade_len < max_xfade ? xfade_len : max_xfade;
        if (local_xfade < 1) local_xfade = 1;

        // Enveloppe de segment (Transient Envelope, paramètre Ableton) :
        // amp(t) = (envelope/100)^(t/segment_duration) ; t normalisé à [0, 1].
        // 100 → 1 partout ; 0 → chute très rapide. Appliquée au segment courant
        // uniquement (pas au pre-read du segment suivant, pour préserver son transient).
        float env_current;
        if (x->envelope >= 100) {
            env_current = 1.0f;
        } else {
            float env_factor = (float)x->envelope * 0.01f;
            double seg_pos = pos_global - t_start_output_d;
            if (seg_pos < 0.0) seg_pos = 0.0;
            double seg_dur = interval_duration_d > 0.0 ? interval_duration_d : 1.0;
            float norm_t = (float)(seg_pos / seg_dur);
            if (norm_t > 1.f) norm_t = 1.f;
            if (env_factor <= 0.f) {
                env_current = (seg_pos < 1.0) ? 1.f : 0.f;
            } else {
                env_current = powf(env_factor, norm_t);
            }
        }

        // Mode attaque : lecture 1:1 du fichier original (pas de changement de pitch)
        if (!x->in_sustain
            && pos_global >= t_start_output_d
            && pos_global <  attack_end_output_d) {

            // Premier sample dans la zone d'attaque, quel que soit l'arrondi
            if (!x->attack_started) {
                t_atom a[2];
                SETSYMBOL(&a[0], gensym("attaque"));
                SETFLOAT(&a[1], (float)attack_duration_output / x->sample_rate * 1000.0f); // durée en ms
                outlet_anything(x->x_bang_out, gensym("mode"), 2, a);
                // Si le sustain précédent a déjà pré-lu des samples d'attaque via
                // le crossfade sustain→next, on reprend là où il s'est arrêté.
                x->play_pos = x->attack_preroll;
                x->attack_preroll = 0;
                x->attack_started = 1;
            }

            // Position dans le fichier original : lecture directe sans tempo scaling
            int source_pos_attack = t_start + x->play_pos;
            x->play_pos++;

            float attack_sample = (source_pos_attack < x->audio_length)
                                  ? x->audio_buffer[source_pos_attack] : 0.f;

            // Crossfade attaque→sustain sur les derniers local_xfade samples :
            // on lit en parallèle le début du loop de sustain (via le modèle
            // overlap-add de compute_sustain_sample) et on fond les deux en
            // equal-power pour supprimer le clic de transition.
            double attack_xfade_start = attack_end_output_d - (double)local_xfade;
            if (pos_global >= attack_xfade_start) {
                int xfade_t = (int)(pos_global - attack_xfade_start);
                if (xfade_t < 0) xfade_t = 0;
                if (xfade_t >= local_xfade) xfade_t = local_xfade - 1;

                int release_start_source = x->release_starts
                    ? x->release_starts[x->current_transient]
                    : (t_start + (t_end - t_start) / 2);
                int loop_len = t_end - release_start_source;
                if (loop_len < 1) loop_len = 1;

                // xfade interne au loop : clampé à 1/4 de loop_len
                int loop_xfade = local_xfade < (loop_len / 4) ? local_xfade : (loop_len / 4);
                if (loop_xfade < 1) loop_xfade = 1;

                // loop_mode 0 : pas de sustain → fade-out pur de l'attaque vers 0
                float sustain_sample = (x->loop_mode == 0) ? 0.f
                    : compute_sustain_sample(x, release_start_source, loop_len,
                                              xfade_t, loop_xfade, x->loop_mode);

                float theta = (float)(M_PI * 0.5) * ((float)xfade_t / (float)local_xfade);
                float fade_out = cosf(theta);
                float fade_in  = sinf(theta);
                out[i] = (attack_sample * fade_out + sustain_sample * fade_in) * env_current;
            } else {
                out[i] = attack_sample * env_current;
            }

            if ((pos_global + 1.0) >= attack_end_output_d) {
                x->in_sustain = 1;
                // On a déjà consommé local_xfade samples du loop pendant le crossfade.
                x->sustain_pos = local_xfade;
                x->attack_started = 0; // reset pour le prochain transient
                t_atom a;
                SETSYMBOL(&a, gensym("sustain"));
                outlet_anything(x->x_bang_out, gensym("mode"), 1, &a);
            }
        }
        // Mode sustain : utilise le tempo scaling et le loop
        else if (x->in_sustain
                 && pos_global >= attack_end_output_d
                 && pos_global <  t_end_output_d) {
            // Début de loop : zero-crossing pré-calculé, ou fallback 50% entre marqueurs
            int release_start_source = x->release_starts
                ? x->release_starts[x->current_transient]
                : (t_start + (t_end - t_start) / 2);
            int loop_len = t_end - release_start_source;
            if (loop_len < 1) loop_len = 1;

            // xfade interne au loop : clampé à 1/4 de loop_len pour rester sain
            int loop_xfade = local_xfade < (loop_len / 4) ? local_xfade : (loop_len / 4);
            if (loop_xfade < 1) loop_xfade = 1;

            // Sample du loop via overlap-add deux grains (vrai crossfade entre cycles).
            // loop_mode 0 : silence pendant le sustain (le xfade sustain→next fera fade-in
            // du prochain transient depuis 0 → transition propre).
            float sustain_sample = (x->loop_mode == 0) ? 0.f
                : compute_sustain_sample(x, release_start_source, loop_len,
                                          x->sustain_pos, loop_xfade, x->loop_mode);

            // Crossfade sustain→next attack sur les derniers local_xfade samples :
            // on pré-lit l'attaque du segment suivant et on fond les deux en equal-power.
            // Le crossfade se termine à t_end_output_d ; le transient suivant n'est donc
            // pas retardé (il démarre légèrement en avance côté source).
            double seg_xfade_start = t_end_output_d - (double)local_xfade;
            int has_next = (x->current_transient + 1 < x->num_transients);
            if (has_next && pos_global >= seg_xfade_start) {
                int xfade_t = (int)(pos_global - seg_xfade_start);
                if (xfade_t < 0) xfade_t = 0;
                if (xfade_t >= local_xfade) xfade_t = local_xfade - 1;

                int t_start_next = x->transients[x->current_transient + 1];
                int source_pos_next = t_start_next + xfade_t;
                float next_attack_sample = (source_pos_next < x->audio_length)
                                           ? x->audio_buffer[source_pos_next] : 0.f;

                float theta = (float)(M_PI * 0.5) * ((float)xfade_t / (float)local_xfade);
                float fade_out = cosf(theta);
                float fade_in  = sinf(theta);
                // env_current sur sustain, PAS sur next_attack (c'est le prochain segment,
                // avec sa propre enveloppe qui n'a pas encore démarré → norm_t=0 → 1.0).
                out[i] = sustain_sample * fade_out * env_current + next_attack_sample * fade_in;
            } else {
                out[i] = sustain_sample * env_current;
            }

            x->sustain_pos++;

            // Fin du sustain
            if ((pos_global + 1.0) >= t_end_output_d) {
                x->current_transient++;
                x->in_sustain = 0;
                x->sustain_pos = 0;
                // On a pré-lu local_xfade samples du prochain segment ; l'attaque
                // qui suit reprendra depuis cette position.
                x->attack_preroll = has_next ? local_xfade : 0;
            }
        }
        // Après le dernier transient, lecture brute jusqu'à la fin
        else if (x->current_transient >= x->num_transients) {
            if ((int)pos_audio < x->audio_length)
                out[i] = x->audio_buffer[(int)pos_audio];
            else
                out[i] = 0;
        }
        // Si rien à lire, silence
        else {
            out[i] = 0;
        }
    }
    return (w + 4);
}

// DSP method
void timestretch_beatmode_tilde_dsp(t_timestretch_beatmode_tilde *x, t_signal **sp) {
    dsp_add(timestretch_beatmode_tilde_perform, 3,
            x, sp[0]->s_vec, sp[0]->s_n);
}

void timestretch_beatmode_tilde_setup(void) {
    timestretch_beatmode_tilde_class = class_new(gensym("timestretch_beatmode~"),
        (t_newmethod)timestretch_beatmode_tilde_new,
        0, sizeof(t_timestretch_beatmode_tilde),
        CLASS_DEFAULT, 0);
    class_addmethod(timestretch_beatmode_tilde_class,
        (t_method)timestretch_beatmode_tilde_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(timestretch_beatmode_tilde_class,
        (t_method)timestretch_beatmode_tilde_play, gensym("play"), A_GIMME, 0);
    class_addmethod(timestretch_beatmode_tilde_class,
        (t_method)timestretch_beatmode_tilde_envelope, gensym("envelope"), A_FLOAT, 0);
    class_addmethod(timestretch_beatmode_tilde_class,
        (t_method)timestretch_beatmode_tilde_loop_mode, gensym("loop_mode"), A_FLOAT, 0);
    CLASS_MAINSIGNALIN(timestretch_beatmode_tilde_class, t_timestretch_beatmode_tilde, f);
}
