#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ui_utils.h"
#include "ui_fonts.h"
#include "module_common.h"

// Format duration as MM:SS
void format_time(char* buf, int ms) {
    int total_secs = ms / 1000;
    int mins = total_secs / 60;
    int secs = total_secs % 60;
    sprintf(buf, "%02d:%02d", mins, secs);
}

// Get format name string
const char* get_format_name(AudioFormat format) {
    switch (format) {
        case AUDIO_FORMAT_MP3: return "MP3";
        case AUDIO_FORMAT_FLAC: return "FLAC";
        case AUDIO_FORMAT_OGG: return "OGG";
        case AUDIO_FORMAT_WAV: return "WAV";
        case AUDIO_FORMAT_MOD: return "MOD";
        case AUDIO_FORMAT_M4A: return "M4A";
        default: return "---";
    }
}

// Scroll gap for software scrolling
#define SCROLL_GAP 30

// Delay before scrolling starts (ms) - show static text first
#define SCROLL_START_DELAY 1000

// Reset scroll state for new text
void ScrollText_reset(ScrollTextState* state, const char* text, TTF_Font* font, int max_width, bool use_gpu) {
    // Clear the scroll layer when text changes to avoid ghost text
    GFX_clearLayers(LAYER_SCROLLTEXT);

    // Free old cached surface if exists
    if (state->cached_scroll_surface) {
        SDL_FreeSurface(state->cached_scroll_surface);
        state->cached_scroll_surface = NULL;
    }

    strncpy(state->text, text, sizeof(state->text) - 1);
    state->text[sizeof(state->text) - 1] = '\0';
    int text_h = 0;
    TTF_SizeUTF8(font, state->text, &state->text_width, &text_h);
    state->max_width = max_width;
    state->start_time = SDL_GetTicks();
    state->scroll_offset = 0;
    state->use_gpu_scroll = use_gpu;
    state->scroll_active = false;

    // Don't enable scrolling yet - delay it so text appears static first
    // needs_scroll will be set to true in ScrollText_render after SCROLL_START_DELAY
    state->needs_scroll = false;

    // Pre-create cached scroll surface for GPU scroll without background
    if ((state->text_width > max_width) && use_gpu) {
        int padding = SCALE1(SCROLL_GAP);
        int total_width = state->text_width * 2 + padding;
        int height = TTF_FontHeight(font);

        state->cached_scroll_surface = SDL_CreateRGBSurfaceWithFormat(0,
            total_width, height, 32, SDL_PIXELFORMAT_ARGB8888);

        if (state->cached_scroll_surface) {
            // Clear to transparent
            SDL_FillRect(state->cached_scroll_surface, NULL, 0);

            // Render text twice for seamless looping
            SDL_Color white = {255, 255, 255, 255};  // Will be overwritten by actual color
            SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font, state->text, white);
            if (text_surf) {
                SDL_SetSurfaceBlendMode(text_surf, SDL_BLENDMODE_NONE);
                SDL_BlitSurface(text_surf, NULL, state->cached_scroll_surface, &(SDL_Rect){0, 0, 0, 0});
                SDL_BlitSurface(text_surf, NULL, state->cached_scroll_surface, &(SDL_Rect){state->text_width + padding, 0, 0, 0});
                SDL_FreeSurface(text_surf);
            }
        }
    }
}

// Check if scrolling is active (text needs to scroll)
bool ScrollText_isScrolling(ScrollTextState* state) {
    return state->needs_scroll;
}

// Check if scroll needs a render to transition from delay to active
bool ScrollText_needsRender(ScrollTextState* state) {
    return state->text[0] && state->text_width > state->max_width && !state->needs_scroll;
}

// Activate scrolling after delay (for player screens that bypass ScrollText_render)
void ScrollText_activateAfterDelay(ScrollTextState* state) {
    if (!state->needs_scroll && state->text_width > state->max_width &&
        SDL_GetTicks() - state->start_time >= SCROLL_START_DELAY) {
        state->needs_scroll = true;
    }
}

