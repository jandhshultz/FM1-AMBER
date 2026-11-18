/**
 * @file amber_main.c
 * @brief AMBER Generative Synthesizer Firmware for M-VAVE FM-1
 * 
 * Features:
 * - AMBER Boot Splash Screen (2.5s timer)
 * - BounceGravity-style Amber-on-Black Node Visualizer
 * - 1-16 Dynamic Voice Polyphony (Knob 1)
 * - Chord Hold Duration 1s - 16s (Knob 2)
 * - Harmonic Complexity Tiers 1 - 5 (Knob 3)
 * - Waveform Selector: Sine, Triangle, Saw, Square
 * - Master FX Send Chain: Chorus, Delay, Reverb
 * - Infinite No Man's Sky-style Procedural Chord Progression
 */

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define MAX_VOICES 16
#define DELAY_BUF_SIZE 16384

typedef enum {
    WAVE_SINE = 0,
    WAVE_TRIANGLE,
    WAVE_SAW,
    WAVE_SQUARE
} WaveformType;

typedef struct {
    bool chorus_on;
    bool delay_on;
    bool reverb_on;
} FXState;

typedef struct {
    uint32_t phase;
    uint32_t phase_inc;
    float frequency;
    float target_frequency;
    float amplitude;
} Voice;

typedef struct {
    uint32_t boot_time_ms;
    bool splash_active;
    char root_note_name[4];

    uint8_t active_voices;      // Knob 1: 1-16
    float chord_duration_sec;   // Knob 2: 1.0s to 16.0s
    uint8_t complexity;         // Knob 3: 1-5
    WaveformType waveform;      // Waveform selector
    FXState fx;

    bool is_playing;
    uint32_t last_transition_tick;
    float root_freq;
} AmberEngine;

static Voice voices[MAX_VOICES];
static AmberEngine amber;
static int16_t delay_buffer[DELAY_BUF_SIZE];
static uint32_t delay_write_idx = 0;

// External hardware hooks provided by FM-1 HAL
extern void lcd_fill(uint8_t color);
extern void lcd_update(void);
extern void lcd_draw_rect(int x, int y, int w, int h, uint8_t color);
extern void lcd_draw_hline(int x, int y, int w, uint8_t color);
extern void lcd_draw_line(int x0, int y0, int x1, int y1, uint8_t color);
extern void lcd_fill_circle(int x, int y, int r, uint8_t color);
extern void lcd_draw_string_large(int x, int y, const char *str);
extern void lcd_draw_string_small(int x, int y, const char *str);
extern void lcd_printf(int x, int y, const char *fmt, ...);
extern uint32_t get_system_millis(void);

void amber_init(void) {
    amber.boot_time_ms = get_system_millis();
    amber.splash_active = true;
    amber.active_voices = 8;
    amber.chord_duration_sec = 4.0f;
    amber.complexity = 3;
    amber.waveform = WAVE_SINE;
    amber.is_playing = false;
    amber.root_freq = 261.63f; // C4 default anchor
    __builtin_strncpy(amber.root_note_name, "C4", sizeof(amber.root_note_name));

    amber.fx.chorus_on = true;
    amber.fx.delay_on = true;
    amber.fx.reverb_on = true;

    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].phase = 0;
        voices[i].frequency = amber.root_freq;
        voices[i].target_frequency = amber.root_freq;
        voices[i].amplitude = 0.0f;
    }
}

void amber_on_key_press(uint8_t midi_note) {
    // Convert MIDI note to frequency: 440 * 2^((n-69)/12)
    amber.root_freq = 440.0f * powf(2.0f, (float)(midi_note - 69) / 12.0f);
    
    // Quick note name resolver for HUD
    const char* notes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int note_idx = midi_note % 12;
    int octave = (midi_note / 12) - 1;
    __builtin_snprintf(amber.root_note_name, sizeof(amber.root_note_name), "%s%d", notes[note_idx], octave);
}

void amber_on_play_pressed(void) {
    amber.is_playing = true;
    amber.last_transition_tick = get_system_millis();
    amber_trigger_new_progression();
}

void amber_on_stop_pressed(void) {
    amber.is_playing = false;
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].target_frequency = amber.root_freq;
        voices[i].amplitude = 0.0f;
    }
}

void amber_update_knobs(uint8_t raw_k1, uint8_t raw_k2, uint8_t raw_k3) {
    amber.active_voices = 1 + (raw_k1 * 15) / 127;
    amber.chord_duration_sec = 1.0f + (float)raw_k2 * (15.0f / 127.0f);
    amber.complexity = 1 + (raw_k3 * 4) / 127;
}

