#include <stdio.h>
#include <string.h>

#include <SDL2/SDL_image.h>

#include "defines.h"
#include "api.h"
#include "ui_system.h"
#include "ui_fonts.h"
#include "selfupdate.h"
#include "qr_code_data.h"

// Render the app update screen
void render_app_updating(SDL_Surface* screen, int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "App Update";
    int title_width = GFX_truncateText(font.medium, title, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.medium, truncated, COLOR_GRAY);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + SCALE1(4), SCALE1(PADDING + 4)});
        SDL_FreeSurface(title_text);
    }

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    const SelfUpdateStatus* status = SelfUpdate_getStatus();
    SelfUpdateState state = status->state;

    // Version info: "v0.1.0 → v0.2.0"
    char ver_str[128];
    if (strlen(status->latest_version) > 0) {
        // Strip 'v' prefix if present for consistent formatting
        const char* curr = status->current_version;
        const char* latest = status->latest_version;
        if (curr[0] == 'v' || curr[0] == 'V') curr++;
        if (latest[0] == 'v' || latest[0] == 'V') latest++;
        snprintf(ver_str, sizeof(ver_str), "v%s  ->  v%s", curr, latest);
    } else {
        const char* curr = status->current_version;
        if (curr[0] == 'v' || curr[0] == 'V') curr++;
        snprintf(ver_str, sizeof(ver_str), "v%s", curr);
    }
    int ver_y = SCALE1(PADDING * 3 + 35);
    SDL_Surface* ver_text = TTF_RenderUTF8_Blended(font.medium, ver_str, COLOR_GRAY);
    if (ver_text) {
        SDL_BlitSurface(ver_text, NULL, screen, &(SDL_Rect){(hw - ver_text->w) / 2, ver_y});
        SDL_FreeSurface(ver_text);
    }

    // Release notes area with word wrapping (positioned right below version info)
    int notes_y = ver_y + SCALE1(30);
    int notes_max_lines = 5;
    int line_height = SCALE1(18);
    int max_line_width = hw - SCALE1(PADDING * 6);

    if (strlen(status->release_notes) > 0 && state != SELFUPDATE_STATE_CHECKING) {
        // Word-wrap release notes
        char notes_copy[1024];
        strncpy(notes_copy, status->release_notes, sizeof(notes_copy) - 1);
        notes_copy[sizeof(notes_copy) - 1] = '\0';

        // Replace newlines with spaces for continuous wrapping
        for (int i = 0; notes_copy[i]; i++) {
            if (notes_copy[i] == '\n' || notes_copy[i] == '\r') notes_copy[i] = ' ';
        }

        char wrapped_lines[5][128];
        int line_count = 0;
        char* src = notes_copy;

        while (*src && line_count < notes_max_lines) {
            // Skip leading spaces
            while (*src == ' ') src++;
            if (!*src) break;

            // Find how many characters fit in max_line_width
            char test_line[128] = "";
            int char_count = 0;
            int last_space = -1;

            while (src[char_count] && char_count < 127) {
                test_line[char_count] = src[char_count];
                test_line[char_count + 1] = '\0';

                if (src[char_count] == ' ') last_space = char_count;

                // Check width
                int text_w, text_h;
                TTF_SizeUTF8(get_font_small(), test_line, &text_w, &text_h);
                if (text_w > max_line_width) {
                    // Line too long, break at last space or current position
                    if (last_space > 0) {
                        char_count = last_space;
                    }
                    break;
                }
                char_count++;
            }

            // Copy the line
            strncpy(wrapped_lines[line_count], src, char_count);
            wrapped_lines[line_count][char_count] = '\0';
            src += char_count;
            line_count++;
        }

        // Render wrapped lines
        for (int i = 0; i < line_count; i++) {
            if (strlen(wrapped_lines[i]) > 0) {
                SDL_Surface* line_text = TTF_RenderUTF8_Blended(get_font_small(), wrapped_lines[i], COLOR_WHITE);
                if (line_text) {
                    SDL_BlitSurface(line_text, NULL, screen, &(SDL_Rect){(hw - line_text->w) / 2, notes_y + i * line_height});
                    SDL_FreeSurface(line_text);
                }
            }
        }
    } else if (state == SELFUPDATE_STATE_CHECKING) {
        // Show checking message
        SDL_Surface* check_text = TTF_RenderUTF8_Blended(font.small, "Checking for updates...", COLOR_GRAY);
        if (check_text) {
            SDL_BlitSurface(check_text, NULL, screen, &(SDL_Rect){(hw - check_text->w) / 2, notes_y});
            SDL_FreeSurface(check_text);
        }
    }

    // Progress bar (only during active update) - positioned above status message
    if (state == SELFUPDATE_STATE_DOWNLOADING || state == SELFUPDATE_STATE_EXTRACTING ||
        state == SELFUPDATE_STATE_APPLYING) {
        int bar_w = hw - SCALE1(PADDING * 8);
        int bar_h = SCALE1(8);
        int bar_x = SCALE1(PADDING * 4);
        int bar_y = hh - SCALE1(PILL_SIZE + PADDING * 7);

        // Background
        SDL_Rect bg_rect = {bar_x, bar_y, bar_w, bar_h};
        SDL_FillRect(screen, &bg_rect, SDL_MapRGB(screen->format, 64, 64, 64));

        // Progress
        int prog_w = (bar_w * status->progress_percent) / 100;
        SDL_Rect prog_rect = {bar_x, bar_y, prog_w, bar_h};
        SDL_FillRect(screen, &prog_rect, SDL_MapRGB(screen->format, 255, 255, 255));
    }

    // Status message during active operations - positioned below progress bar
    if (state == SELFUPDATE_STATE_DOWNLOADING || state == SELFUPDATE_STATE_EXTRACTING ||
        state == SELFUPDATE_STATE_APPLYING || state == SELFUPDATE_STATE_COMPLETED ||
        state == SELFUPDATE_STATE_ERROR) {

        const char* status_msg = status->status_message;
        if (state == SELFUPDATE_STATE_ERROR && strlen(status->error_message) > 0) {
            status_msg = status->error_message;
        }

        SDL_Color status_color = COLOR_WHITE;
        if (state == SELFUPDATE_STATE_ERROR) {
            status_color = (SDL_Color){255, 100, 100, 255};
        } else if (state == SELFUPDATE_STATE_COMPLETED) {
            status_color = (SDL_Color){100, 255, 100, 255};
        }

        SDL_Surface* status_text = TTF_RenderUTF8_Blended(font.small, status_msg, status_color);
        if (status_text) {
            SDL_BlitSurface(status_text, NULL, screen, &(SDL_Rect){(hw - status_text->w) / 2, hh - SCALE1(PILL_SIZE + PADDING * 4)});
            SDL_FreeSurface(status_text);
        }
    }

    // Button hints
    if (state == SELFUPDATE_STATE_COMPLETED) {
        GFX_blitButtonGroup((char*[]){"A", "RESTART", NULL}, 1, screen, 1);
    } else if (state == SELFUPDATE_STATE_DOWNLOADING) {
        GFX_blitButtonGroup((char*[]){"B", "CANCEL", NULL}, 1, screen, 1);
    } else {
        GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
    }
}

