#ifndef COLOR_H
#define COLOR_H

#include <stdint.h>

// Standard VGA colors (16 colors)
enum VGA_COLOR {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN   = 14,
    VGA_COLOR_WHITE         = 15,
};

// Build an 8-bit color attribute from foreground and background
static inline uint8_t vga_make_color(VGA_COLOR fg, VGA_COLOR bg) {
    return (bg << 4) | (fg & 0x0F);
}

// Build a VGA entry (character + color)
static inline uint16_t vga_make_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

// Default color (white on black)
#define VGA_DEFAULT_COLOR vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK)

#endif // COLOR_H