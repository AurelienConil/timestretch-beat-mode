/* timestretch_beatmode~.c - Pure Data external template for timestretch beat mode */

#include "m_pd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

    // Playback state
    int playing;
    int current_transient;
    int sustain_ms;
    int play_pos;
    int sustain_pos;
    int sustain_samples;
    int in_sustain;
    int attack_samples;
    float tempo_ratio;
    unsigned long sample_counter;
    unsigned long global_pos; // position globale en samples
    // ... autres paramètres ...
} t_timestretch_beatmode_tilde;
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
        x->playing = 1;
        x->current_transient = 0;
        x->play_pos = 0;
        x->sustain_samples = (int)(x->sample_rate * (x->sustain_ms / 1000.0f));
        x->attack_samples = (int)(x->sample_rate * 0.03f); // 30ms attaque par défaut
        x->in_sustain = 0;
        x->sustain_pos = 0;
    x->tempo_ratio = (float)x->tempo_target / (float)x->tempo_original;
    x->sample_counter = 0;
    x->global_pos = 0;
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
    x->current_transient = 0;
    x->sustain_ms = 200; // valeur par défaut
    return (void *)x;
}

// DSP perform routine (à compléter avec la logique de timestretch)
static t_int *timestretch_beatmode_tilde_perform(t_int *w) {
    t_timestretch_beatmode_tilde *x = (t_timestretch_beatmode_tilde *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int n = (int)(w[3]);
    // Calcul de la durée totale de lecture en sortie (corrigé)
    unsigned long duration_out = (unsigned long)((float)x->audio_length * x->tempo_ratio);
    for (int i = 0; i < n; i++) {
        // Arrêter la lecture si pas de lecture active ou pas de buffer
        if (!x->playing || !x->audio_buffer || x->audio_length <= 0) {
            out[i] = 0;
            continue;
        }
        x->sample_counter++;
        // Position globale en samples (horloge)
        unsigned long pos_global = x->global_pos;
        x->global_pos++;
        // Calcul de la position dans le buffer audio selon le tempo
        unsigned long pos_audio = (unsigned long)(pos_global * x->tempo_ratio);
        
        // Vérifier si on a dépassé la fin du fichier audio original
        if (pos_audio >= (unsigned long)x->audio_length) {
            out[i] = 0;
            if (x->playing) {
                float seconds = (float)x->global_pos / (float)x->sample_rate;
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
                float seconds = (float)x->global_pos / (float)x->sample_rate;
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
        
        // Calcul des durées d'attaque adaptatives (dans le temps de sortie)
        int interval_duration_output = (int)((float)(t_end - t_start) / x->tempo_ratio);
        int attack_duration_output = interval_duration_output / 2;
        // Limites de sécurité pour l'attaque
        int min_attack = (int)(x->sample_rate * 0.01f); // 10ms
        int max_attack = (int)(x->sample_rate * 0.2f);  // 200ms
        if (attack_duration_output < min_attack) attack_duration_output = min_attack;
        if (attack_duration_output > max_attack) attack_duration_output = max_attack;
        
        // Positions dans le temps de sortie (échelle tempo)
        int t_start_output = (int)((float)t_start / x->tempo_ratio);
        int attack_end_output = t_start_output + attack_duration_output;
        int t_end_output = (int)((float)t_end / x->tempo_ratio);
        
        // Mode attaque : lecture 1:1 du fichier original (pas de changement de pitch)
        if (!x->in_sustain && (int)pos_global >= t_start_output && (int)pos_global < attack_end_output) {
            if ((int)pos_global == t_start_output) {
                t_atom a[2];
                SETSYMBOL(&a[0], gensym("attaque"));
                SETFLOAT(&a[1], (float)attack_duration_output / x->sample_rate * 1000.0f); // durée en ms
                outlet_anything(x->x_bang_out, gensym("mode"), 2, a);
                x->play_pos = 0; // Reset du compteur pour l'attaque
            }
            
            // Position dans le fichier original : lecture directe sans tempo scaling
            int source_pos_attack = t_start + x->play_pos;
            x->play_pos++;
            
            if (source_pos_attack < x->audio_length)
                out[i] = x->audio_buffer[source_pos_attack];
            else
                out[i] = 0;
                
            if ((int)pos_global + 1 >= attack_end_output) {
                x->in_sustain = 1;
                x->sustain_pos = 0;
                t_atom a;
                SETSYMBOL(&a, gensym("sustain"));
                outlet_anything(x->x_bang_out, gensym("mode"), 1, &a);
            }
        }
        // Mode sustain : utilise le tempo scaling et le loop
        else if (x->in_sustain && (int)pos_global >= attack_end_output && (int)pos_global < t_end_output) {
            // Position de début du release dans le fichier original (50% entre marqueurs)
            int release_start_source = t_start + (t_end - t_start) / 2;
            
            // Longueur du loop : de 50% à 100% entre les marqueurs
            int loop_len = (t_end - t_start) / 2;
            
            // Position dans le loop (revient au début du release à chaque cycle)
            int loop_pos_source = release_start_source + (x->sustain_pos % loop_len);
            
            // Fenêtre de Hann pour le loop
            int fade_len = loop_len / 10;
            float hann = 1.0f;
            if (x->sustain_pos % loop_len < fade_len) {
                int fade_pos = x->sustain_pos % loop_len;
                hann = 0.5f * (1 - cosf(3.14159f * fade_pos / fade_len));
            } else if (x->sustain_pos % loop_len > loop_len - fade_len) {
                int fade_pos = (x->sustain_pos % loop_len) - (loop_len - fade_len);
                hann = 0.5f * (1 - cosf(3.14159f * (fade_len - fade_pos) / fade_len));
            }
            
            if (loop_pos_source < x->audio_length)
                out[i] = x->audio_buffer[loop_pos_source] * hann;
            else
                out[i] = 0;
                
            x->sustain_pos++;
            
            // Fin du sustain
            if ((int)pos_global + 1 >= t_end_output) {
                x->current_transient++;
                x->in_sustain = 0;
                x->sustain_pos = 0;
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
    CLASS_MAINSIGNALIN(timestretch_beatmode_tilde_class, t_timestretch_beatmode_tilde, f);
}
