#include <types.h>
#include <globals.h>
#include <terminal.h>

#define CHAR_W 6
#define CHAR_H 9
#define GLYPH_W 5
#define GLYPH_H 7

static size_t cursor_x;
static size_t cursor_y;

static const u8 font_upper[26][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
    {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30},
    {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 14}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14}, {7, 2, 2, 2, 18, 18, 12},
    {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17},
    {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
    {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},
    {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31}
};

static const u8 font_lower[26][7] = {
    {0, 0, 14, 1, 15, 17, 15}, {16, 16, 30, 17, 17, 17, 30},
    {0, 0, 15, 16, 16, 16, 15}, {1, 1, 15, 17, 17, 17, 15},
    {0, 0, 14, 17, 31, 16, 14}, {7, 8, 8, 30, 8, 8, 8},
    {0, 0, 15, 17, 17, 15, 1}, {16, 16, 30, 17, 17, 17, 17},
    {4, 0, 12, 4, 4, 4, 14}, {2, 0, 6, 2, 2, 18, 12},
    {16, 16, 18, 20, 24, 20, 18}, {12, 4, 4, 4, 4, 4, 14},
    {0, 0, 26, 21, 21, 17, 17}, {0, 0, 30, 17, 17, 17, 17},
    {0, 0, 14, 17, 17, 17, 14}, {0, 0, 30, 17, 17, 30, 16},
    {0, 0, 15, 17, 17, 15, 1}, {0, 0, 23, 24, 16, 16, 16},
    {0, 0, 15, 16, 14, 1, 30}, {8, 8, 30, 8, 8, 9, 6},
    {0, 0, 17, 17, 17, 19, 13}, {0, 0, 17, 17, 17, 10, 4},
    {0, 0, 17, 17, 21, 21, 10}, {0, 0, 17, 10, 4, 10, 17},
    {0, 0, 17, 17, 15, 1, 14}, {0, 0, 31, 2, 4, 8, 31}
};

static const u8 font_digits[10][7] = {
    {14, 17, 19, 21, 25, 17, 14},
    {4, 12, 4, 4, 4, 4, 14},
    {14, 17, 1, 2, 4, 8, 31},
    {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2},
    {31, 16, 16, 30, 1, 1, 30},
    {14, 16, 16, 30, 17, 17, 14},
    {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14},
    {14, 17, 17, 15, 1, 1, 14}
};

static const pixel_t FG = 0x00E6E6E6;
static const pixel_t BG = 0x00101820;

static inline pixel_t *row_ptr(u32 y) {
    return (pixel_t *)((u8 *)fb_addr + ((size_t)y * fb_pixperline));
}