// Update scroll animation only (for GPU mode, doesn't redraw screen)
// Call this when dirty=0 but scrolling is active - uses saved position from last render
void ScrollText_animateOnly(ScrollTextState* state) {
    if (!state->text[0] || !state->needs_scroll || !state->use_gpu_scroll) return;
    if (!state->last_font) return;  // Never rendered yet

    // Just update the scroll layer - don't redraw main screen
    GFX_clearLayers(LAYER_SCROLLTEXT);
    GFX_scrollTextTexture(
        state->last_font,
        state->text,
        state->last_x, state->last_y,
        state->max_width,
        TTF_FontHeight(state->last_font),
        state->last_color,
        1.0f,
        NULL  // fontMutex - not needed for single-threaded rendering
    );
}

// Render scrolling text - GPU mode for lists, software mode for player
void ScrollText_render(ScrollTextState* state, TTF_Font* font, SDL_Color color,
                       SDL_Surface* screen, int x, int y) {
    if (!state->text[0]) return;

    // Save position info for animate-only mode
    state->last_x = x;
    state->last_y = y;
    state->last_font = font;
    state->last_color = color;

    // Check if scroll delay has elapsed - activate scrolling
    if (!state->needs_scroll && state->text_width > state->max_width &&
        SDL_GetTicks() - state->start_time >= SCROLL_START_DELAY) {
        if (state->use_gpu_scroll && !state->scroll_active) {
            // First frame after delay: reset GPU scroll and render static text
            // This gives the GPU scroll one frame to initialize before we use it
            GFX_resetScrollText();
            state->scroll_active = true;
        } else {
            state->needs_scroll = true;
        }
    }

    // If text fits (or still in delay/transition), render normally without scrolling
    if (!state->needs_scroll) {
        // Clear scroll layer to remove any previous scrolling text
        GFX_clearLayers(LAYER_SCROLLTEXT);
        SDL_Surface* surf = TTF_RenderUTF8_Blended(font, state->text, color);
        if (surf) {
            SDL_Rect src = {0, 0, surf->w > state->max_width ? state->max_width : surf->w, surf->h};
            SDL_BlitSurface(surf, &src, screen, &(SDL_Rect){x, y, 0, 0});
            SDL_FreeSurface(surf);
        }
        return;
    }

    if (state->use_gpu_scroll) {
        // GPU mode: Use NextUI's scroll text (has pill background)
        GFX_clearLayers(LAYER_SCROLLTEXT);
        GFX_scrollTextTexture(
            font,
            state->text,
            x, y,
            state->max_width,
            TTF_FontHeight(font),
            color,
            1.0f,
            NULL  // fontMutex - not needed for single-threaded rendering
        );
    } else {
        // Software mode: No background, smooth scrolling for player title
        GFX_clearLayers(LAYER_SCROLLTEXT);

        // Render text surface
        SDL_Surface* single_surf = TTF_RenderUTF8_Blended(font, state->text, color);
        if (!single_surf) return;

        // Create combined surface with two text copies for seamless loop
        SDL_Surface* full_surf = SDL_CreateRGBSurfaceWithFormat(0,
            state->text_width * 2 + SCROLL_GAP, single_surf->h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!full_surf) {
            SDL_FreeSurface(single_surf);
            return;
        }

        SDL_FillRect(full_surf, NULL, 0);
        SDL_SetSurfaceBlendMode(single_surf, SDL_BLENDMODE_NONE);
        SDL_BlitSurface(single_surf, NULL, full_surf, &(SDL_Rect){0, 0, 0, 0});
        SDL_BlitSurface(single_surf, NULL, full_surf, &(SDL_Rect){state->text_width + SCROLL_GAP, 0, 0, 0});
        SDL_FreeSurface(single_surf);

        // Simple per-frame increment like NextUI
        state->scroll_offset += 2;
        if (state->scroll_offset >= state->text_width + SCROLL_GAP) {
            state->scroll_offset = 0;
        }

        // Blit the visible portion
        SDL_SetSurfaceBlendMode(full_surf, SDL_BLENDMODE_BLEND);
        SDL_Rect src = {state->scroll_offset, 0, state->max_width, full_surf->h};
        SDL_Rect dst = {x, y, 0, 0};
        SDL_BlitSurface(full_surf, &src, screen, &dst);
        SDL_FreeSurface(full_surf);
    }
}

