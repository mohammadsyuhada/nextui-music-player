#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_music.h"
#include "ui_fonts.h"
#include "ui_utils.h"
#include "ui_album_art.h"
#include "selfupdate.h"

// Menu items
static const char* menu_items[] = {"Local Files", "Internet Radio", "MP3 Downloader", "About"};
#define MENU_ITEM_COUNT 4

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

    // Title pill
    const char* title = "Music Player";
    int title_width = GFX_truncateText(get_font_medium(), title, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(get_font_medium(), truncated, COLOR_GRAY);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + SCALE1(BUTTON_PADDING), SCALE1(PADDING + 4)});
        SDL_FreeSurface(title_text);
    }

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    // File list
    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int list_h = hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE);
    browser->items_per_page = list_h / item_h;

    // Adjust scroll
    if (browser->selected < browser->scroll_offset) {
        browser->scroll_offset = browser->selected;
    }
    if (browser->selected >= browser->scroll_offset + browser->items_per_page) {
        browser->scroll_offset = browser->selected - browser->items_per_page + 1;
    }

    // Render items
    int max_width = hw - SCALE1(PADDING * 4);

    for (int i = 0; i < browser->items_per_page && browser->scroll_offset + i < browser->entry_count; i++) {
        int idx = browser->scroll_offset + i;
        FileEntry* entry = &browser->entries[idx];
        bool selected = (idx == browser->selected);

        int y = list_y + i * item_h;

        // Icon or folder indicator
        char display[256];
        if (entry->is_dir) {
            snprintf(display, sizeof(display), "[%s]", entry->name);
        } else {
            Browser_getDisplayName(entry->name, display, sizeof(display));
        }

        // Calculate text width for pill sizing
        char truncated[256];
        int pill_width = calc_list_pill_width(get_font_large(), display, truncated, max_width, 0);

        // Background pill (sized to text width)
        SDL_Rect pill_rect = {SCALE1(PADDING), y, pill_width, item_h};
        draw_list_item_bg(screen, &pill_rect, selected);

        SDL_Color text_color = get_list_text_color(selected);
        int text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
        int text_y = y + (item_h - TTF_FontHeight(get_font_large())) / 2;

        if (selected) {
            // Selected item: use scrolling text
            ScrollText_update(&browser_scroll, display, get_font_large(), pill_width - SCALE1(BUTTON_PADDING * 2),
                              text_color, screen, text_x, text_y);
        } else {
            // Non-selected items: static rendering with clipping
            SDL_Surface* text = TTF_RenderUTF8_Blended(get_font_large(), display, text_color);
            if (text) {
                SDL_Rect src = {0, 0, text->w > max_width ? max_width : text->w, text->h};
                SDL_BlitSurface(text, &src, screen, &(SDL_Rect){text_x, text_y, 0, 0});
                SDL_FreeSurface(text);
            }
        }
    }

    // Scroll indicators
    if (browser->entry_count > browser->items_per_page) {
        int ox = (hw - SCALE1(24)) / 2;
        if (browser->scroll_offset > 0) {
            GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE)});
        }
        if (browser->scroll_offset + browser->items_per_page < browser->entry_count) {
            GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE)});
        }
    }

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

