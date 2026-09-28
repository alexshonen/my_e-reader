/**
 * @file gui.h
 * @brief High-contrast UI drawing routines directly into 4-bit PSRAM framebuffer
 *        for LilyGo T5 4.7" S3 E-Paper Display (540x960).
 */

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include "font8x8.h"

#define EPD_WIDTH  540
#define EPD_HEIGHT 960

// Color constants for 4-bit grayscale (0x0 = black, 0xF = white)
#define COLOR_BLACK      0x0
#define COLOR_DARK_GRAY  0x4
#define COLOR_MID_GRAY   0x8
#define COLOR_LIGHT_GRAY 0xC
#define COLOR_WHITE      0xF

inline void fb_clear(uint8_t *fb, uint8_t color = COLOR_WHITE) {
    uint8_t packed = ((color & 0x0F) << 4) | (color & 0x0F);
    memset(fb, packed, (EPD_WIDTH * EPD_HEIGHT) / 2);
}

inline void fb_set_pixel(uint8_t *fb, int x, int y, uint8_t color) {
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;
    size_t idx = (size_t)y * EPD_WIDTH + x;
    size_t byte_idx = idx / 2;
    if ((idx & 1) == 0) {
        fb[byte_idx] = (fb[byte_idx] & 0x0F) | ((color & 0x0F) << 4);
    } else {
        fb[byte_idx] = (fb[byte_idx] & 0xF0) | (color & 0x0F);
    }
}

inline void fb_draw_hline(uint8_t *fb, int x, int y, int w, uint8_t color) {
    for (int i = 0; i < w; i++) {
        fb_set_pixel(fb, x + i, y, color);
    }
}

inline void fb_draw_vline(uint8_t *fb, int x, int y, int h, uint8_t color) {
    for (int i = 0; i < h; i++) {
        fb_set_pixel(fb, x, y + i, color);
    }
}

inline void fb_draw_rect(uint8_t *fb, int x, int y, int w, int h, uint8_t color, bool filled = false) {
    if (filled) {
        for (int row = 0; row < h; row++) {
            fb_draw_hline(fb, x, y + row, w, color);
        }
    } else {
        fb_draw_hline(fb, x, y, w, color);
        fb_draw_hline(fb, x, y + h - 1, w, color);
        fb_draw_vline(fb, x, y, h, color);
        fb_draw_vline(fb, x + w - 1, y, h, color);
    }
}

inline void fb_draw_rounded_rect(uint8_t *fb, int x, int y, int w, int h, int r, uint8_t color, bool filled = false) {
    if (filled) {
        for (int row = 0; row < h; row++) {
            int dx = 0;
            if (row < r) {
                dx = r - (int)sqrt(r * r - (r - row) * (r - row));
            } else if (row >= h - r) {
                int dy = row - (h - r);
                dx = r - (int)sqrt(r * r - dy * dy);
            }
            fb_draw_hline(fb, x + dx, y + row, w - 2 * dx, color);
        }
    } else {
        // Outline rounded rectangle
        fb_draw_hline(fb, x + r, y, w - 2 * r, color);
        fb_draw_hline(fb, x + r, y + h - 1, w - 2 * r, color);
        fb_draw_vline(fb, x, y + r, h - 2 * r, color);
        fb_draw_vline(fb, x + w - 1, y + r, h - 2 * r, color);
    }
}

inline void fb_draw_char(uint8_t *fb, int x, int y, char c, uint8_t color, int scale = 2) {
    uint8_t uc = (uint8_t)c;
    if (uc >= 128) uc = '?';
    for (int row = 0; row < 8; row++) {
        uint8_t b = font8x8_basic[uc][row];
        for (int col = 0; col < 8; col++) {
            if (b & (1 << col)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        fb_set_pixel(fb, x + col * scale + sx, y + row * scale + sy, color);
                    }
                }
            }
        }
    }
}

inline void fb_draw_string(uint8_t *fb, int x, int y, const char *str, uint8_t color, int scale = 2) {
    if (!str) return;
    int cur_x = x;
    int cur_y = y;
    while (*str) {
        if (*str == '\n') {
            cur_y += 10 * scale;
            cur_x = x;
        } else {
            fb_draw_char(fb, cur_x, cur_y, *str, color, scale);
            cur_x += 8 * scale;
        }
        str++;
    }
}

struct BookMetadata {
    char path[80];       // SD directory path, e.g. "/books/Candide"
    char title[64];      // Display title, e.g. "Candide - Voltaire"
    int page_count;      // Total number of pages available
    int current_page;    // Last read page bookmark
};

/**
 * @brief Renders the Library / Bookshelf menu screen onto the PSRAM framebuffer.
 */