// Unified update: checks for text change, resets if needed, and renders
// use_gpu: true for lists (GPU layer with pill bg), false for player (software, no bg)
void ScrollText_update(ScrollTextState* state, const char* text, TTF_Font* font,
                       int max_width, SDL_Color color, SDL_Surface* screen, int x, int y, bool use_gpu) {
    // Check if text changed - use existing state->text for comparison
    if (strcmp(state->text, text) != 0) {
        ScrollText_reset(state, text, font, max_width, use_gpu);
    }
    ScrollText_render(state, font, color, screen, x, y);
}

// GPU scroll without background (for player title)
// Uses PLAT_drawOnLayer to render to GPU layer without pill background
void ScrollText_renderGPU_NoBg(ScrollTextState* state, TTF_Font* font,
                                SDL_Color color, int x, int y) {
    if (!state->text[0] || !state->needs_scroll || !state->cached_scroll_surface) {
        // Static text or no scroll needed - just clear layer
        PLAT_clearLayers(LAYER_SCROLLTEXT);
        return;
    }

    // Save render info
    state->last_x = x;
    state->last_y = y;
    state->last_font = font;
    state->last_color = color;

    int padding = SCALE1(SCROLL_GAP);
    int height = state->cached_scroll_surface->h;

    // Create clipped view at current scroll offset (only this is created per-frame)
    SDL_Surface* clipped = SDL_CreateRGBSurfaceWithFormat(0,
        state->max_width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!clipped) return;

    SDL_FillRect(clipped, NULL, 0);
    SDL_SetSurfaceBlendMode(state->cached_scroll_surface, SDL_BLENDMODE_NONE);
    SDL_Rect src = {state->scroll_offset, 0, state->max_width, height};
    SDL_BlitSurface(state->cached_scroll_surface, &src, clipped, NULL);

    // Render to GPU layer
    PLAT_clearLayers(LAYER_SCROLLTEXT);
    PLAT_drawOnLayer(clipped, x, y, state->max_width, height, 1.0f, false, LAYER_SCROLLTEXT);
    SDL_FreeSurface(clipped);

    // Advance scroll offset (1 pixel per frame for smooth, slower scrolling)
    state->scroll_offset += 1;
    if (state->scroll_offset >= state->text_width + padding) {
        state->scroll_offset = 0;
    }

    PLAT_GPU_Flip();
}

// Render standard screen header (title pill + hardware status)
void render_screen_header(SDL_Surface* screen, const char* title, int show_setting) {
    int hw = screen->w;
    char truncated[256];

    int title_width = GFX_truncateText(Fonts_getMedium(), title, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(Fonts_getMedium(), truncated, COLOR_GRAY);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + SCALE1(BUTTON_PADDING), SCALE1(PADDING + 4)});
        SDL_FreeSurface(title_text);
    }

    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }
}

// Adjust scroll offset to keep selected item visible
void adjust_list_scroll(int selected, int* scroll, int items_per_page) {
    if (selected < *scroll) {
        *scroll = selected;
    }
    if (selected >= *scroll + items_per_page) {
        *scroll = selected - items_per_page + 1;
    }
}

