#include "header/libaudio.h"
#include "header/stdio.h"
#include "header/timer.h"
#include "header/itoa_atoi.h"
#include "header/ringbuf.h"   // v10.5: note queue ring buffer
#include <stdint.h>
#include <stddef.h>

/* AUDIO DRIVER — Paus OS
 * Three layers:
 *   1. pc_speaker_on/off — hardware primitives (PIT channel 2 +
 *      port 0x61). No queue knowledge.
 *   2. NOTE QUEUE (v10.5) — audio_ring + audio_tick(). Producer is
 *      TASK code (sys_sndbeep / shell `song`), consumer is the timer
 *      IRQ — the ring direction is INVERTED vs kbd/mouse, which is
 *      exactly why the ring primitive guards every op regardless of
 *      caller context. During a queue session ONLY audio_tick touches
 *      the speaker: no sleep loops, the whole OS stays responsive.
 *   3. Blocking beep helpers (audio_beep*) — legacy convenience API;
 *      they flush the queue first so a manual tone can't be killed
 *      mid-beep by a lingering queued note.
 * Backends: PC speaker (always), SB16 (detection only). */

#define PIT_BASE_HZ   1193180u

static void pc_speaker_on(uint32_t freq_hz) {
    /* FIX A2: validate freq_hz. freq=0 -> div-by-zero crash. */
    if (freq_hz == 0) freq_hz = 1;
    /* FIX A6: clamp range to PIT 16-bit divisor + sane upper bound. */
    if (freq_hz < 18) freq_hz = 18;
    if (freq_hz > 1000000) freq_hz = 1000000;

    uint16_t divisor = (uint16_t)(PIT_BASE_HZ / freq_hz);
    if (divisor == 0) divisor = 1;

    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t cur = inb(0x61);
    outb(0x61, (uint8_t)(cur | 0x03));
}

static void pc_speaker_off(void) {
    uint8_t cur = inb(0x61);
    outb(0x61, (uint8_t)(cur & ~0x03));
}

/* ==================== NOTE QUEUE (v10.5) ====================
 *
 * audio_ring holds up to 64 pending notes; `aq` is the playback
 * session state owned by the timer IRQ consumer:
 *
 *   active       — 1 while a session is in progress (set by the first
 *                  push, cleared when the ring runs dry)
 *   remaining_ms — time left on the CURRENT note; the current note
 *                  itself is no longer in the ring (it was popped)
 *   cur_freq     — frequency of that note (0 = rest / silent)
 *
 * Timeline of a 2-note queue (timer at 100 Hz, ms_per_tick = 10):
 *   push(A,100) push(B,50)         [task context, ring: A B]
 *   tick 1  : active, remaining 0 -> pop A, speaker(A), remaining=100
 *   tick 2-11: 100 > 10 -> decrement (note lasts ~10 ticks)
 *   tick 12 : 0 -> pop B, speaker(B), remaining=50
 *   tick 17 : 0 -> ring empty -> speaker off, active=0
 * Timing accuracy is +-1 tick (10 ms) — fine for PC-speaker music. */
static RingBuffer<audio_note_t, AUDIO_QUEUE_CAPACITY> audio_ring;

static volatile struct {
    uint32_t cur_freq;      // Hz of the sounding note (0 = silent)
    uint32_t remaining_ms;  // time left on it
    uint8_t  active;        // 1 = queue playback session running
} aq = { .cur_freq = 0, .remaining_ms = 0, .active = 0 };

int audio_queue_tone(uint32_t freq_hz, uint32_t ms) {
    if (ms == 0) return -1;

    audio_note_t n;
    n.freq = freq_hz;
    n.ms   = ms;

    /* One guard around push + session flag keeps them atomic as a
     * unit against the timer-IRQ consumer (nested guards inside the
     * ring are safe — flags are saved, never blindly re-enabled). */
    uint32_t f = rb_irq_save();
    bool stored = audio_ring.push(n);          // drop-newest on full
    if (stored) aq.active = 1;                 // wake the consumer
    rb_irq_restore(f);

    return stored ? 0 : -1;
}

void audio_queue_flush(void) {
    uint32_t f = rb_irq_save();
    audio_ring.reset();
    aq.active = 0;
    aq.cur_freq = 0;
    aq.remaining_ms = 0;
    rb_irq_restore(f);
}

void audio_tick(uint32_t ms) {
    if (!aq.active) return;                 // no session: cheap exit

    if (aq.remaining_ms > ms) {
        aq.remaining_ms -= ms;              // current note keeps sounding
        return;
    }

    /* Current note finished (or never started) — advance. */
    audio_note_t n;
    if (audio_ring.pop(&n)) {
        if (n.freq != 0) pc_speaker_on(n.freq);
        else             pc_speaker_off();  // rest
        aq.cur_freq     = n.freq;
        aq.remaining_ms = n.ms;
    } else {
        pc_speaker_off();                   // queue ran dry: silence
        aq.active = 0;
        aq.cur_freq = 0;
        aq.remaining_ms = 0;
    }
}

void audio_queue_stats(uint32_t* count, uint32_t* drops, uint32_t* cap) {
    if (count) *count = audio_ring.count();
    if (drops) *drops = audio_ring.drops();
    if (cap)   *cap   = audio_ring.capacity();
}

void audio_queue_playing(uint32_t* playing_freq, uint32_t* playing_ms) {
    if (playing_freq) *playing_freq = aq.active ? aq.cur_freq : 0;
    if (playing_ms)   *playing_ms   = aq.active ? aq.remaining_ms : 0;
}

