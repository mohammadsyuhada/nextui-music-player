#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "defines.h"
#include "api.h"
#include "ui_fonts.h"

// Path to Next font (font1.ttf) - supports CJK characters
#define NEXT_FONT_PATH RES_PATH "/font1.ttf"

// Custom font size for title (larger than system fonts)
#define FONT_TITLE_SIZE 28

// Custom fonts for the interface (except buttons)
typedef struct {
    TTF_Font* title;      // Track title (28pt)
    TTF_Font* large;      // List items, menus (16pt)
    TTF_Font* artist;     // Artist name (14pt)
    TTF_Font* album;      // Album name (12pt)
    TTF_Font* badge;      // Format badge, small text (12pt)
    TTF_Font* tiny;       // Genre, bitrate (10pt)
    bool loaded;          // True if custom fonts were loaded
} CustomFonts;

static CustomFonts custom_font = {0};

// Load Next font (font1.ttf) at custom sizes
void load_custom_fonts(void) {
    // Load all font sizes from font1.ttf
    custom_font.title = TTF_OpenFont(NEXT_FONT_PATH, SCALE1(FONT_TITLE_SIZE));
    custom_font.large = TTF_OpenFont(NEXT_FONT_PATH, SCALE1(FONT_LARGE));
    custom_font.artist = TTF_OpenFont(NEXT_FONT_PATH, SCALE1(FONT_MEDIUM));
    custom_font.album = TTF_OpenFont(NEXT_FONT_PATH, SCALE1(FONT_SMALL));
    custom_font.badge = TTF_OpenFont(NEXT_FONT_PATH, SCALE1(FONT_SMALL));
    custom_font.tiny = TTF_OpenFont(NEXT_FONT_PATH, SCALE1(FONT_TINY));

    if (custom_font.title && custom_font.large && custom_font.artist &&
        custom_font.album && custom_font.badge && custom_font.tiny) {
        custom_font.loaded = true;
    } else {
        // Failed to load, cleanup partial loads and fall back to system fonts
        if (custom_font.title) { TTF_CloseFont(custom_font.title); custom_font.title = NULL; }
        if (custom_font.large) { TTF_CloseFont(custom_font.large); custom_font.large = NULL; }
        if (custom_font.artist) { TTF_CloseFont(custom_font.artist); custom_font.artist = NULL; }
        if (custom_font.album) { TTF_CloseFont(custom_font.album); custom_font.album = NULL; }
        if (custom_font.badge) { TTF_CloseFont(custom_font.badge); custom_font.badge = NULL; }
        if (custom_font.tiny) { TTF_CloseFont(custom_font.tiny); custom_font.tiny = NULL; }
        custom_font.loaded = false;
    }
}

// Cleanup custom fonts
void unload_custom_fonts(void) {
    if (custom_font.title) { TTF_CloseFont(custom_font.title); custom_font.title = NULL; }
    if (custom_font.large) { TTF_CloseFont(custom_font.large); custom_font.large = NULL; }
    if (custom_font.artist) { TTF_CloseFont(custom_font.artist); custom_font.artist = NULL; }
    if (custom_font.album) { TTF_CloseFont(custom_font.album); custom_font.album = NULL; }
    if (custom_font.badge) { TTF_CloseFont(custom_font.badge); custom_font.badge = NULL; }
    if (custom_font.tiny) { TTF_CloseFont(custom_font.tiny); custom_font.tiny = NULL; }
    custom_font.loaded = false;
}

// Get font for specific element (custom or system fallback)
// Title font (28pt) - for track title
TTF_Font* get_font_title(void) {
    return custom_font.loaded ? custom_font.title : font.large;
}

// Artist font (14pt) - for artist name
TTF_Font* get_font_artist(void) {
    return custom_font.loaded ? custom_font.artist : font.medium;
}

// Album font (12pt) - for album name
TTF_Font* get_font_album(void) {
    return custom_font.loaded ? custom_font.album : font.medium;
}

// Large font for general use (menus, list items)
TTF_Font* get_font_large(void) {
    return custom_font.loaded ? custom_font.large : font.large;
}

// Medium font for general use (lists, info)
TTF_Font* get_font_medium(void) {
    return custom_font.loaded ? custom_font.artist : font.medium;
}

// Small font (badges, secondary text)
TTF_Font* get_font_small(void) {
    return custom_font.loaded ? custom_font.badge : font.small;
}

// Tiny font (genre, bitrate)
TTF_Font* get_font_tiny(void) {
    return custom_font.loaded ? custom_font.tiny : font.tiny;
}