// Render scroll up/down indicators for lists
void render_scroll_indicators(SDL_Surface* screen, int scroll, int items_per_page, int total_count) {
    if (total_count <= items_per_page) return;

    int hw = screen->w;
    int hh = screen->h;
    int ox = (hw - SCALE1(24)) / 2;

    if (scroll > 0) {
        // Position just below header with gap from first item
        GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE - BUTTON_MARGIN)});
    }
    if (scroll + items_per_page < total_count) {
        // Position at the end of the list area (just above button hints)
        GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN)});
    }
}

// ============================================
// Generic List Rendering Helpers
// ============================================

// Calculate standard list layout based on screen dimensions
ListLayout calc_list_layout(SDL_Surface* screen, int offset_y) {
    int hw = screen->w;
    int hh = screen->h;

    ListLayout layout;
    layout.list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN) + offset_y;
    layout.list_h = hh - layout.list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    layout.item_h = SCALE1(PILL_SIZE);
    layout.items_per_page = layout.list_h / layout.item_h;
    layout.max_width = hw - SCALE1(PADDING * 2);

    return layout;
}

// Render a list item's text with optional scrolling for selected items
void render_list_item_text(SDL_Surface* screen, ScrollTextState* scroll_state,
                           const char* text, TTF_Font* font_param,
                           int text_x, int text_y, int max_text_width,
                           bool selected) {
    SDL_Color text_color = Fonts_getListTextColor(selected);

    // Set clip rect to prevent any text overflow beyond pill boundary
    // Intersect with existing clip to stay within viewport bounds
    SDL_Rect old_clip;
    SDL_GetClipRect(screen, &old_clip);
    SDL_Rect clip = {text_x, text_y, max_text_width, TTF_FontHeight(font_param)};
    if (old_clip.w > 0 && old_clip.h > 0) {
        int left = clip.x > old_clip.x ? clip.x : old_clip.x;
        int top = clip.y > old_clip.y ? clip.y : old_clip.y;
        int right = (clip.x + clip.w) < (old_clip.x + old_clip.w) ? (clip.x + clip.w) : (old_clip.x + old_clip.w);
        int bottom = (clip.y + clip.h) < (old_clip.y + old_clip.h) ? (clip.y + clip.h) : (old_clip.y + old_clip.h);
        if (right > left && bottom > top) {
            clip = (SDL_Rect){left, top, right - left, bottom - top};
        } else {
            return;  // Entirely outside viewport, skip rendering
        }
    }
    SDL_SetClipRect(screen, &clip);

    if (selected && scroll_state) {
        // Selected item: use scrolling text (GPU mode with pill bg)
        ScrollText_update(scroll_state, text, font_param, max_text_width,
                          text_color, screen, text_x, text_y, true);
    } else {
        // Non-selected items: static rendering with clipping
        SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font_param, text, text_color);
        if (text_surf) {
            SDL_Rect src = {0, 0, text_surf->w > max_text_width ? max_text_width : text_surf->w, text_surf->h};
            SDL_BlitSurface(text_surf, &src, screen, &(SDL_Rect){text_x, text_y, 0, 0});
            SDL_FreeSurface(text_surf);
        }
    }

    // Restore previous clip rect
    if (old_clip.w > 0 && old_clip.h > 0)
        SDL_SetClipRect(screen, &old_clip);
    else
        SDL_SetClipRect(screen, NULL);
}

// Render a list item's pill background and calculate text position
ListItemPos render_list_item_pill(SDL_Surface* screen, ListLayout* layout,
                                   const char* text, char* truncated,
                                   int y, bool selected, int prefix_width) {
    ListItemPos pos;

    // Calculate text width for pill sizing (list items use medium font)
    pos.pill_width = Fonts_calcListPillWidth(Fonts_getMedium(), text, truncated, layout->max_width, prefix_width);

    // Background pill (sized to text width)
    SDL_Rect pill_rect = {SCALE1(PADDING), y, pos.pill_width, layout->item_h};
    Fonts_drawListItemBg(screen, &pill_rect, selected);

    // Calculate text position
    pos.text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
    pos.text_y = y + (layout->item_h - TTF_FontHeight(Fonts_getMedium())) / 2;

    return pos;
}