inline void fb_render_library_screen(
    uint8_t *fb,
    const BookMetadata *books,
    int total_books,
    int current_book_idx,
    int current_lib_page,
    int books_per_page = 4
) {
    fb_clear(fb, COLOR_WHITE);

    // 1. Header Bar
    fb_draw_rect(fb, 0, 0, EPD_WIDTH, 85, COLOR_BLACK, true);
    fb_draw_string(fb, 24, 18, "BIBLIOTHEQUE / LIBRARY", COLOR_WHITE, 3);
    fb_draw_string(fb, 26, 56, "Touchez un livre pour commencer la lecture", COLOR_LIGHT_GRAY, 2);

    // 2. Book Cards
    int start_idx = current_lib_page * books_per_page;
    int end_idx = min(start_idx + books_per_page, total_books);
    int y_start = 105;
    int card_height = 150;
    int card_spacing = 18;

    if (total_books == 0) {
        fb_draw_rounded_rect(fb, 30, 200, EPD_WIDTH - 60, 240, 8, COLOR_MID_GRAY, false);
        fb_draw_string(fb, 50, 250, "Aucun livre detecte !", COLOR_BLACK, 3);
        fb_draw_string(fb, 50, 310, "Copiez des dossiers de livres dans", COLOR_DARK_GRAY, 2);
        fb_draw_string(fb, 50, 340, "/books/<nom_livre>/ sur la carte SD.", COLOR_BLACK, 2);
    }

    for (int i = start_idx; i < end_idx; i++) {
        int slot = i - start_idx;
        int card_y = y_start + slot * (card_height + card_spacing);
        bool is_current = (i == current_book_idx);

        // Draw card boundary
        if (is_current) {
            // Highlight active book with double border and badge
            fb_draw_rounded_rect(fb, 20, card_y, EPD_WIDTH - 40, card_height, 6, COLOR_BLACK, false);
            fb_draw_rounded_rect(fb, 22, card_y + 2, EPD_WIDTH - 44, card_height - 4, 4, COLOR_BLACK, false);
            fb_draw_rect(fb, EPD_WIDTH - 150, card_y + 12, 115, 26, COLOR_BLACK, true);
            fb_draw_string(fb, EPD_WIDTH - 142, card_y + 16, "EN COURS", COLOR_WHITE, 2);
        } else {
            fb_draw_rounded_rect(fb, 20, card_y, EPD_WIDTH - 40, card_height, 6, COLOR_MID_GRAY, false);
        }

        // Book title (truncate if too long)
        char disp_title[28];
        strncpy(disp_title, books[i].title, sizeof(disp_title) - 1);
        disp_title[sizeof(disp_title) - 1] = '\0';
        fb_draw_string(fb, 38, card_y + 18, disp_title, COLOR_BLACK, 3);

        // Page information & Progress Percentage
        int cur_p = books[i].current_page;
        int tot_p = max(1, books[i].page_count);
        int percent = (cur_p * 100) / tot_p;
        if (percent > 100) percent = 100;

        char progress_str[48];
        snprintf(progress_str, sizeof(progress_str), "Page %d / %d  (%d%%)", cur_p + 1, tot_p, percent);
        fb_draw_string(fb, 38, card_y + 68, progress_str, COLOR_DARK_GRAY, 2);

        // Progress Bar
        int bar_x = 38;
        int bar_y = card_y + 102;
        int bar_w = EPD_WIDTH - 76;
        int bar_h = 16;
        fb_draw_rect(fb, bar_x, bar_y, bar_w, bar_h, COLOR_MID_GRAY, false);
        int fill_w = (bar_w - 4) * percent / 100;
        if (fill_w > 0) {
            fb_draw_rect(fb, bar_x + 2, bar_y + 2, fill_w, bar_h - 4, COLOR_BLACK, true);
        }
    }

    // 3. Footer Navigation Bar
    int footer_y = 860;
    fb_draw_hline(fb, 0, footer_y - 15, EPD_WIDTH, COLOR_LIGHT_GRAY);

    // Prev Page Button (Left)
    fb_draw_rounded_rect(fb, 20, footer_y, 140, 65, 4, COLOR_BLACK, false);
    fb_draw_string(fb, 42, footer_y + 22, "< PREV", COLOR_BLACK, 2);

    // Resume Reading Button (Center)
    fb_draw_rounded_rect(fb, 180, footer_y, 180, 65, 4, COLOR_BLACK, true);
    fb_draw_string(fb, 196, footer_y + 22, "LIRE / RESUME", COLOR_WHITE, 2);

    // Next Page Button (Right)
    fb_draw_rounded_rect(fb, 380, footer_y, 140, 65, 4, COLOR_BLACK, false);
    fb_draw_string(fb, 402, footer_y + 22, "NEXT >", COLOR_BLACK, 2);

    // Library page indicator
    int total_lib_pages = max(1, (total_books + books_per_page - 1) / books_per_page);
    char lib_page_str[32];
    snprintf(lib_page_str, sizeof(lib_page_str), "Biblio %d / %d", current_lib_page + 1, total_lib_pages);
    fb_draw_string(fb, 210, 935, lib_page_str, COLOR_MID_GRAY, 2);
}
