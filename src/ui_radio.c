#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_radio.h"
#include "ui_fonts.h"
#include "ui_album_art.h"
#include "radio_album_art.h"
#include "radio_curated.h"

// Render the radio station list
void render_radio_list(SDL_Surface* screen, int show_setting,
                       int radio_selected, int* radio_scroll) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "Internet Radio";
    int title_width = GFX_truncateText(get_font_medium(), title, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(get_font_medium(), truncated, COLOR_GRAY);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + SCALE1(4), SCALE1(PADDING + 4)});
        SDL_FreeSurface(title_text);
    }

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    // Station list
    RadioStation* stations;
    int station_count = Radio_getStations(&stations);

    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int list_h = hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE);
    int items_per_page = list_h / item_h;

    // Adjust scroll
    if (radio_selected < *radio_scroll) {
        *radio_scroll = radio_selected;
    }
    if (radio_selected >= *radio_scroll + items_per_page) {
        *radio_scroll = radio_selected - items_per_page + 1;
    }

    for (int i = 0; i < items_per_page && *radio_scroll + i < station_count; i++) {
        int idx = *radio_scroll + i;
        RadioStation* station = &stations[idx];
        bool selected = (idx == radio_selected);

        int y = list_y + i * item_h;

        if (selected) {
            SDL_Rect pill_rect = {SCALE1(PADDING), y, hw - SCALE1(PADDING * 2), item_h};
            GFX_blitPill(ASSET_WHITE_PILL, screen, &pill_rect);
        }

        // Station name
        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        SDL_Surface* name_text = TTF_RenderUTF8_Blended(get_font_large(), station->name, text_color);
        if (name_text) {
            int max_width = hw - SCALE1(PADDING * 4);
            SDL_Rect src = {0, 0, name_text->w > max_width ? max_width : name_text->w, name_text->h};
            SDL_BlitSurface(name_text, &src, screen, &(SDL_Rect){SCALE1(PADDING * 2), y + (item_h - name_text->h) / 2});
            SDL_FreeSurface(name_text);
        }

        // Genre (if available)
        if (station->genre[0]) {
            SDL_Color genre_color = selected ? COLOR_GRAY : COLOR_DARK_TEXT;
            SDL_Surface* genre_text = TTF_RenderUTF8_Blended(get_font_tiny(), station->genre, genre_color);
            if (genre_text) {
                SDL_BlitSurface(genre_text, NULL, screen, &(SDL_Rect){hw - genre_text->w - SCALE1(PADDING * 2), y + (item_h - genre_text->h) / 2});
                SDL_FreeSurface(genre_text);
            }
        }
    }

    // Scroll indicators
    if (station_count > items_per_page) {
        int ox = (hw - SCALE1(24)) / 2;
        if (*radio_scroll > 0) {
            GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE)});
        }
        if (*radio_scroll + items_per_page < station_count) {
            GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE)});
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"Y", "MANAGE STATIONS", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "PLAY", NULL}, 1, screen, 1);
}

// Helper to get current station by index
static RadioStation* get_station_by_index(int index) {
    RadioStation* stations;
    int count = Radio_getStations(&stations);
    if (count > 0 && index < count) {
        return &stations[index];
    }
    return NULL;
}

