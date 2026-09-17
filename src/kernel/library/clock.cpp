#include "header/clock.h"
#include "header/stdio.h"
#include "header/color.h"
#include "header/timer.h"
#include "header/libstring.h"
#include <stdint.h>

extern "C" uint32_t timer_freq_hz(void);

static void draw_clock(uint32_t hours, uint32_t minutes, uint32_t seconds) {
    clear_screen();
    set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    set_cursor_position(1, 25);
    printf("=== Paus OS Clock ===");
    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    set_cursor_position(4, 30);
    printf("%02d : %02d : %02d", hours, minutes, seconds);
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    set_cursor_position(6, 25);
    printf("Uptime sejak boot");
    set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    set_cursor_position(10, 25);
    printf("Press ESC to exit");
}

void clock_run(void) {
    uint32_t freq = timer_freq_hz();
    if (freq == 0) freq = 100;

    while (1) {
        uint32_t ticks = get_tick();
        /* BUG FIX M5: use the actual frequency, not a hardcoded 100 */
        uint32_t total_sec = ticks / freq;
        uint32_t hours = total_sec / 3600;
        uint32_t minutes = (total_sec % 3600) / 60;
        uint32_t seconds = total_sec % 60;

        draw_clock(hours, minutes, seconds);

        for (int i = 0; i < 10; i++) {
            sleep_ms(100);
            if (keyboard_has_data()) {
                int key = getchar();
                if (key == 27) {
                    clear_screen();
                    set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                    printf("Clock stopped.\n");
                    return;
                }
            }
        }
    }
}
