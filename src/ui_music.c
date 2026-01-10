#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_music.h"
#include "ui_fonts.h"
#include "ui_utils.h"
#include "ui_album_art.h"

// Scroll text state for browser list (selected item)
static ScrollTextState browser_scroll = {0};

// Scroll text state for player title
static ScrollTextState player_title_scroll;

// Render the file browser
void render_browser(SDL_Surface* screen, int show_setting, BrowserContext* browser) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    render_screen_header(screen, "Music Player", show_setting);

    // Use common list layout calculation
    ListLayout layout = calc_list_layout(screen, 0);
    browser->items_per_page = layout.items_per_page;

    adjust_list_scroll(browser->selected, &browser->scroll_offset, browser->items_per_page);

    for (int i = 0; i < browser->items_per_page && browser->scroll_offset + i < browser->entry_count; i++) {
        int idx = browser->scroll_offset + i;
        FileEntry* entry = &browser->entries[idx];
        bool selected = (idx == browser->selected);

        int y = layout.list_y + i * layout.item_h;

        // Icon or folder indicator
        char display[256];
        if (entry->is_dir) {
            snprintf(display, sizeof(display), "[%s]", entry->name);
        } else {
            Browser_getDisplayName(entry->name, display, sizeof(display));
        }

        // Render pill background and get text position
        ListItemPos pos = render_list_item_pill(screen, &layout, display, truncated, y, selected, 0);

        // Use common text rendering with scrolling for selected items
        render_list_item_text(screen, &browser_scroll, display, get_font_medium(),
                              pos.text_x, pos.text_y, pos.pill_width - SCALE1(BUTTON_PADDING * 2), selected);
    }

    render_scroll_indicators(screen, browser->scroll_offset, browser->items_per_page, browser->entry_count);

    // Empty folder message
    if (browser->entry_count == 0) {
        const char* msg = "No music files found";
        SDL_Surface* text = TTF_RenderUTF8_Blended(get_font_large(), msg, COLOR_GRAY);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, hh / 2 - text->h / 2});
            SDL_FreeSurface(text);
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"U/D", "SCROLL", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "SELECT", NULL}, 1, screen, 1);
}