// Render the radio playing screen
void render_radio_playing(SDL_Surface* screen, int show_setting, int radio_selected) {
    GFX_clear(screen);

    // Render album art as triangular background (if available and not being fetched)
    // Skip during fetch to avoid accessing potentially invalid surface
    if (!radio_album_art_is_fetching()) {
        SDL_Surface* album_art = Radio_getAlbumArt();
        if (album_art && album_art->w > 0 && album_art->h > 0) {
            render_album_art_background(screen, album_art);
        }
    }

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    RadioState state = Radio_getState();
    const RadioMetadata* meta = Radio_getMetadata();
    RadioStation* current_station = get_station_by_index(radio_selected);
    RadioStation* stations;
    int station_count = Radio_getStations(&stations);

    // === TOP BAR ===
    int top_y = SCALE1(PADDING);

    // "RADIO" badge with border (like format badge in local player)
    const char* badge_text = "RADIO";
    SDL_Surface* badge_surf = TTF_RenderUTF8_Blended(get_font_tiny(), badge_text, COLOR_GRAY);
    int badge_h = badge_surf ? badge_surf->h + SCALE1(4) : SCALE1(16);
    int badge_x = SCALE1(PADDING);
    int badge_w = 0;

    if (badge_surf) {
        badge_w = badge_surf->w + SCALE1(10);
        // Draw border (gray)
        SDL_Rect border = {badge_x, top_y, badge_w, badge_h};
        SDL_FillRect(screen, &border, RGB_GRAY);
        SDL_Rect inner = {badge_x + 1, top_y + 1, badge_w - 2, badge_h - 2};
        SDL_FillRect(screen, &inner, RGB_BLACK);
        SDL_BlitSurface(badge_surf, NULL, screen, &(SDL_Rect){badge_x + SCALE1(5), top_y + SCALE1(2)});
        SDL_FreeSurface(badge_surf);
    }

    // Station counter "01 - 12" (like track counter in local player)
    char station_str[32];
    snprintf(station_str, sizeof(station_str), "%02d - %02d", radio_selected + 1, station_count);
    SDL_Surface* station_surf = TTF_RenderUTF8_Blended(get_font_tiny(), station_str, COLOR_GRAY);
    if (station_surf) {
        int station_x = badge_x + badge_w + SCALE1(8);
        int station_y = top_y + (badge_h - station_surf->h) / 2;
        SDL_BlitSurface(station_surf, NULL, screen, &(SDL_Rect){station_x, station_y});
        SDL_FreeSurface(station_surf);
    }

    // Hardware status (clock, battery) on right
    GFX_blitHardwareGroup(screen, show_setting);

    // === STATION INFO SECTION ===
    int info_y = SCALE1(PADDING + 45);

    // Max widths for text (album art is now only shown as background)
    int max_w_half = (hw - SCALE1(PADDING * 2)) / 2;
    int max_w_full = hw - SCALE1(PADDING * 2);

    // Genre (like Artist in local player) - gray, medium font
    const char* genre = (current_station && current_station->genre[0]) ? current_station->genre : "Radio";
    GFX_truncateText(get_font_artist(), genre, truncated, max_w_half, 0);
    SDL_Surface* genre_surf = TTF_RenderUTF8_Blended(get_font_artist(), truncated, COLOR_GRAY);
    if (genre_surf) {
        SDL_BlitSurface(genre_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
        info_y += genre_surf->h + SCALE1(2);
        SDL_FreeSurface(genre_surf);
    } else {
        info_y += SCALE1(18);
    }

    // Station name (like Title in local player) - white, large font
    const char* station_name = meta->station_name[0] ? meta->station_name :
                               (current_station ? current_station->name : "Unknown Station");
    GFX_truncateText(get_font_title(), station_name, truncated, max_w_full, 0);
    SDL_Surface* name_surf = TTF_RenderUTF8_Blended(get_font_title(), truncated, COLOR_WHITE);
    if (name_surf) {
        SDL_BlitSurface(name_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
        info_y += name_surf->h + SCALE1(2);
        SDL_FreeSurface(name_surf);
    } else {
        info_y += SCALE1(32);
    }

    // Now Playing - Title on top (white, large), Artist below (gray, small)
    if (meta->title[0]) {
        // Title with text wrapping (max 3 lines)
        TTF_Font* title_font = get_font_artist();
        const char* src = meta->title;
        int max_lines = 3;
        int lines_rendered = 0;

        while (*src && lines_rendered < max_lines) {
            // Find how many characters fit on this line
            int text_len = strlen(src);
            int char_count = text_len;

            // Binary search for characters that fit
            while (char_count > 0) {
                char line_buf[256];
                int copy_len = (char_count < 255) ? char_count : 255;
                strncpy(line_buf, src, copy_len);
                line_buf[copy_len] = '\0';

                int w, h;
                TTF_SizeUTF8(title_font, line_buf, &w, &h);
                if (w <= max_w_full) break;
                char_count--;
            }

            if (char_count == 0) char_count = 1;  // At least one character

            // Try to break at a space if not last line and not at end
            if (lines_rendered < max_lines - 1 && char_count < text_len) {
                int last_space = -1;
                for (int i = char_count - 1; i > 0; i--) {
                    if (src[i] == ' ') {
                        last_space = i;
                        break;
                    }
                }
                if (last_space > 0) char_count = last_space + 1;
            }

            // Render this line
            char line_buf[256];
            size_t copy_len = (char_count > 0 && char_count < 255) ? (size_t)char_count : 255;
            memcpy(line_buf, src, copy_len);
            line_buf[copy_len] = '\0';

            // Trim trailing space
            while (copy_len > 0 && line_buf[copy_len - 1] == ' ') {
                line_buf[--copy_len] = '\0';
            }

            if (strlen(line_buf) > 0) {
                SDL_Surface* title_surf = TTF_RenderUTF8_Blended(title_font, line_buf, COLOR_WHITE);
                if (title_surf) {
                    SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
                    info_y += title_surf->h + SCALE1(2);
                    SDL_FreeSurface(title_surf);
                }
            }

            src += char_count;
            // Skip leading spaces on next line
            while (*src == ' ') src++;
            lines_rendered++;
        }
    }
    if (meta->artist[0]) {
        // Artist line (smaller font)
        GFX_truncateText(get_font_small(), meta->artist, truncated, max_w_full, 0);
        SDL_Surface* artist_surf = TTF_RenderUTF8_Blended(get_font_small(), truncated, COLOR_GRAY);
        if (artist_surf) {
            SDL_BlitSurface(artist_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
            info_y += artist_surf->h + SCALE1(2);
            SDL_FreeSurface(artist_surf);
        }
    }

    // Show slogan if no title/artist available
    if (!meta->title[0] && !meta->artist[0] && current_station && current_station->slogan[0]) {
        GFX_truncateText(get_font_album(), current_station->slogan, truncated, max_w_full, 0);
        SDL_Surface* slogan_surf = TTF_RenderUTF8_Blended(get_font_album(), truncated, COLOR_GRAY);
        if (slogan_surf) {
            SDL_BlitSurface(slogan_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
            info_y += slogan_surf->h + SCALE1(2);
            SDL_FreeSurface(slogan_surf);
        }
    }

    // Position for error message (was visualization area)
    int vis_y = hh - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN + 90);

    // === BOTTOM BAR ===
    int bottom_y = hh - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN + 35);

    // Bitrate on left (like time in local player) - white
    int bitrate_end_x = SCALE1(PADDING);
    int bitrate_h = 0;
    if (meta->bitrate > 0) {
        char bitrate_str[32];
        snprintf(bitrate_str, sizeof(bitrate_str), "%d kbps", meta->bitrate);
        SDL_Surface* br_surf = TTF_RenderUTF8_Blended(get_font_small(), bitrate_str, COLOR_WHITE);
        if (br_surf) {
            SDL_BlitSurface(br_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), bottom_y});
            bitrate_end_x = SCALE1(PADDING) + br_surf->w + SCALE1(6);
            bitrate_h = br_surf->h;
            SDL_FreeSurface(br_surf);
        }
    }

    // Status text (tiny, gray) next to bitrate
    const char* status_text = "";
    switch (state) {
        case RADIO_STATE_CONNECTING: status_text = "connecting"; break;
        case RADIO_STATE_BUFFERING: status_text = "buffering"; break;
        case RADIO_STATE_PLAYING: status_text = "streaming"; break;
        case RADIO_STATE_ERROR: status_text = "error"; break;
        default: break;
    }
    if (status_text[0]) {
        SDL_Surface* status_surf = TTF_RenderUTF8_Blended(get_font_tiny(), status_text, COLOR_GRAY);
        if (status_surf) {
            int status_y = bottom_y + bitrate_h - status_surf->h;  // Align baselines
            SDL_BlitSurface(status_surf, NULL, screen, &(SDL_Rect){bitrate_end_x, status_y});
            SDL_FreeSurface(status_surf);
        }
    }

    // Buffer level indicator on right (instead of SHUFFLE/REPEAT)
    float buffer_level = Radio_getBufferLevel();
    int bar_w = SCALE1(60);
    int bar_h = SCALE1(8);
    int bar_x = hw - SCALE1(PADDING) - bar_w;
    int bar_y = bottom_y + SCALE1(4);

    // Draw buffer bar background
    SDL_Rect bar_bg = {bar_x, bar_y, bar_w, bar_h};
    SDL_FillRect(screen, &bar_bg, RGB_DARK_GRAY);

    // Draw buffer fill
    int fill_w = (int)(bar_w * buffer_level);
    if (fill_w > 0) {
        SDL_Rect bar_fill = {bar_x, bar_y, fill_w, bar_h};
        SDL_FillRect(screen, &bar_fill, RGB_WHITE);
    }

    // Error message (displayed prominently if in error state)
    if (state == RADIO_STATE_ERROR) {
        SDL_Surface* err_text = TTF_RenderUTF8_Blended(get_font_small(), Radio_getError(), (SDL_Color){255, 100, 100, 255});
        if (err_text) {
            SDL_BlitSurface(err_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING), vis_y - SCALE1(20)});
            SDL_FreeSurface(err_text);
        }
    }

    // === BUTTON HINTS ===
    GFX_blitButtonGroup((char*[]){"U/D", "PREV/NEXT", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "STOP", NULL}, 1, screen, 1);
}