// Render a 2-row list item pill with optional right-side badge area
// Height is 1.5x PILL_SIZE (same as rich). Title (medium) + subtitle (small) inside pill.
// When badge_width > 0 and selected: THEME_COLOR2 outer capsule + THEME_COLOR1 inner capsule
// When badge_width == 0: single THEME_COLOR1 capsule
ListItemBadgedPos render_list_item_pill_badged(SDL_Surface* screen, ListLayout* layout,
                                                const char* text, char* truncated,
                                                int y, bool selected, int badge_width) {
    ListItemBadgedPos pos;

    int item_h = SCALE1(PILL_SIZE) * 3 / 2;

    // Badge area: badge content + BUTTON_PADDING on each side
    int badge_area_w = badge_width > 0 ? badge_width + SCALE1(BUTTON_PADDING * 2) : 0;

    // Calculate title pill width (reduced max to leave room for badge area)
    int title_max_width = layout->max_width - badge_area_w;
    pos.pill_width = Fonts_calcListPillWidth(Fonts_getMedium(), text, truncated, title_max_width, 0);

    if (selected) {
        int px = SCALE1(PADDING);

        if (badge_area_w > 0) {
            // Layer 1: THEME_COLOR2 outer capsule covering title + badge area
            int total_w = pos.pill_width + badge_area_w;
            int r = item_h / 3;
            if (r > total_w / 2) r = total_w / 2;
            if (item_h - 2 * r > 0) {
                SDL_FillRect(screen, &(SDL_Rect){px, y + r, total_w, item_h - 2 * r}, THEME_COLOR2);
            }
            for (int dy = 0; dy < r; dy++) {
                int yd = r - dy;
                int inset = r - (int)sqrtf((float)(r * r - yd * yd));
                int row_w = total_w - 2 * inset;
                if (row_w <= 0) continue;
                SDL_FillRect(screen, &(SDL_Rect){px + inset, y + dy, row_w, 1}, THEME_COLOR2);
                SDL_FillRect(screen, &(SDL_Rect){px + inset, y + item_h - 1 - dy, row_w, 1}, THEME_COLOR2);
            }
        }

        // Layer 2 (or only layer): THEME_COLOR1 inner capsule for title area
        {
            int pw = pos.pill_width;
            int r = item_h / 3;
            if (r > pw / 2) r = pw / 2;
            if (item_h - 2 * r > 0) {
                SDL_FillRect(screen, &(SDL_Rect){px, y + r, pw, item_h - 2 * r}, THEME_COLOR1);
            }
            for (int dy = 0; dy < r; dy++) {
                int yd = r - dy;
                int inset = r - (int)sqrtf((float)(r * r - yd * yd));
                int row_w = pw - 2 * inset;
                if (row_w <= 0) continue;
                SDL_FillRect(screen, &(SDL_Rect){px + inset, y + dy, row_w, 1}, THEME_COLOR1);
                SDL_FillRect(screen, &(SDL_Rect){px + inset, y + item_h - 1 - dy, row_w, 1}, THEME_COLOR1);
            }
        }
    }

    // Text positions: two rows vertically centered (like rich)
    int text_start_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
    int medium_h = TTF_FontHeight(Fonts_getMedium());
    int small_h = TTF_FontHeight(Fonts_getSmall());
    int total_text_h = medium_h + small_h;
    int top_gap = (item_h - total_text_h) / 2;

    pos.text_x = text_start_x;
    pos.text_y = y + top_gap;

    pos.subtitle_x = text_start_x;
    pos.subtitle_y = y + top_gap + medium_h;

    // Badge position (centered vertically in capsule)
    pos.badge_x = SCALE1(PADDING) + pos.pill_width + SCALE1(BUTTON_PADDING);
    pos.badge_y = y + (item_h - TTF_FontHeight(Fonts_getTiny())) / 2;

    // Account for right-side capsule radius reducing usable text width
    int r = item_h / 2;
    pos.text_max_width = pos.pill_width - SCALE1(BUTTON_PADDING) - r / 2;

    pos.total_width = pos.pill_width + badge_area_w;

    return pos;
}

