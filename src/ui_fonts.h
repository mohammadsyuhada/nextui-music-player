#ifndef __UI_FONTS_H__
#define __UI_FONTS_H__

#include <SDL2/SDL_ttf.h>
#include <stdbool.h>

// Initialize custom fonts (call once at startup)
void load_custom_fonts(void);

// Cleanup custom fonts (call at shutdown)
void unload_custom_fonts(void);

// Font accessors - return custom font or system fallback
TTF_Font* get_font_title(void);   // Track title (Regular large)
TTF_Font* get_font_artist(void);  // Artist name (Medium)
TTF_Font* get_font_album(void);   // Album name (Bold)
TTF_Font* get_font_large(void);   // General large (time display)
TTF_Font* get_font_medium(void);  // General medium (lists)
TTF_Font* get_font_small(void);   // Badges, secondary text
TTF_Font* get_font_tiny(void);    // Genre, bitrate

// Theme color helpers for list items (follows system appearance)
SDL_Color get_list_text_color(bool selected);
void draw_list_item_bg(SDL_Surface* screen, SDL_Rect* rect, bool selected);

// Calculate pill width for list items
// prefix_width: width of any prefix elements (indicator, checkbox, status) - pass 0 for simple text
// Returns the calculated pill width and fills truncated buffer with the truncated text
int calc_list_pill_width(TTF_Font* font, const char* text, char* truncated, int max_width, int prefix_width);

#endif