// Render quit confirmation dialog overlay
void render_quit_confirm(SDL_Surface* screen) {
    int hw = screen->w;
    int hh = screen->h;

    // Dialog box (centered)
    int box_w = SCALE1(220);
    int box_h = SCALE1(90);
    int box_x = (hw - box_w) / 2;
    int box_y = (hh - box_h) / 2;

    // Dark background around dialog
    SDL_Rect top_area = {0, 0, hw, box_y};
    SDL_Rect bot_area = {0, box_y + box_h, hw, hh - box_y - box_h};
    SDL_Rect left_area = {0, box_y, box_x, box_h};
    SDL_Rect right_area = {box_x + box_w, box_y, hw - box_x - box_w, box_h};
    SDL_FillRect(screen, &top_area, RGB_BLACK);
    SDL_FillRect(screen, &bot_area, RGB_BLACK);
    SDL_FillRect(screen, &left_area, RGB_BLACK);
    SDL_FillRect(screen, &right_area, RGB_BLACK);

    // Box background
    SDL_Rect box = {box_x, box_y, box_w, box_h};
    SDL_FillRect(screen, &box, RGB_BLACK);

    // Box border
    SDL_Rect border_top = {box_x, box_y, box_w, SCALE1(2)};
    SDL_Rect border_bot = {box_x, box_y + box_h - SCALE1(2), box_w, SCALE1(2)};
    SDL_Rect border_left = {box_x, box_y, SCALE1(2), box_h};
    SDL_Rect border_right = {box_x + box_w - SCALE1(2), box_y, SCALE1(2), box_h};
    SDL_FillRect(screen, &border_top, RGB_WHITE);
    SDL_FillRect(screen, &border_bot, RGB_WHITE);
    SDL_FillRect(screen, &border_left, RGB_WHITE);
    SDL_FillRect(screen, &border_right, RGB_WHITE);

    // Message text
    const char* msg = "Quit Music Player?";
    SDL_Surface* msg_surf = TTF_RenderUTF8_Blended(get_font_medium(), msg, COLOR_WHITE);
    if (msg_surf) {
        SDL_BlitSurface(msg_surf, NULL, screen, &(SDL_Rect){(hw - msg_surf->w) / 2, box_y + SCALE1(20)});
        SDL_FreeSurface(msg_surf);
    }

    // Button hints
    const char* hint = "A: Yes   B: No";
    SDL_Surface* hint_surf = TTF_RenderUTF8_Blended(get_font_small(), hint, COLOR_GRAY);
    if (hint_surf) {
        SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){(hw - hint_surf->w) / 2, box_y + SCALE1(55)});
        SDL_FreeSurface(hint_surf);
    }
}

// Check if browser list has active scrolling (for refresh optimization)
bool browser_needs_scroll_refresh(void) {
    return ScrollText_isScrolling(&browser_scroll);
}

// Render the main menu
void render_menu(SDL_Surface* screen, int show_setting, int menu_selected) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    (void)hh;  // Unused in this function
    char truncated[256];

    // Title
    const char* title = "Music Player";
    int title_width = GFX_truncateText(get_font_medium(), title, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(get_font_medium(), truncated, COLOR_GRAY);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + SCALE1(BUTTON_PADDING), SCALE1(PADDING + 4)});
        SDL_FreeSurface(title_text);
    }

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    // Menu items
    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE + BUTTON_MARGIN);

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int y = list_y + i * item_h;
        bool selected = (i == menu_selected);

        // Dynamic label for About menu item (show update badge if available)
        const char* label = menu_items[i];
        char about_label[64];
        if (i == 3) {  // About menu item
            const SelfUpdateStatus* status = SelfUpdate_getStatus();
            if (status->update_available) {
                snprintf(about_label, sizeof(about_label), "About (Update Available)");
                label = about_label;
            }
        }

        // Calculate text width for pill sizing
        int max_width = hw - SCALE1(PADDING * 2);
        int pill_width = calc_list_pill_width(get_font_large(), label, truncated, max_width, 0);

        SDL_Rect pill_rect = {SCALE1(PADDING), y, pill_width, SCALE1(PILL_SIZE)};
        draw_list_item_bg(screen, &pill_rect, selected);

        SDL_Color text_color = get_list_text_color(selected);
        int text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
        SDL_Surface* text = TTF_RenderUTF8_Blended(get_font_large(), truncated, text_color);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){text_x, y + (SCALE1(PILL_SIZE) - text->h) / 2});
            SDL_FreeSurface(text);
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"U/D", "SELECT", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "EXIT", "A", "OPEN", NULL}, 1, screen, 1);
}
