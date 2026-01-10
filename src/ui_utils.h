#ifndef __UI_UTILS_H__
#define __UI_UTILS_H__

#include <stdbool.h>
#include <stdint.h>
#include "defines.h"  // Brings in SDL2 via platform.h -> sdl.h
#include "api.h"      // For SDL types and TTF
#include "player.h"   // For AudioFormat

// Format duration as MM:SS
void format_time(char* buf, int ms);

// Get format name string
const char* get_format_name(AudioFormat format);

// Scrolling text state for marquee animation
typedef struct {
    char text[512];         // Text to display
    int text_width;         // Full text width in pixels
    int max_width;          // Maximum display width
    uint32_t start_time;    // Animation start time
    bool needs_scroll;      // True if text is wider than max_width
} ScrollTextState;

// Reset scroll state for new text
void ScrollText_reset(ScrollTextState* state, const char* text, TTF_Font* font, int max_width);

// Check if scrolling is active (text needs to scroll)
bool ScrollText_isScrolling(ScrollTextState* state);

// Render scrolling text (call every frame)
void ScrollText_render(ScrollTextState* state, TTF_Font* font, SDL_Color color,
                       SDL_Surface* screen, int x, int y);

// Unified update: checks for text change, resets if needed, and renders
void ScrollText_update(ScrollTextState* state, const char* text, TTF_Font* font,
                       int max_width, SDL_Color color, SDL_Surface* screen, int x, int y);

// Render standard screen header (title pill + hardware status)
void render_screen_header(SDL_Surface* screen, const char* title, int show_setting);

// Adjust scroll offset to keep selected item visible
void adjust_list_scroll(int selected, int* scroll, int items_per_page);

// Render scroll up/down indicators for lists
void render_scroll_indicators(SDL_Surface* screen, int scroll, int items_per_page, int total_count);

// ============================================
// Generic List Rendering Helpers
// ============================================

// Layout information for a standard scrollable list
typedef struct {
    int list_y;          // Y position where list starts
    int list_h;          // Total height available for list
    int item_h;          // Height of each item
    int items_per_page;  // Number of visible items
    int max_width;       // Maximum width for content (hw - padding*2)
} ListLayout;

// Calculate standard list layout based on screen dimensions
// Use offset_y for additional offset from header (e.g., for subtitle)
ListLayout calc_list_layout(SDL_Surface* screen, int offset_y);

// Render a list item's text with optional scrolling for selected items
// Returns the text_x position after any prefix (useful for chaining)
// If scroll_state is NULL, no scrolling is used
void render_list_item_text(SDL_Surface* screen, ScrollTextState* scroll_state,
                           const char* text, TTF_Font* font_param,
                           int text_x, int text_y, int max_text_width,
                           bool selected);

// Position information returned by render_list_item_pill
typedef struct {
    int pill_width;   // Width of the rendered pill
    int text_x;       // X position for text (after padding)
    int text_y;       // Y position for text (vertically centered)
} ListItemPos;

// Render a list item's pill background and calculate text position
// Combines: calc_list_pill_width + draw_list_item_bg + text position calculation
// prefix_width: extra width to account for (e.g., checkbox, indicator)
ListItemPos render_list_item_pill(SDL_Surface* screen, ListLayout* layout,
                                   const char* text, char* truncated,
                                   int y, bool selected, int prefix_width);

// Position information returned by render_menu_item_pill
typedef struct {
    int pill_width;   // Width of the rendered pill
    int text_x;       // X position for text (after padding)
    int text_y;       // Y position for text (vertically centered in pill)
    int item_y;       // Y position of this menu item
} MenuItemPos;

// Render a menu item's pill background and calculate text position
// Menu items have spacing between them (item_h includes margin, pill uses PILL_SIZE)
// index: menu item index (0-based)
MenuItemPos render_menu_item_pill(SDL_Surface* screen, ListLayout* layout,
                                   const char* text, char* truncated,
                                   int index, bool selected);

// ============================================
// Generic Simple Menu Rendering
// ============================================

// Callback to customize item label (e.g., "About" -> "About (Update Available)")
// Returns custom label or NULL to use default
typedef const char* (*MenuItemLabelCallback)(int index, const char* default_label,
                                              char* buffer, int buffer_size);

// Callback to render right-side badge (e.g., queue count)
// Called after pill is rendered, can draw additional elements
typedef void (*MenuItemBadgeCallback)(SDL_Surface* screen, int index, bool selected,
                                       int item_y, int item_h);

// Configuration for generic simple menu rendering
typedef struct {
    const char* title;                    // Header title
    const char** items;                   // Array of menu item labels
    int item_count;                       // Number of items
    const char* btn_b_label;              // B button label ("EXIT", "BACK", etc.)
    MenuItemLabelCallback get_label;      // Optional: customize item label
    MenuItemBadgeCallback render_badge;   // Optional: render right-side badge
} SimpleMenuConfig;

// Render a simple menu with optional customization callbacks
void render_simple_menu(SDL_Surface* screen, int show_setting, int menu_selected,
                        const SimpleMenuConfig* config);

#endif