// Render a 2-row list item pill with image area on the left
// Height is 1.5x PILL_SIZE, fits 4 items per page
ListItemRichPos render_list_item_pill_rich(SDL_Surface* screen, ListLayout* layout,
                                            const char* title, const char* subtitle,
                                            char* truncated,
                                            int y, bool selected, bool has_image,
                                            int extra_subtitle_width) {
    ListItemRichPos pos;

    int item_h = SCALE1(PILL_SIZE) * 3 / 2;
    int img_padding = SCALE1(4);

    // Image area: only reserve space when image is available
    int image_area_w;
    if (has_image) {
        pos.image_size = item_h - img_padding * 2;
        image_area_w = img_padding + pos.image_size + SCALE1(BUTTON_PADDING);
        pos.image_x = SCALE1(PADDING) + img_padding;
        pos.image_y = y + img_padding;
    } else {
        pos.image_size = 0;
        image_area_w = SCALE1(BUTTON_PADDING);  // Just left text padding
        pos.image_x = 0;
        pos.image_y = 0;
    }

    // Calculate pill width considering both title and subtitle
    pos.pill_width = Fonts_calcListPillWidth(Fonts_getMedium(), title, truncated, layout->max_width, image_area_w);
    if (subtitle && subtitle[0]) {
        int sub_w;
        TTF_SizeUTF8(Fonts_getSmall(), subtitle, &sub_w, NULL);
        int sub_pill_w = MIN(layout->max_width, image_area_w + sub_w + extra_subtitle_width + SCALE1(BUTTON_PADDING * 2));
        if (sub_pill_w > pos.pill_width)
            pos.pill_width = sub_pill_w;
    }

    // Draw background (rounded rectangle with reduced radius)
    if (selected) {
        int px = SCALE1(PADDING);
        int pw = pos.pill_width;
        int r = item_h / 3;
        if (r > pw / 2) r = pw / 2;

        // Main body between corner rows
        if (item_h - 2 * r > 0) {
            SDL_FillRect(screen, &(SDL_Rect){px, y + r, pw, item_h - 2 * r}, THEME_COLOR1);
        }
        // Top and bottom corner rows with circular inset
        for (int dy = 0; dy < r; dy++) {
            int yd = r - dy;
            int inset = r - (int)sqrtf((float)(r * r - yd * yd));
            int row_w = pw - 2 * inset;
            if (row_w <= 0) continue;
            SDL_FillRect(screen, &(SDL_Rect){px + inset, y + dy, row_w, 1}, THEME_COLOR1);
            SDL_FillRect(screen, &(SDL_Rect){px + inset, y + item_h - 1 - dy, row_w, 1}, THEME_COLOR1);
        }
    }

    // Text positions: two rows vertically centered
    int text_start_x = SCALE1(PADDING) + image_area_w;
    int medium_h = TTF_FontHeight(Fonts_getMedium());
    int small_h = TTF_FontHeight(Fonts_getSmall());
    int total_text_h = medium_h + small_h;
    int top_gap = (item_h - total_text_h) / 2;

    pos.title_x = text_start_x;
    pos.title_y = y + top_gap;

    pos.subtitle_x = text_start_x;
    pos.subtitle_y = y + top_gap + medium_h;

    pos.text_max_width = pos.pill_width - image_area_w - SCALE1(BUTTON_PADDING);

    return pos;
}