// Render the about screen
void render_about(SDL_Surface* screen, int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "About";
    int title_width = GFX_truncateText(font.medium, title, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.medium, truncated, COLOR_GRAY);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING) + SCALE1(4), SCALE1(PADDING + 4)});
        SDL_FreeSurface(title_text);
    }

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    // App name with version
    const char* version = SelfUpdate_getVersion();
    const char* ver = version;
    if (ver[0] == 'v' || ver[0] == 'V') ver++;
    char app_name[128];
    snprintf(app_name, sizeof(app_name), "Music Player (v%s)", ver);
    SDL_Surface* name_text = TTF_RenderUTF8_Blended(font.large, app_name, COLOR_WHITE);
    if (name_text) {
        SDL_BlitSurface(name_text, NULL, screen, &(SDL_Rect){(hw - name_text->w) / 2, SCALE1(PADDING * 3 + PILL_SIZE)});
        SDL_FreeSurface(name_text);
    }

    // Tagline (2 lines) - directly below app title
    int info_y = SCALE1(PADDING * 3 + PILL_SIZE + 30);
    const char* tagline1 = "Your favorite tunes on the go,";
    const char* tagline2 = "powered by your gaming handheld.";
    SDL_Surface* tagline_text1 = TTF_RenderUTF8_Blended(get_font_small(), tagline1, COLOR_WHITE);
    if (tagline_text1) {
        SDL_BlitSurface(tagline_text1, NULL, screen, &(SDL_Rect){(hw - tagline_text1->w) / 2, info_y});
        SDL_FreeSurface(tagline_text1);
    }
    SDL_Surface* tagline_text2 = TTF_RenderUTF8_Blended(get_font_small(), tagline2, COLOR_WHITE);
    if (tagline_text2) {
        SDL_BlitSurface(tagline_text2, NULL, screen, &(SDL_Rect){(hw - tagline_text2->w) / 2, info_y + SCALE1(18)});
        SDL_FreeSurface(tagline_text2);
    }

    // Show update available message if there's an update (directly under tagline)
    const SelfUpdateStatus* status = SelfUpdate_getStatus();
    if (status->update_available) {
        char update_msg[128];
        snprintf(update_msg, sizeof(update_msg), "Update available: v%s", status->latest_version);
        SDL_Surface* update_text = TTF_RenderUTF8_Blended(get_font_small(), update_msg, (SDL_Color){100, 255, 100, 255});
        if (update_text) {
            SDL_BlitSurface(update_text, NULL, screen, &(SDL_Rect){(hw - update_text->w) / 2, info_y + SCALE1(36)});
            SDL_FreeSurface(update_text);
        }
    }

    // GitHub QR Code
    SDL_RWops* rw = SDL_RWFromConstMem(qr_code_png, qr_code_png_len);
    if (rw) {
        SDL_Surface* qr_surface = IMG_Load_RW(rw, 1);
        if (qr_surface) {
            // Scale QR code to fit nicely (target ~150x150 pixels)
            int qr_size = SCALE1(75);
            SDL_Rect src_rect = {0, 0, qr_surface->w, qr_surface->h};
            SDL_Rect dst_rect = {(hw - qr_size) / 2, hh - SCALE1(PILL_SIZE + PADDING * 2) - qr_size, qr_size, qr_size};
            SDL_BlitScaled(qr_surface, &src_rect, screen, &dst_rect);
            SDL_FreeSurface(qr_surface);
        }
    }

    // Button hints - show UPDATE button if update available
    if (status->update_available) {
        GFX_blitButtonGroup((char*[]){"A", "UPDATE", "B", "BACK", NULL}, 1, screen, 1);
    } else {
        GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
    }
}
