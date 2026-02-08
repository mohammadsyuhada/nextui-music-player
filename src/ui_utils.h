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
    int scroll_offset;      // Current pixel offset for smooth scrolling
    bool use_gpu_scroll;    // True = use GPU layer (for lists), False = software (for player)
    int last_x, last_y;     // Last render position (for animate-only mode)
    TTF_Font* last_font;    // Last font used (for animate-only mode)
    SDL_Color last_color;   // Last color used (for animate-only mode)
    SDL_Surface* cached_scroll_surface;  // Cached surface for GPU scroll (no bg)
    bool scroll_active;     // True once GPU scroll has actually started (after delay)
} ScrollTextState;

// Reset scroll state for new text
// use_gpu: true for lists (GPU layer with pill bg), false for player (software, no bg)
void ScrollText_reset(ScrollTextState* state, const char* text, TTF_Font* font, int max_width, bool use_gpu);

// Check if scrolling is active (text needs to scroll)
bool ScrollText_isScrolling(ScrollTextState* state);

// Check if scroll needs a render to transition from delay to active
bool ScrollText_needsRender(ScrollTextState* state);

// Activate scrolling after delay (for player screens that bypass ScrollText_render)
void ScrollText_activateAfterDelay(ScrollTextState* state);

// Update scroll animation only (for GPU mode, doesn't redraw screen)
// Call this when dirty=0 but scrolling is active - uses saved position from last render
void ScrollText_animateOnly(ScrollTextState* state);

// Render scrolling text (call every frame)
void ScrollText_render(ScrollTextState* state, TTF_Font* font, SDL_Color color,
                       SDL_Surface* screen, int x, int y);

// Unified update: checks for text change, resets if needed, and renders
// use_gpu: true for lists (GPU layer with pill bg), false for player (software, no bg)
void ScrollText_update(ScrollTextState* state, const char* text, TTF_Font* font,
                       int max_width, SDL_Color color, SDL_Surface* screen, int x, int y, bool use_gpu);

// GPU scroll without background (for player title)
// Uses PLAT_drawOnLayer to render to GPU layer without pill background
void ScrollText_renderGPU_NoBg(ScrollTextState* state, TTF_Font* font,
                                SDL_Color color, int x, int y);

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
// Combines: Fonts_calcListPillWidth + Fonts_drawListItemBg + text position calculation
// prefix_width: extra width to account for (e.g., checkbox, indicator)
ListItemPos render_list_item_pill(SDL_Surface* screen, ListLayout* layout,
                                   const char* text, char* truncated,
                                   int y, bool selected, int prefix_width);

// Position information returned by render_list_item_pill_badged
typedef struct {
    int pill_width;     // Width of the title (inner) pill
    int text_x;         // X position for title text (after padding)
    int text_y;         // Y position for title text (medium font, row 1)
    int subtitle_x;     // X position for subtitle text (row 2)
    int subtitle_y;     // Y position for subtitle text (small font, row 2)
    int badge_x;        // X position for badge content start
    int badge_y;        // Y position for badge content (tiny font, centered)
    int total_width;    // Total width of title pill + badge area
    int text_max_width; // Max width for text content (accounts for capsule radius)
} ListItemBadgedPos;

// Render a list item's pill with optional right-side badge area (settings-style two-layer)
// When badge_width > 0 and selected: THEME_COLOR2 outer pill + THEME_COLOR1 inner title pill
// When badge_width == 0: behaves like render_list_item_pill
// Caller renders badge content at badge_x, badge_y
ListItemBadgedPos render_list_item_pill_badged(SDL_Surface* screen, ListLayout* layout,
                                                const char* text, char* truncated,
                                                int y, bool selected, int badge_width);

// Position information returned by render_list_item_pill_rich
typedef struct {
    int pill_width;              // Width of the rendered pill
    int title_x, title_y;       // Row 1 position (medium font)
    int subtitle_x, subtitle_y; // Row 2 position (small font)
    int image_x, image_y;       // Image position (top-left corner)
    int image_size;              // Image width & height (square)
    int text_max_width;          // Max width for text (for scrolling)
} ListItemRichPos;

// Render a 2-row list item pill with image area on the left
// Height is 1.5x PILL_SIZE. Image is square, vertically centered.
// Row 1: title (medium font), Row 2: subtitle (small font)
// Caller renders image at image_x/image_y and text via render_list_item_text()
ListItemRichPos render_list_item_pill_rich(SDL_Surface* screen, ListLayout* layout,
                                            const char* title, const char* subtitle,
                                            char* truncated,
                                            int y, bool selected, bool has_image,
                                            int extra_subtitle_width);

// Position information returned by render_menu_item_pill
typedef struct {
    int pill_width;   // Width of the rendered pill
    int text_x;       // X position for text (after padding)
    int text_y;       // Y position for text (vertically centered in pill)
    int item_y;       // Y position of this menu item
} MenuItemPos;

// Render a menu item's pill background and calculate text position
// Menu items have small spacing (2px) between them (item_h includes margin, pill uses PILL_SIZE)
// index: menu item index (0-based)
// prefix_width: extra width to account for (e.g., icon)
MenuItemPos render_menu_item_pill(SDL_Surface* screen, ListLayout* layout,
                                   const char* text, char* truncated,
                                   int index, bool selected, int prefix_width);

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

// Callback to get icon for a menu item
// Returns SDL_Surface* icon or NULL if no icon for this item
typedef SDL_Surface* (*MenuItemIconCallback)(int index, bool selected);

// Configuration for generic simple menu rendering
typedef struct {
    const char* title;                    // Header title
    const char** items;                   // Array of menu item labels
    int item_count;                       // Number of items
    const char* btn_b_label;              // B button label ("EXIT", "BACK", etc.)
    MenuItemLabelCallback get_label;      // Optional: customize item label
    MenuItemBadgeCallback render_badge;   // Optional: render right-side badge
    MenuItemIconCallback get_icon;        // Optional: get icon for item
} SimpleMenuConfig;

// Render a simple menu with optional customization callbacks
void render_simple_menu(SDL_Surface* screen, int show_setting, int menu_selected,
                        const SimpleMenuConfig* config);

// ============================================
// Rounded Rectangle Background
// ============================================

// Render a filled rounded rectangle background
// Works at any height (unlike pill asset which requires PILL_SIZE)
// Uses two overlapping rects to create corner inset effect
void render_rounded_rect_bg(SDL_Surface* screen, int x, int y, int w, int h, uint32_t color);

// ============================================
// Toast Notification (GPU layer, highest z-index)
// ============================================

// Render toast notification to GPU layer (above all other content including scroll text)
// Call this at the end of your render function. Toast auto-hides after TOAST_DURATION.
void render_toast(SDL_Surface* screen, const char* message, uint32_t toast_time);

// Clear toast from GPU layer (call when leaving screen or clearing state)
void clear_toast(void);

#endif