// Render add stations - country selection screen
void render_radio_add(SDL_Surface* screen, int show_setting,
                      int add_country_selected, int* add_country_scroll) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "Add Stations";
    int title_width = GFX_truncateText(get_font_medium(), title, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(get_font_medium(), truncated, COLOR_GRAY);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + SCALE1(4), SCALE1(PADDING + 4)});
        SDL_FreeSurface(title_text);
    }

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    // Subtitle
    const char* subtitle = "Select Country";
    SDL_Surface* sub_text = TTF_RenderUTF8_Blended(get_font_small(), subtitle, COLOR_GRAY);
    if (sub_text) {
        SDL_BlitSurface(sub_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING + PILL_SIZE + 4)});
        SDL_FreeSurface(sub_text);
    }

    // Country list
    int country_count = Radio_getCuratedCountryCount();
    const CuratedCountry* countries = Radio_getCuratedCountries();

    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + 20);
    int list_h = hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE);
    int items_per_page = list_h / item_h;

    // Adjust scroll
    if (add_country_selected < *add_country_scroll) {
        *add_country_scroll = add_country_selected;
    }
    if (add_country_selected >= *add_country_scroll + items_per_page) {
        *add_country_scroll = add_country_selected - items_per_page + 1;
    }

    for (int i = 0; i < items_per_page && *add_country_scroll + i < country_count; i++) {
        int idx = *add_country_scroll + i;
        const CuratedCountry* country = &countries[idx];
        bool selected = (idx == add_country_selected);

        int y = list_y + i * item_h;

        if (selected) {
            SDL_Rect pill_rect = {SCALE1(PADDING), y, hw - SCALE1(PADDING * 2), item_h};
            GFX_blitPill(ASSET_WHITE_PILL, screen, &pill_rect);
        }

        // Country name
        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        SDL_Surface* name_text = TTF_RenderUTF8_Blended(get_font_large(), country->name, text_color);
        if (name_text) {
            SDL_BlitSurface(name_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING * 2), y + (item_h - name_text->h) / 2});
            SDL_FreeSurface(name_text);
        }

        // Station count on right
        int station_count = Radio_getCuratedStationCount(country->code);
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "%d stations", station_count);
        SDL_Color count_color = selected ? COLOR_GRAY : COLOR_DARK_TEXT;
        SDL_Surface* count_text = TTF_RenderUTF8_Blended(get_font_tiny(), count_str, count_color);
        if (count_text) {
            SDL_BlitSurface(count_text, NULL, screen, &(SDL_Rect){hw - count_text->w - SCALE1(PADDING * 2), y + (item_h - count_text->h) / 2});
            SDL_FreeSurface(count_text);
        }
    }

    // Scroll indicators
    if (country_count > items_per_page) {
        int ox = (hw - SCALE1(24)) / 2;
        if (*add_country_scroll > 0) {
            GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE)});
        }
        if (*add_country_scroll + items_per_page < country_count) {
            GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE)});
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"Y", "HELP", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"A", "SELECT", "B", "BACK", NULL}, 1, screen, 1);
}