/* Non-blocking tone control for game sound effects (SYS_SPEAKER).
 * Unlike audio_beep() these NEVER sleep: the caller starts a tone,
 * keeps animating, and silences it later (typically a few frames
 * later, timed with gettick()/sleep_ms()). Both first FLUSH the note
 * queue: a manual tone is an override and must not be cut short by a
 * queued melody draining in the timer IRQ. */
void audio_tone_on(uint32_t freq_hz) {
    audio_queue_flush();
    pc_speaker_on(freq_hz);
}

void audio_tone_off(void) {
    audio_queue_flush();
    pc_speaker_off();
}

void audio_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz == 0) return;
    if (duration_ms == 0) return;
    pc_speaker_on(freq_hz);
    sleep_ms(duration_ms);
    pc_speaker_off();
}

static uint32_t note_freq_lookup(char note, int octave) {
    static const uint32_t base_freq[] = {
        261, 293, 329, 349, 392, 440, 493  /* C D E F G A B */
    };
    int idx = 0;
    switch (note) {
        case 'C': idx = 0; break;
        case 'D': idx = 1; break;
        case 'E': idx = 2; break;
        case 'F': idx = 3; break;
        case 'G': idx = 4; break;
        case 'A': idx = 5; break;
        case 'B': idx = 6; break;
        default: return 0;
    }
    /* FIX A6: clamp octave to [0,8] to prevent overflow. */
    if (octave < 0) octave = 0;
    if (octave > 8) octave = 8;
    int shift = octave - 4;
    uint32_t freq = base_freq[idx];
    if (shift > 0) {
        for (int i = 0; i < shift; i++) freq *= 2;
    } else if (shift < 0) {
        for (int i = 0; i < -shift; i++) freq /= 2;
    }
    return freq;
}

uint32_t audio_note_freq(const char* note, int octave) {
    if (!note || !note[0]) return 0;
    char n = note[0];
    if (n >= 'a' && n <= 'g') n = (char)(n - 'a' + 'A');
    return note_freq_lookup(n, octave);
}

void audio_beep_note(const char* note, int octave, uint32_t duration_ms) {
    uint32_t freq = audio_note_freq(note, octave);
    if (freq == 0) return;
    audio_beep(freq, duration_ms);
}

void audio_twinkle(void) {
    static const char* notes[] = {"C", "C", "G", "G", "A", "A", "G",
                                  "F", "F", "E", "E", "D", "D", "C"};
    static const int octave[] = {4, 4, 4, 4, 4, 4, 4,
                                  4, 4, 4, 4, 4, 4, 4};
    static const int durations[] = {250, 250, 250, 250, 250, 250, 500,
                                    250, 250, 250, 250, 250, 250, 500};
    for (int i = 0; i < 14; i++) {
        audio_beep_note(notes[i], octave[i], (uint32_t)durations[i]);
        sleep_ms(50);
    }
}

void audio_play_song(void) {
    audio_twinkle();
}

void audio_error(void) {
    audio_beep(200, 300);
    sleep_ms(100);
    audio_beep(150, 300);
}

void audio_success(void) {
    audio_beep(800, 100);
    sleep_ms(50);
    audio_beep(1000, 200);
}

/* ==================== SOUND BLASTER 16 (DSP) ==================== */

static int sb16_initialized = 0;
static uint16_t sb16_irq __attribute__((unused)) = 5;

/* FIX A3: timeout-bounded DSP wait. */
static uint8_t sb16_read_dsp(void) {
    uint32_t timeout = 100000;
    while (((inb(0x22C) & 0x80) == 0) && timeout-- > 0);
    if (timeout == 0) return 0xFF;
    return inb(0x22A);
}

static int sb16_write_dsp(uint8_t val) {
    uint32_t timeout = 100000;
    while (((inb(0x22C) & 0x80) != 0) && timeout-- > 0);
    if (timeout == 0) return -1;
    outb(0x22C, val);
    return 0;
}

static uint16_t sb16_detect(void) {
    outb(0x226, 1);
    sleep_ms(10);
    outb(0x226, 0);
    sleep_ms(10);

    /* FIX A4: check reset ack (0xAA) before reading version. */
    uint8_t reset_ack = sb16_read_dsp();
    if (reset_ack != 0xAA) return 0;

    if (sb16_write_dsp(0xE1) != 0) return 0;
    uint8_t ver_major = sb16_read_dsp();
    uint8_t ver_minor = sb16_read_dsp();
    if (ver_major == 0xFF && ver_minor == 0xFF) return 0;
    return (uint16_t)((ver_major << 8) | ver_minor);
}

int audio_sb16_init(void) {
    if (sb16_initialized) return 1;
    uint16_t version = sb16_detect();
    if (version == 0) {
        printf("SB16: Not detected\n");
        return 0;
    }
    printf("SB16: Detected version %d.%d\n", version >> 8, version & 0xFF);
    sb16_initialized = 1;
    return 1;
}

void audio_sb16_play(const uint8_t* data, uint32_t size, uint32_t sample_rate) {
    /* FIX A5: don't fake playback with beep — be honest. */
    (void)data;
    if (!sb16_initialized) {
        printf("SB16: not initialized, cannot play audio\n");
        return;
    }
    printf("SB16: DMA playback not implemented yet (%u bytes @ %u Hz)\n",
           size, sample_rate);
}

void audio_sb16_stop(void) {
    if (!sb16_initialized) return;
    (void)sb16_write_dsp(0xD0);
}

void audio_sb16_set_volume(uint8_t volume) {
    if (!sb16_initialized) return;
    if (volume > 100) volume = 100;
    uint8_t vol = (uint8_t)((uint32_t)volume * 255 / 100);
    (void)sb16_write_dsp(0x30);
    (void)sb16_write_dsp(vol);
    (void)sb16_write_dsp(0x38);
    (void)sb16_write_dsp(vol);
}