// Render a menu item's pill background and calculate text position
// Menu items have larger spacing (PILL_SIZE + BUTTON_MARGIN) but pill height is just PILL_SIZE
// prefix_width: extra width to account for (e.g., icon)
MenuItemPos render_menu_item_pill(SDL_Surface* screen, ListLayout* layout,
                                   const char* text, char* truncated,
                                   int index, bool selected, int prefix_width) {
    MenuItemPos pos;

    // Menu items have small spacing between them (reduced from BUTTON_MARGIN for tighter layout)
    int item_h = SCALE1(PILL_SIZE + 2);
    pos.item_y = layout->list_y + index * item_h;

    // Calculate text width for pill sizing (include prefix_width for icon)
    pos.pill_width = Fonts_calcListPillWidth(Fonts_getLarge(), text, truncated, layout->max_width - prefix_width, prefix_width);

    // Background pill (pill height is PILL_SIZE, not item_h)
    SDL_Rect pill_rect = {SCALE1(PADDING), pos.item_y, pos.pill_width, SCALE1(PILL_SIZE)};
    Fonts_drawListItemBg(screen, &pill_rect, selected);

    // Calculate text position (centered within PILL_SIZE, not item_h)
    pos.text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
    pos.text_y = pos.item_y + (SCALE1(PILL_SIZE) - TTF_FontHeight(Fonts_getLarge())) / 2;

    return pos;
}

// ============================================
// Rounded Rectangle Background
// ============================================

// Render a filled rounded rectangle with smooth circular corners.
// Corner radius is SCALE1(2), clamped to half the width/height.
// Works at any size — unlike pill asset which requires PILL_SIZE height.
void render_rounded_rect_bg(SDL_Surface* screen, int x, int y, int w, int h, uint32_t color) {
    int r = SCALE1(7);
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;

    // Main body between corner rows (full width)
    if (h - 2 * r > 0) {
        SDL_FillRect(screen, &(SDL_Rect){x, y + r, w, h - 2 * r}, color);
    }

    // Top and bottom corner rows with circular inset
    for (int dy = 0; dy < r; dy++) {
        int yd = r - dy;
        int inset = r - (int)sqrtf((float)(r * r - yd * yd));
        int row_w = w - 2 * inset;
        if (row_w <= 0) continue;
        // Top row
        SDL_FillRect(screen, &(SDL_Rect){x + inset, y + dy, row_w, 1}, color);
        // Bottom row (mirrored)
        SDL_FillRect(screen, &(SDL_Rect){x + inset, y + h - 1 - dy, row_w, 1}, color);
    }
}

// ============================================
// Generic Simple Menu Rendering
// ============================================