// Render add stations - station selection screen
void render_radio_add_stations(SDL_Surface* screen, int show_setting,
                               const char* country_code,
                               int add_station_selected, int* add_station_scroll,
                               const bool* add_station_checked) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Get country name for title
    const char* country_name = "Stations";
    const CuratedCountry* countries = Radio_getCuratedCountries();
    int country_count = Radio_getCuratedCountryCount();
    for (int i = 0; i < country_count; i++) {
        if (strcmp(countries[i].code, country_code) == 0) {
            country_name = countries[i].name;
            break;
        }
    }

    // Title
    int title_width = GFX_truncateText(get_font_medium(), country_name, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(get_font_medium(), truncated, COLOR_GRAY);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + SCALE1(4), SCALE1(PADDING + 4)});
        SDL_FreeSurface(title_text);
    }

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    // Get stations for selected country
    int station_count = 0;
    const CuratedStation* stations = Radio_getCuratedStations(country_code, &station_count);

    // Count selected stations
    int selected_count = 0;
    for (int i = 0; i < station_count && i < 256; i++) {
        if (add_station_checked[i]) selected_count++;
    }

    // Subtitle with selection count
    char subtitle[64];
    snprintf(subtitle, sizeof(subtitle), "%d selected", selected_count);
    SDL_Surface* sub_text = TTF_RenderUTF8_Blended(get_font_small(), subtitle, COLOR_GRAY);
    if (sub_text) {
        SDL_BlitSurface(sub_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING + PILL_SIZE + 4)});
        SDL_FreeSurface(sub_text);
    }

    // Station list
    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + 20);
    int list_h = hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE);
    int items_per_page = list_h / item_h;

    // Adjust scroll
    if (add_station_selected < *add_station_scroll) {
        *add_station_scroll = add_station_selected;
    }
    if (add_station_selected >= *add_station_scroll + items_per_page) {
        *add_station_scroll = add_station_selected - items_per_page + 1;
    }

    for (int i = 0; i < items_per_page && *add_station_scroll + i < station_count; i++) {
        int idx = *add_station_scroll + i;
        const CuratedStation* station = &stations[idx];
        bool selected = (idx == add_station_selected);
        bool checked = (idx < 256) ? add_station_checked[idx] : false;

        int y = list_y + i * item_h;

        if (selected) {
            SDL_Rect pill_rect = {SCALE1(PADDING), y, hw - SCALE1(PADDING * 2), item_h};
            GFX_blitPill(ASSET_WHITE_PILL, screen, &pill_rect);
        }

        // Checkbox indicator
        const char* checkbox = checked ? "[x]" : "[ ]";
        SDL_Color cb_color = selected ? COLOR_BLACK : COLOR_WHITE;
        SDL_Surface* cb_text = TTF_RenderUTF8_Blended(get_font_small(), checkbox, cb_color);
        int cb_width = 0;
        if (cb_text) {
            SDL_BlitSurface(cb_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING * 2), y + (item_h - cb_text->h) / 2});
            cb_width = cb_text->w + SCALE1(6);
            SDL_FreeSurface(cb_text);
        }

        // Station name
        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        SDL_Surface* name_text = TTF_RenderUTF8_Blended(get_font_large(), station->name, text_color);
        if (name_text) {
            int max_width = hw - SCALE1(PADDING * 4) - cb_width - SCALE1(60);
            SDL_Rect src = {0, 0, name_text->w > max_width ? max_width : name_text->w, name_text->h};
            SDL_BlitSurface(name_text, &src, screen, &(SDL_Rect){SCALE1(PADDING * 2) + cb_width, y + (item_h - name_text->h) / 2});
            SDL_FreeSurface(name_text);
        }

        // Genre on right
        if (station->genre[0]) {
            SDL_Color genre_color = selected ? COLOR_GRAY : COLOR_DARK_TEXT;
            SDL_Surface* genre_text = TTF_RenderUTF8_Blended(get_font_tiny(), station->genre, genre_color);
            if (genre_text) {
                SDL_BlitSurface(genre_text, NULL, screen, &(SDL_Rect){hw - genre_text->w - SCALE1(PADDING * 2), y + (item_h - genre_text->h) / 2});
                SDL_FreeSurface(genre_text);
            }
        }
    }

    // Scroll indicators
    if (station_count > items_per_page) {
        int ox = (hw - SCALE1(24)) / 2;
        if (*add_station_scroll > 0) {
            GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE)});
        }
        if (*add_station_scroll + items_per_page < station_count) {
            GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE)});
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"X", "SAVE", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"A", "TOGGLE", "B", "BACK", NULL}, 1, screen, 1);
}