static u8 glyph_row(char c, unsigned row) {
    if (c >= 'A' && c <= 'Z')
        return font_upper[(unsigned)(c - 'A')][row];

    if (c >= 'a' && c <= 'z')
        return font_lower[(unsigned)(c - 'a')][row];

    if (c >= '0' && c <= '9')
        return font_digits[(unsigned)(c - '0')][row];

    switch (c) {
        case ' ': return 0;
        case '!': { static const u8 g[7] = {4, 4, 4, 4, 4, 0, 4}; return g[row]; }
        case '"': { static const u8 g[7] = {10, 10, 10, 0, 0, 0, 0}; return g[row]; }
        case '#': { static const u8 g[7] = {10, 31, 10, 10, 31, 10, 0}; return g[row]; }
        case '%': { static const u8 g[7] = {25, 25, 2, 4, 8, 19, 19}; return g[row]; }
        case '&': { static const u8 g[7] = {6, 9, 10, 4, 10, 17, 14}; return g[row]; }
        case '\'': { static const u8 g[7] = {12, 4, 0, 0, 0, 0, 0}; return g[row]; }
        case '(': { static const u8 g[7] = {2, 4, 8, 8, 8, 4, 2}; return g[row]; }
        case ')': { static const u8 g[7] = {8, 4, 2, 2, 2, 4, 8}; return g[row]; }
        case '*': { static const u8 g[7] = {0, 10, 4, 31, 4, 10, 0}; return g[row]; }
        case '+': { static const u8 g[7] = {0, 4, 4, 31, 4, 4, 0}; return g[row]; }
        case ',': { static const u8 g[7] = {0, 0, 0, 0, 0, 12, 8}; return g[row]; }
        case '-': { static const u8 g[7] = {0, 0, 0, 31, 0, 0, 0}; return g[row]; }
        case '.': { static const u8 g[7] = {0, 0, 0, 0, 0, 12, 12}; return g[row]; }
        case '/': { static const u8 g[7] = {1, 2, 4, 8, 16, 0, 0}; return g[row]; }
        case ':': { static const u8 g[7] = {0, 12, 12, 0, 12, 12, 0}; return g[row]; }
        case ';': { static const u8 g[7] = {0, 12, 12, 0, 12, 12, 8}; return g[row]; }
        case '<': { static const u8 g[7] = {2, 4, 8, 16, 8, 4, 2}; return g[row]; }
        case '=': { static const u8 g[7] = {0, 0, 31, 0, 31, 0, 0}; return g[row]; }
        case '>': { static const u8 g[7] = {8, 4, 2, 1, 2, 4, 8}; return g[row]; }
        case '?': { static const u8 g[7] = {14, 17, 1, 2, 4, 0, 4}; return g[row]; }
        case '[': { static const u8 g[7] = {14, 8, 8, 8, 8, 8, 14}; return g[row]; }
        case '\\':{ static const u8 g[7] = {16, 8, 4, 2, 1, 0, 0}; return g[row]; }
        case ']': { static const u8 g[7] = {14, 2, 2, 2, 2, 2, 14}; return g[row]; }
        case '_': { static const u8 g[7] = {0, 0, 0, 0, 0, 0, 31}; return g[row]; }
        case '`': { static const u8 g[7] = {12, 4, 0, 0, 0, 0, 0}; return g[row]; }
        case '{': { static const u8 g[7] = {6, 8, 8, 24, 8, 8, 6}; return g[row]; }
        case '|': { static const u8 g[7] = {4, 4, 4, 4, 4, 4, 4}; return g[row]; }
        case '}': { static const u8 g[7] = {12, 2, 2, 3, 2, 2, 12}; return g[row]; }
        case '~': { static const u8 g[7] = {0, 0, 8, 21, 2, 0, 0}; return g[row]; }
        case '^': { static const u8 g[7] = {4, 10, 17, 0, 0, 0, 0}; return g[row]; }
        case '@': { static const u8 g[7] = {14, 17, 23, 21, 23, 16, 14}; return g[row]; }
        default: return 0;
    }
}

static void clear_row(u32 y) {
    pixel_t *row = row_ptr(y);

    for (u32 x = 0; x < fb_w; ++x)
        row[x] = BG;
}

void terminal_clear(void) {
    for (u32 y = 0; y < fb_h; ++y)
        clear_row(y);

    cursor_x = 0;
    cursor_y = 0;
}

static void scroll(void) {
    if (fb_h <= CHAR_H)
        return;

    for (u32 y = 0; y + CHAR_H < fb_h; ++y) {
        pixel_t *dst = row_ptr(y);
        pixel_t *src = row_ptr(y + CHAR_H);

        for (u32 x = 0; x < fb_w; ++x)
            dst[x] = src[x];
    }

    for (u32 y = fb_h - CHAR_H; y < fb_h; ++y)
        clear_row(y);
}

void terminal_init(void) {
    if (!fb_addr || !fb_w || !fb_h || !fb_pixperline)
        return;

    terminal_clear();
}

void terminal_putchar(char c) {
    if (!fb_addr || !fb_w || !fb_h || !fb_pixperline)
        return;

    if (c == '\r') {
        cursor_x = 0;
        return;
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y += CHAR_H;

        if (cursor_y + GLYPH_H > fb_h) {
            scroll();
            cursor_y = fb_h - CHAR_H;
        }

        return;
    }

    if (c == '\b') {
        if (cursor_x >= CHAR_W)
            cursor_x -= CHAR_W;

        for (u32 gy = 0; gy < GLYPH_H; ++gy) {
            pixel_t *row = row_ptr((u32)(cursor_y + gy));

            for (u32 gx = 0; gx < GLYPH_W; ++gx)
                row[cursor_x + gx] = BG;
        }

        return;
    }

    if (cursor_x + GLYPH_W > fb_w) {
        cursor_x = 0;
        cursor_y += CHAR_H;
    }

    if (cursor_y + GLYPH_H > fb_h) {
        scroll();
        cursor_y = fb_h - CHAR_H;
    }

    for (unsigned gy = 0; gy < GLYPH_H; ++gy) {
        u8 bits = glyph_row(c, gy);
        pixel_t *row = row_ptr((u32)(cursor_y + gy));

        for (unsigned gx = 0; gx < GLYPH_W; ++gx) {
            row[cursor_x + gx] =
                (bits & (1u << (GLYPH_W - 1 - gx))) ? FG : BG;
        }
    }

    cursor_x += CHAR_W;
}

void terminal_write(const char *str) {
    if (!str)
        return;

    while (*str)
        terminal_putchar(*str++);
}
