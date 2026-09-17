#ifndef LIBAUDIO_H
#define LIBAUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ======================== PC SPEAKER ========================
// Play beep using PC speaker (port 0x61) and PIT (timer)
void audio_beep(uint32_t freq_hz, uint32_t duration_ms);
void audio_beep_note(const char* note, int octave, uint32_t duration_ms);
void audio_play_song(void);

// Non-blocking PC-speaker tone control for games (SYS_SPEAKER):
// start a tone and keep running, silence it later yourself.
// audio_beep() above is BLOCKING (it sleeps for the duration);
// these two never block. Both FLUSH the note queue first: a manual
// tone always wins over a queued melody (SYS_SPEAKER contract).
void audio_tone_on(uint32_t freq_hz);
void audio_tone_off(void);

// ==================== NOTE QUEUE (v10.5 ring buffer) ====================
// One queued note. freq = 0 is a REST (silence for ms milliseconds).
typedef struct {
    uint32_t freq;   // Hz (0 = rest)
    uint32_t ms;     // duration in milliseconds
} audio_note_t;

#define AUDIO_QUEUE_CAPACITY 64

// Queue one timed note for playback by the kernel timer IRQ. Returns
// 0 on success, -1 when the queue is full (note NOT queued) or ms==0.
// NEVER blocks: a melody is just a series of these calls.
int audio_queue_tone(uint32_t freq_hz, uint32_t ms);

// Discard every pending note and stop queue playback. Does not touch
// the speaker state itself (pair it with audio_tone_on/off).
void audio_queue_flush(void);

// Advance queue playback by `ms` — called from the timer IRQ only.
// The first note starts on the tick after the first push (worst case
// 10 ms latency at the current 100 Hz timer).
void audio_tick(uint32_t ms);

// Diagnostics for the shell `ringstats` command. playing_freq is the
// currently sounding note (0 = silent); playing_ms is the time left
// on it. Any pointer may be NULL.
void audio_queue_stats(uint32_t* count, uint32_t* drops, uint32_t* cap);
void audio_queue_playing(uint32_t* playing_freq, uint32_t* playing_ms);

// ======================== SOUND BLASTER 16 (DSP) ========================
// Initialize Sound Blaster 16 (auto-detect)
int audio_sb16_init(void);

// Play raw 8-bit audio via Sound Blaster 16 DMA
void audio_sb16_play(const uint8_t* data, uint32_t size, uint32_t sample_rate);

// Stop playback
void audio_sb16_stop(void);

// Set volume (0-100)
void audio_sb16_set_volume(uint8_t volume);

// ======================== PC BEEP UTILITIES ========================
// Frequency lookup for notes (C4 = 261.63 Hz)
uint32_t audio_note_freq(const char* note, int octave);

// Simple melody: play Twinkle Twinkle Little Star
void audio_twinkle(void);

// Play a beep for error (short low beep)
void audio_error(void);

// Play a beep for success (short high beep)
void audio_success(void);

#ifdef __cplusplus
}
#endif

#endif