// Render help/instructions screen
void render_radio_help(SDL_Surface* screen, int show_setting, int* help_scroll) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "How to Add Stations";
    int title_width = GFX_truncateText(get_font_medium(), title, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(get_font_medium(), truncated, COLOR_GRAY);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + SCALE1(4), SCALE1(PADDING + 4)});
        SDL_FreeSurface(title_text);
    }

    // Hardware status (WiFi, battery)
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    // Instructions text
    int content_start_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN + 10);
    int line_h = SCALE1(18);
    int button_area_h = SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int visible_height = hh - content_start_y - button_area_h;

    const char* lines[] = {
        "To add custom radio stations:",
        "",
        "1. Create or edit the file:",
        "   /.userdata/shared/radio_stations.txt",
        "",
        "2. Add one station per line:",
        "   Name|URL|Genre|Slogan",
        "",
        "Example:",
        "   My Radio|http://example.com/stream|Music|Your Slogan",
        "",
        "Notes:",
        "- MP3, AAC, and M3U8 formats supported",
        "- Maximum 32 stations",
        "- Slogan is optional (shown when no song info)",
        "",
        "Find more stations at: fmstream.org"
    };

    int num_lines = sizeof(lines) / sizeof(lines[0]);

    // Calculate total content height
    int total_content_h = 0;
    for (int i = 0; i < num_lines; i++) {
        if (lines[i][0] == '\0') {
            total_content_h += line_h / 2;
        } else {
            total_content_h += line_h;
        }
    }

    // Calculate max scroll
    int max_scroll = total_content_h - visible_height;
    if (max_scroll < 0) max_scroll = 0;
    if (*help_scroll > max_scroll) *help_scroll = max_scroll;
    if (*help_scroll < 0) *help_scroll = 0;

    // Render lines with scroll offset
    int text_y = content_start_y - *help_scroll;
    for (int i = 0; i < num_lines; i++) {
        int current_line_h = (lines[i][0] == '\0') ? line_h / 2 : line_h;

        // Skip lines that are above visible area
        if (text_y + current_line_h < content_start_y) {
            text_y += current_line_h;
            continue;
        }

        // Stop if we're below visible area
        if (text_y >= hh - button_area_h) {
            break;
        }

        if (lines[i][0] == '\0') {
            text_y += line_h / 2;
            continue;
        }

        SDL_Color color = COLOR_WHITE;
        TTF_Font* use_font = get_font_small();

        // Highlight special lines
        if (strstr(lines[i], "Example:") || strstr(lines[i], "Notes:")) {
            color = COLOR_GRAY;
        } else if (lines[i][0] == '-') {
            color = COLOR_GRAY;
            use_font = get_font_tiny();
        }

        SDL_Surface* line_text = TTF_RenderUTF8_Blended(use_font, lines[i], color);
        if (line_text) {
            SDL_BlitSurface(line_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING), text_y});
            SDL_FreeSurface(line_text);
        }
        text_y += line_h;
    }

    // Scroll indicators
    if (max_scroll > 0) {
        int ox = (hw - SCALE1(24)) / 2;
        if (*help_scroll > 0) {
            GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, content_start_y - SCALE1(12)});
        }
        if (*help_scroll < max_scroll) {
            GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - button_area_h - SCALE1(16)});
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
}