// Render a simple menu with optional customization callbacks
void render_simple_menu(SDL_Surface* screen, int show_setting, int menu_selected,
                        const SimpleMenuConfig* config) {
    GFX_clear(screen);
    char truncated[256];
    char label_buffer[256];

    render_screen_header(screen, config->title, show_setting);
    ListLayout layout = calc_list_layout(screen, 0);

    // Calculate icon size and spacing (scale 24px icons to fit in PILL_SIZE)
    int icon_size = SCALE1(24);
    int icon_spacing = SCALE1(6);

    for (int i = 0; i < config->item_count; i++) {
        bool selected = (i == menu_selected);

        // Get label (use callback if provided)
        const char* label = config->items[i];
        if (config->get_label) {
            const char* custom = config->get_label(i, label, label_buffer, sizeof(label_buffer));
            if (custom) label = custom;
        }

        // Check if we have an icon for this item
        SDL_Surface* icon = NULL;
        int icon_offset = 0;
        if (config->get_icon) {
            icon = config->get_icon(i, selected);
            if (icon) {
                icon_offset = icon_size + icon_spacing;
            }
        }

        // Render pill and text (account for icon width in pill calculation)
        MenuItemPos pos = render_menu_item_pill(screen, &layout, label, truncated, i, selected, icon_offset);

        // Render icon if present (scale to display size)
        int text_x = pos.text_x;
        if (icon) {
            int icon_y = pos.item_y + (SCALE1(PILL_SIZE) - icon_size) / 2;
            SDL_Rect src_rect = {0, 0, icon->w, icon->h};
            SDL_Rect dst_rect = {pos.text_x, icon_y, icon_size, icon_size};
            SDL_BlitScaled(icon, &src_rect, screen, &dst_rect);
            text_x += icon_offset;
        }

        // Render text after icon
        render_list_item_text(screen, NULL, truncated, Fonts_getLarge(),
                              text_x, pos.text_y, layout.max_width - icon_offset, selected);

        // Render badge if callback provided
        if (config->render_badge) {
            config->render_badge(screen, i, selected, pos.item_y, SCALE1(PILL_SIZE));
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"START", "CONTROLS", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", (char*)config->btn_b_label, "A", "OPEN", NULL}, 1, screen, 1);
}

// ============================================
// Toast Notification (GPU layer, highest z-index)
// ============================================

// Toast is rendered to GPU layer 5 (highest) to appear above all content
#define LAYER_TOAST 5

// Render toast notification to GPU layer (above all other content)
void render_toast(SDL_Surface* screen, const char* message, uint32_t toast_time) {
    if (!message || message[0] == '\0') {
        PLAT_clearLayers(LAYER_TOAST);
        return;
    }

    uint32_t now = SDL_GetTicks();
    if (now - toast_time >= TOAST_DURATION) {
        PLAT_clearLayers(LAYER_TOAST);
        return;
    }

    int hw = screen->w;
    int hh = screen->h;

    SDL_Surface* toast_text = TTF_RenderUTF8_Blended(Fonts_getMedium(), message, COLOR_WHITE);
    if (toast_text) {
        int border = SCALE1(2);
        int toast_w = toast_text->w + SCALE1(PADDING * 3);
        int toast_h = toast_text->h + SCALE1(12);
        int toast_x = (hw - toast_w) / 2;
        int toast_y = hh - SCALE1(BUTTON_SIZE + BUTTON_MARGIN + PADDING * 3) - toast_h;

        // Total surface size including border
        int surface_w = toast_w + border * 2;
        int surface_h = toast_h + border * 2;

        // Create surface for GPU layer rendering
        SDL_Surface* toast_surface = SDL_CreateRGBSurfaceWithFormat(0,
            surface_w, surface_h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (toast_surface) {
            // Disable blending so fills are opaque
            SDL_SetSurfaceBlendMode(toast_surface, SDL_BLENDMODE_NONE);

            // Draw light gray border (outer rect)
            SDL_FillRect(toast_surface, NULL, SDL_MapRGBA(toast_surface->format, 200, 200, 200, 255));

            // Draw dark grey background (inner rect)
            SDL_Rect bg_rect = {border, border, toast_w, toast_h};
            SDL_FillRect(toast_surface, &bg_rect, SDL_MapRGBA(toast_surface->format, 40, 40, 40, 255));

            // Draw text centered within the toast (blend text onto surface)
            SDL_SetSurfaceBlendMode(toast_surface, SDL_BLENDMODE_BLEND);
            int text_x = border + (toast_w - toast_text->w) / 2;
            int text_y = border + (toast_h - toast_text->h) / 2;
            SDL_BlitSurface(toast_text, NULL, toast_surface, &(SDL_Rect){text_x, text_y});

            // Render to GPU layer at the correct screen position
            PLAT_clearLayers(LAYER_TOAST);
            PLAT_drawOnLayer(toast_surface, toast_x - border, toast_y - border,
                            surface_w, surface_h, 1.0f, false, LAYER_TOAST);

            SDL_FreeSurface(toast_surface);
        }
        SDL_FreeSurface(toast_text);
    }
}

// Clear toast from GPU layer
void clear_toast(void) {
    PLAT_clearLayers(LAYER_TOAST);
}