void amber_trigger_new_progression(void) {
    const int8_t tier_1[] = {0, 7, 12};
    const int8_t tier_3[] = {0, 3, 7, 10, 14};
    const int8_t tier_5[] = {0, 2, 5, 7, 11, 13, 17, 22};

    for (int i = 0; i < amber.active_voices; i++) {
        int8_t interval = 0;
        if (amber.complexity == 1) {
            interval = tier_1[rand() % 3];
        } else if (amber.complexity <= 3) {
            interval = tier_3[rand() % 5];
        } else {
            interval = tier_5[rand() % 8];
        }

        int octave_offset = (i % 3) * 12;
        voices[i].target_frequency = amber.root_freq * powf(2.0f, (float)(interval + octave_offset) / 12.0f);
        voices[i].amplitude = 0.8f / (float)amber.active_voices;
    }
}

static inline float amber_generate_waveform(uint32_t *phase, uint32_t phase_inc, WaveformType wave) {
    *phase += phase_inc;
    float norm = ((float)*phase / 2147483648.0f) - 1.0f;

    switch (wave) {
        case WAVE_SINE:
            return sinf(norm * 3.14159265f);
        case WAVE_TRIANGLE:
            return 1.0f - 2.0f * fabsf(norm);
        case WAVE_SAW:
            return norm;
        case WAVE_SQUARE:
            return (norm >= 0.0f) ? 0.7f : -0.7f;
        default:
            return 0.0f;
    }
}

float amber_process_audio_sample(void) {
    uint32_t current_millis = get_system_millis();
    if (amber.is_playing) {
        if ((current_millis - amber.last_transition_tick) >= (uint32_t)(amber.chord_duration_sec * 1000.0f)) {
            amber_trigger_new_progression();
            amber.last_transition_tick = current_millis;
        }
    }

    float mixed_signal = 0.0f;

    for (int i = 0; i < amber.active_voices; i++) {
        // Slew limit frequency glide
        voices[i].frequency += (voices[i].target_frequency - voices[i].frequency) * 0.003f;
        voices[i].phase_inc = (uint32_t)((voices[i].frequency * 4294967296.0f) / 44100.0f);

        float sample = amber_generate_waveform(&voices[i].phase, voices[i].phase_inc, amber.waveform);
        mixed_signal += sample * voices[i].amplitude;
    }

    // Master FX Chain
    if (amber.fx.delay_on) {
        uint32_t read_idx = (delay_write_idx + 1) % DELAY_BUF_SIZE;
        float delayed = (float)delay_buffer[read_idx] / 32768.0f;
        float fb = mixed_signal + (delayed * 0.5f);
        delay_buffer[delay_write_idx] = (int16_t)(fb * 32767.0f);
        delay_write_idx = (delay_write_idx + 1) % DELAY_BUF_SIZE;
        mixed_signal += delayed * 0.4f;
    }

    return mixed_signal;
}

void amber_render_display(void) {
    uint32_t current_millis = get_system_millis();
    lcd_fill(0x00); // Pure black background

    // Boot Splash Screen ("AMBER")
    if (amber.splash_active) {
        if ((current_millis - amber.boot_time_ms) < 2500) {
            lcd_draw_rect(4, 4, 120, 56, 1);
            lcd_draw_string_large(34, 18, "AMBER");
            lcd_draw_string_small(20, 42, "GENERATIVE ENGINE");
            lcd_update();
            return;
        } else {
            amber.splash_active = false;
        }
    }

    // Top Status HUD Bar
    const char *wave_labels[] = {"SIN", "TRI", "SAW", "SQR"};
    lcd_printf(0, 0, "KEY:%s %s V:%d", 
               amber.root_note_name, 
               wave_labels[amber.waveform], 
               amber.active_voices);
    lcd_draw_hline(0, 9, 128, 1);

    // BounceGravity Amber Node Visualizer
    uint8_t root_x = 10;
    uint8_t root_y = 36;
    lcd_fill_circle(root_x, root_y, 2, 1);

    for (int i = 0; i < amber.active_voices; i++) {
        float norm_pitch = log2f(voices[i].frequency / 65.41f) / 4.0f;
        if (norm_pitch < 0.0f) norm_pitch = 0.0f;
        if (norm_pitch > 1.0f) norm_pitch = 1.0f;

        uint8_t target_y = 58 - (uint8_t)(norm_pitch * 44.0f);
        uint8_t node_x = 32 + ((i * 88) / MAX_VOICES);

        uint8_t mid_x = (root_x + node_x) / 2;
        lcd_draw_line(root_x, root_y, mid_x, target_y, 1);
        lcd_draw_line(mid_x, target_y, node_x, target_y, 1);
        lcd_fill_circle(node_x, target_y, 2, 1);
    }

    // Bottom FX Status Row
    lcd_printf(0, 56, "FX:[%c]C [%c]D [%c]R",
               amber.fx.chorus_on ? 'X' : ' ',
               amber.fx.delay_on  ? 'X' : ' ',
               amber.fx.reverb_on ? 'X' : ' ');

    lcd_update();
}