// Render the now playing screen
void render_playing(SDL_Surface* screen, int show_setting, BrowserContext* browser,
                    bool shuffle_enabled, bool repeat_enabled) {
    GFX_clear(screen);

    // Render album art as triangular background (if available)
    SDL_Surface* album_art = Player_getAlbumArt();
    if (album_art && album_art->w > 0 && album_art->h > 0) {
        render_album_art_background(screen, album_art);
    }

    int hw = screen->w;
    int hh = screen->h;

    const TrackInfo* info = Player_getTrackInfo();
    PlayerState state = Player_getState();
    AudioFormat format = Player_detectFormat(Player_getCurrentFile());
    int duration = Player_getDuration();
    int position = Player_getPosition();
    float progress = (duration > 0) ? (float)position / duration : 0.0f;

    // === TOP BAR ===
    int top_y = SCALE1(PADDING);

    // Format badge "FLAC" with border (smaller, gray) - render first on the left
    const char* fmt_name = get_format_name(format);
    SDL_Surface* fmt_surf = TTF_RenderUTF8_Blended(get_font_tiny(), fmt_name, COLOR_GRAY);
    int badge_h = fmt_surf ? fmt_surf->h + SCALE1(4) : SCALE1(16);
    int badge_x = SCALE1(PADDING);
    int badge_w = 0;

    // Draw format badge on the left
    if (fmt_surf) {
        badge_w = fmt_surf->w + SCALE1(10);
        // Draw border (gray)
        SDL_Rect border = {badge_x, top_y, badge_w, badge_h};
        SDL_FillRect(screen, &border, RGB_GRAY);
        SDL_Rect inner = {badge_x + 1, top_y + 1, badge_w - 2, badge_h - 2};
        SDL_FillRect(screen, &inner, RGB_BLACK);
        SDL_BlitSurface(fmt_surf, NULL, screen, &(SDL_Rect){badge_x + SCALE1(5), top_y + SCALE1(2)});
        SDL_FreeSurface(fmt_surf);
    }

    // Track counter "01 - 03" (smaller, gray) - after the format badge
    int track_num = Browser_getCurrentTrackNumber(browser);
    int total_tracks = Browser_countAudioFiles(browser);
    char track_str[32];
    snprintf(track_str, sizeof(track_str), "%02d - %02d", track_num, total_tracks);
    SDL_Surface* track_surf = TTF_RenderUTF8_Blended(get_font_tiny(), track_str, COLOR_GRAY);
    if (track_surf) {
        int track_x = badge_x + badge_w + SCALE1(8);
        int track_y = top_y + (badge_h - track_surf->h) / 2;
        SDL_BlitSurface(track_surf, NULL, screen, &(SDL_Rect){track_x, track_y});
        SDL_FreeSurface(track_surf);
    }

    // Hardware status (clock, battery) on right
    GFX_blitHardwareGroup(screen, show_setting);

    // === TRACK INFO SECTION ===
    int info_y = SCALE1(PADDING + 45);
    char truncated[256];

    // Max width for text (album art is now only shown as background)
    int max_w_text = hw - SCALE1(PADDING * 2);

    // Artist name (Medium font, gray)
    const char* artist = info->artist[0] ? info->artist : "Unknown Artist";
    GFX_truncateText(get_font_artist(), artist, truncated, max_w_text, 0);
    SDL_Surface* artist_surf = TTF_RenderUTF8_Blended(get_font_artist(), truncated, COLOR_GRAY);
    if (artist_surf) {
        SDL_BlitSurface(artist_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
        info_y += artist_surf->h + SCALE1(2);  // Same gap as title-album
        SDL_FreeSurface(artist_surf);
    } else {
        info_y += SCALE1(18);
    }

    // Song title (Regular font extra large, white) - with scrolling animation
    const char* title = info->title[0] ? info->title : "Unknown Title";
    ScrollText_update(&player_title_scroll, title, get_font_title(), max_w_text,
                      COLOR_WHITE, screen, SCALE1(PADDING), info_y);
    info_y += TTF_FontHeight(get_font_title()) + SCALE1(2);  // Smaller gap after title

    // Album name (Bold font smaller, gray)
    const char* album = info->album[0] ? info->album : "";
    if (album[0]) {
        GFX_truncateText(get_font_album(), album, truncated, max_w_text, 0);
        SDL_Surface* album_surf = TTF_RenderUTF8_Blended(get_font_album(), truncated, COLOR_GRAY);
        if (album_surf) {
            SDL_BlitSurface(album_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
            SDL_FreeSurface(album_surf);
        }
    }

    // === WAVEFORM SECTION ===
    // Position waveform lower to avoid overlap
    int wave_y = hh - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN + 90);
    int wave_h = SCALE1(50);
    int wave_x = SCALE1(PADDING);
    int wave_w = hw - SCALE1(PADDING * 2);

    const WaveformData* waveform = Player_getWaveform();
    if (waveform && waveform->valid && waveform->bar_count > 0) {
        int total_bars = waveform->bar_count;
        float bar_width_f = (float)wave_w / total_bars;
        int bar_gap = 1;
        int bar_draw_w = (int)bar_width_f - bar_gap;
        if (bar_draw_w < 1) bar_draw_w = 1;

        int current_bar = (int)(progress * total_bars);
        if (current_bar >= total_bars) current_bar = total_bars - 1;

        // Draw waveform bars (centered vertically)
        for (int i = 0; i < total_bars; i++) {
            float amplitude = waveform->bars[i];
            int bar_h = (int)(amplitude * wave_h * 0.85f);
            if (bar_h < SCALE1(2)) bar_h = SCALE1(2);

            int bar_x_pos = wave_x + (int)(i * bar_width_f);
            int bar_y_pos = wave_y + (wave_h - bar_h) / 2;

            uint32_t color = (i <= current_bar) ? RGB_WHITE : RGB_DARK_GRAY;
            SDL_Rect bar_rect = {bar_x_pos, bar_y_pos, bar_draw_w, bar_h};
            SDL_FillRect(screen, &bar_rect, color);
        }
    } else {
        // Fallback progress bar (thin line when no waveform)
        SDL_Rect bg = {wave_x, wave_y + wave_h / 2 - SCALE1(1), wave_w, SCALE1(2)};
        SDL_FillRect(screen, &bg, RGB_DARK_GRAY);
        if (duration > 0) {
            int fill_w = (int)(progress * wave_w);
            if (fill_w > 0) {
                SDL_Rect fill = {wave_x, wave_y + wave_h / 2 - SCALE1(1), fill_w, SCALE1(2)};
                SDL_FillRect(screen, &fill, RGB_WHITE);
            }
        }
    }

    // === BOTTOM BAR ===
    int bottom_y = hh - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN + 35);

    // Current time (small font)
    char pos_str[16];
    format_time(pos_str, position);
    SDL_Surface* pos_surf = TTF_RenderUTF8_Blended(get_font_small(), pos_str, COLOR_WHITE);
    if (pos_surf) {
        SDL_BlitSurface(pos_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), bottom_y});

        // Total duration (tiny, next to current time)
        char dur_str[16];
        format_time(dur_str, duration);
        SDL_Surface* dur_surf = TTF_RenderUTF8_Blended(get_font_tiny(), dur_str, COLOR_GRAY);
        if (dur_surf) {
            SDL_BlitSurface(dur_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + pos_surf->w + SCALE1(6), bottom_y + pos_surf->h - dur_surf->h});
            SDL_FreeSurface(dur_surf);
        }
        SDL_FreeSurface(pos_surf);
    }

    // Shuffle and Repeat labels on right side
    int label_x = hw - SCALE1(PADDING);

    // Repeat label
    const char* repeat_text = "REPEAT";
    SDL_Color repeat_color = repeat_enabled ? COLOR_WHITE : COLOR_GRAY;
    SDL_Surface* repeat_surf = TTF_RenderUTF8_Blended(get_font_tiny(), repeat_text, repeat_color);
    if (repeat_surf) {
        label_x -= repeat_surf->w;
        SDL_BlitSurface(repeat_surf, NULL, screen, &(SDL_Rect){label_x, bottom_y});
        // Draw underline if enabled
        if (repeat_enabled) {
            SDL_Rect underline = {label_x, bottom_y + repeat_surf->h, repeat_surf->w, SCALE1(1)};
            SDL_FillRect(screen, &underline, RGB_WHITE);
        }
        SDL_FreeSurface(repeat_surf);
    }

    // Shuffle label (with gap before repeat)
    label_x -= SCALE1(12);
    const char* shuffle_text = "SHUFFLE";
    SDL_Color shuffle_color = shuffle_enabled ? COLOR_WHITE : COLOR_GRAY;
    SDL_Surface* shuffle_surf = TTF_RenderUTF8_Blended(get_font_tiny(), shuffle_text, shuffle_color);
    if (shuffle_surf) {
        label_x -= shuffle_surf->w;
        SDL_BlitSurface(shuffle_surf, NULL, screen, &(SDL_Rect){label_x, bottom_y});
        // Draw underline if enabled
        if (shuffle_enabled) {
            SDL_Rect underline = {label_x, bottom_y + shuffle_surf->h, shuffle_surf->w, SCALE1(1)};
            SDL_FillRect(screen, &underline, RGB_WHITE);
        }
        SDL_FreeSurface(shuffle_surf);
    }

    // === BUTTON HINTS ===
    GFX_blitButtonGroup((char*[]){"U/D", "PREV/NEXT", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", "A", state == PLAYER_STATE_PLAYING ? "PAUSE" : "PLAY", NULL}, 1, screen, 1);
}

// Check if browser list has active scrolling (for refresh optimization)
bool browser_needs_scroll_refresh(void) {
    return ScrollText_isScrolling(&browser_scroll);
}
