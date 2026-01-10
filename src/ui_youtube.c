#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_youtube.h"
#include "ui_fonts.h"
#include "ui_utils.h"

// Scroll text state for YouTube results (selected item)
static ScrollTextState youtube_results_scroll_text = {0};

// Scroll text state for YouTube download queue (selected item)
static ScrollTextState youtube_queue_scroll_text = {0};

// YouTube sub-menu items
static const char* youtube_menu_items[] = {"Search Music", "Download Queue", "Update yt-dlp"};
#define YOUTUBE_MENU_COUNT 3

// Toast duration constant
#define YOUTUBE_TOAST_DURATION 1500  // 1.5 seconds

// Render YouTube sub-menu
void render_youtube_menu(SDL_Surface* screen, int show_setting, int menu_selected) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    (void)hh;
    char truncated[256];

    // Title
    const char* title = "MP3 Downloader";
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

    for (int i = 0; i < YOUTUBE_MENU_COUNT; i++) {
        int y = list_y + i * item_h;
        bool selected = (i == menu_selected);

        // Calculate text width for pill sizing
        int max_width = hw - SCALE1(PADDING * 2);
        int pill_width = calc_list_pill_width(get_font_large(), youtube_menu_items[i], truncated, max_width, 0);

        SDL_Rect pill_rect = {SCALE1(PADDING), y, pill_width, SCALE1(PILL_SIZE)};
        draw_list_item_bg(screen, &pill_rect, selected);

        SDL_Color text_color = get_list_text_color(selected);
        int text_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
        SDL_Surface* text = TTF_RenderUTF8_Blended(get_font_large(), truncated, text_color);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){text_x, y + (SCALE1(PILL_SIZE) - text->h) / 2});
            SDL_FreeSurface(text);
        }

        // Show queue count next to Download Queue
        if (i == 1) {
            int qcount = YouTube_queueCount();
            if (qcount > 0) {
                char count_str[16];
                snprintf(count_str, sizeof(count_str), "(%d)", qcount);
                SDL_Surface* count_text = TTF_RenderUTF8_Blended(get_font_small(), count_str, selected ? uintToColour(THEME_COLOR5_255) : COLOR_GRAY);
                if (count_text) {
                    SDL_BlitSurface(count_text, NULL, screen, &(SDL_Rect){hw - count_text->w - SCALE1(PADDING * 2), y + (SCALE1(PILL_SIZE) - count_text->h) / 2});
                    SDL_FreeSurface(count_text);
                }
            }
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"U/D", "SELECT", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){ "B", "BACK", "A", "OPEN", NULL}, 1, screen, 1);
}

// Render YouTube searching status
void render_youtube_searching(SDL_Surface* screen, int show_setting, const char* search_query) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "Searching...";
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

    // Searching message
    char search_msg[300];
    snprintf(search_msg, sizeof(search_msg), "Searching for: %s", search_query);
    SDL_Surface* query_text = TTF_RenderUTF8_Blended(get_font_medium(), search_msg, COLOR_GRAY);
    if (query_text) {
        int qx = (hw - query_text->w) / 2;
        if (qx < SCALE1(PADDING)) qx = SCALE1(PADDING);
        SDL_BlitSurface(query_text, NULL, screen, &(SDL_Rect){qx, hh / 2 - SCALE1(30)});
        SDL_FreeSurface(query_text);
    }

    // Loading indicator
    const char* loading = "Please wait...";
    SDL_Surface* load_text = TTF_RenderUTF8_Blended(get_font_medium(), loading, COLOR_WHITE);
    if (load_text) {
        SDL_BlitSurface(load_text, NULL, screen, &(SDL_Rect){(hw - load_text->w) / 2, hh / 2 + SCALE1(10)});
        SDL_FreeSurface(load_text);
    }

    // No button hints during search - it's blocking
}

// Render YouTube search results
void render_youtube_results(SDL_Surface* screen, int show_setting,
                            const char* search_query,
                            YouTubeResult* results, int result_count,
                            int selected, int* scroll,
                            char* toast_message, uint32_t toast_time, bool searching) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    char title[128];
    snprintf(title, sizeof(title), "Results: %s", search_query);
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

    // Results list
    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int list_h = hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE);
    int items_per_page = list_h / item_h;

    // Adjust scroll (only if there's a selection)
    if (selected >= 0) {
        if (selected < *scroll) {
            *scroll = selected;
        }
        if (selected >= *scroll + items_per_page) {
            *scroll = selected - items_per_page + 1;
        }
    }

    // Reserve space for duration on the right (format: "99:59" max)
    int dur_w, dur_h;
    TTF_SizeUTF8(get_font_tiny(), "99:59", &dur_w, &dur_h);
    int duration_reserved = dur_w + SCALE1(PADDING * 2);  // Duration width + gap
    int max_width = hw - SCALE1(PADDING * 2) - duration_reserved;

    for (int i = 0; i < items_per_page && *scroll + i < result_count; i++) {
        int idx = *scroll + i;
        YouTubeResult* result = &results[idx];
        bool is_selected = (idx == selected);
        bool in_queue = YouTube_isInQueue(result->video_id);

        int y = list_y + i * item_h;

        // Calculate indicator width if in queue
        int indicator_width = 0;
        if (in_queue) {
            int ind_w, ind_h;
            TTF_SizeUTF8(get_font_tiny(), "[+]", &ind_w, &ind_h);
            indicator_width = ind_w + SCALE1(4);
        }

        // Calculate text width for pill sizing
        char truncated[256];
        int pill_width = calc_list_pill_width(get_font_large(), result->title, truncated, max_width, indicator_width);

        // Background pill (sized to text width)
        SDL_Rect pill_rect = {SCALE1(PADDING), y, pill_width, item_h};
        draw_list_item_bg(screen, &pill_rect, is_selected);

        int title_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
        int text_y = y + (item_h - TTF_FontHeight(get_font_large())) / 2;

        // Show indicator if already in queue
        if (in_queue) {
            SDL_Surface* indicator = TTF_RenderUTF8_Blended(get_font_tiny(), "[+]", is_selected ? uintToColour(THEME_COLOR5_255) : COLOR_GRAY);
            if (indicator) {
                SDL_BlitSurface(indicator, NULL, screen, &(SDL_Rect){title_x, y + (item_h - indicator->h) / 2});
                title_x += indicator->w + SCALE1(4);
                SDL_FreeSurface(indicator);
            }
        }

        // Title
        SDL_Color text_color = get_list_text_color(is_selected);
        int title_max_w = pill_width - SCALE1(BUTTON_PADDING * 2) - indicator_width;

        if (is_selected) {
            // Selected item: use scrolling text
            ScrollText_update(&youtube_results_scroll_text, result->title, get_font_large(), title_max_w,
                              text_color, screen, title_x, text_y);
        } else {
            // Non-selected items: static rendering with clipping
            SDL_Surface* text = TTF_RenderUTF8_Blended(get_font_large(), result->title, text_color);
            if (text) {
                SDL_Rect src = {0, 0, text->w > title_max_w ? title_max_w : text->w, text->h};
                SDL_BlitSurface(text, &src, screen, &(SDL_Rect){title_x, text_y, 0, 0});
                SDL_FreeSurface(text);
            }
        }

        // Duration (always on right, outside pill)
        if (result->duration_sec > 0) {
            char dur[16];
            int m = result->duration_sec / 60;
            int s = result->duration_sec % 60;
            snprintf(dur, sizeof(dur), "%d:%02d", m, s);
            SDL_Surface* dur_text = TTF_RenderUTF8_Blended(get_font_tiny(), dur, is_selected ? COLOR_GRAY : COLOR_GRAY);
            if (dur_text) {
                SDL_BlitSurface(dur_text, NULL, screen, &(SDL_Rect){hw - dur_text->w - SCALE1(PADDING * 2), y + (item_h - dur_text->h) / 2});
                SDL_FreeSurface(dur_text);
            }
        }
    }

    // Empty results message
    if (result_count == 0) {
        const char* msg = searching ? "Searching..." : "No results found";
        SDL_Surface* text = TTF_RenderUTF8_Blended(get_font_large(), msg, COLOR_GRAY);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, hh / 2 - text->h / 2});
            SDL_FreeSurface(text);
        }
    }

    // Toast notification
    if (toast_message[0] != '\0') {
        uint32_t now = SDL_GetTicks();
        if (now - toast_time < YOUTUBE_TOAST_DURATION) {
            // Draw toast at bottom center
            SDL_Surface* toast_text = TTF_RenderUTF8_Blended(get_font_medium(), toast_message, COLOR_WHITE);
            if (toast_text) {
                int toast_w = toast_text->w + SCALE1(PADDING * 2);
                int toast_h = toast_text->h + SCALE1(8);
                int toast_x = (hw - toast_w) / 2;
                int toast_y = hh - SCALE1(BUTTON_SIZE + BUTTON_MARGIN + PADDING) - toast_h;

                // Draw semi-transparent background
                SDL_Rect bg_rect = {toast_x, toast_y, toast_w, toast_h};
                GFX_blitPill(ASSET_BLACK_PILL, screen, &bg_rect);

                // Draw text
                SDL_BlitSurface(toast_text, NULL, screen, &(SDL_Rect){toast_x + SCALE1(PADDING), toast_y + SCALE1(4)});
                SDL_FreeSurface(toast_text);
            }
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"U/D", "SELECT", NULL}, 0, screen, 0);

    // Dynamic hint based on queue status (only show A action if item is selected)
    if (selected >= 0 && result_count > 0) {
        const char* action_hint = "ADD";
        YouTubeResult* selected_result = &results[selected];
        if (YouTube_isInQueue(selected_result->video_id)) {
            action_hint = "REMOVE";
        }
        GFX_blitButtonGroup((char*[]){"A", (char*)action_hint, "B", "BACK", NULL}, 1, screen, 1);
    } else {
        GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
    }
}

// Render YouTube download queue
void render_youtube_queue(SDL_Surface* screen, int show_setting,
                          int queue_selected, int* queue_scroll) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "Download Queue";
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

    // Queue list
    int qcount = 0;
    YouTubeQueueItem* queue = YouTube_queueGet(&qcount);

    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int list_h = hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE);
    int items_per_page = list_h / item_h;

    // Adjust scroll
    if (queue_selected < *queue_scroll) {
        *queue_scroll = queue_selected;
    }
    if (queue_selected >= *queue_scroll + items_per_page) {
        *queue_scroll = queue_selected - items_per_page + 1;
    }

    int max_width = hw - SCALE1(PADDING * 4);

    for (int i = 0; i < items_per_page && *queue_scroll + i < qcount; i++) {
        int idx = *queue_scroll + i;
        YouTubeQueueItem* item = &queue[idx];
        bool selected = (idx == queue_selected);

        int y = list_y + i * item_h;

        // Status indicator (only for non-pending items)
        const char* status_str = NULL;
        SDL_Color status_color = COLOR_GRAY;
        switch (item->status) {
            case YOUTUBE_STATUS_PENDING: status_str = NULL; break;  // No prefix for pending
            case YOUTUBE_STATUS_DOWNLOADING: status_str = NULL; break;  // Show progress bar instead
            case YOUTUBE_STATUS_COMPLETE: status_str = "[OK]"; break;
            case YOUTUBE_STATUS_FAILED: status_str = "[X]"; break;
        }

        // Calculate status indicator width
        int status_width = 0;
        if (status_str) {
            int st_w, st_h;
            TTF_SizeUTF8(get_font_tiny(), status_str, &st_w, &st_h);
            status_width = st_w + SCALE1(8);
        }

        // Calculate text width for pill sizing
        char truncated[256];
        int pill_width = calc_list_pill_width(get_font_large(), item->title, truncated, max_width, status_width);

        // Background pill (sized to text width)
        SDL_Rect pill_rect = {SCALE1(PADDING), y, pill_width, item_h};
        draw_list_item_bg(screen, &pill_rect, selected);

        int title_x = SCALE1(PADDING) + SCALE1(BUTTON_PADDING);
        int text_y = y + (item_h - TTF_FontHeight(get_font_large())) / 2;

        // Render status indicator
        if (status_str) {
            SDL_Surface* status_text = TTF_RenderUTF8_Blended(get_font_tiny(), status_str, selected ? uintToColour(THEME_COLOR5_255) : status_color);
            if (status_text) {
                SDL_BlitSurface(status_text, NULL, screen, &(SDL_Rect){title_x, y + (item_h - status_text->h) / 2});
                title_x += status_text->w + SCALE1(8);
                SDL_FreeSurface(status_text);
            }
        }

        // Title
        SDL_Color text_color = get_list_text_color(selected);
        int title_max_w = pill_width - SCALE1(BUTTON_PADDING * 2) - status_width;

        if (selected) {
            // Selected item: use scrolling text
            ScrollText_update(&youtube_queue_scroll_text, item->title, get_font_large(), title_max_w,
                              text_color, screen, title_x, text_y);
        } else {
            // Non-selected items: static rendering with clipping
            SDL_Surface* text = TTF_RenderUTF8_Blended(get_font_large(), item->title, text_color);
            if (text) {
                SDL_Rect src = {0, 0, text->w > title_max_w ? title_max_w : text->w, text->h};
                SDL_BlitSurface(text, &src, screen, &(SDL_Rect){title_x, text_y, 0, 0});
                SDL_FreeSurface(text);
            }
        }

        // Progress bar for downloading items (always on right, outside pill)
        if (item->status == YOUTUBE_STATUS_DOWNLOADING) {
            int bar_w = SCALE1(60);
            int bar_h = SCALE1(8);
            int bar_x = hw - SCALE1(PADDING * 2) - bar_w;
            int bar_y = y + (item_h - bar_h) / 2;

            // Background bar
            SDL_Rect bg_rect = {bar_x, bar_y, bar_w, bar_h};
            SDL_FillRect(screen, &bg_rect, SDL_MapRGB(screen->format, 60, 60, 60));

            // Progress fill
            int fill_w = (bar_w * item->progress_percent) / 100;
            if (fill_w > 0) {
                SDL_Rect fill_rect = {bar_x, bar_y, fill_w, bar_h};
                SDL_FillRect(screen, &fill_rect, SDL_MapRGB(screen->format, 100, 200, 100));
            }

            // Percentage text
            char pct_str[8];
            snprintf(pct_str, sizeof(pct_str), "%d%%", item->progress_percent);
            SDL_Surface* pct_text = TTF_RenderUTF8_Blended(get_font_tiny(), pct_str, COLOR_GRAY);
            if (pct_text) {
                SDL_BlitSurface(pct_text, NULL, screen, &(SDL_Rect){bar_x - pct_text->w - SCALE1(4), y + (item_h - pct_text->h) / 2});
                SDL_FreeSurface(pct_text);
            }
        }
    }

    // Empty queue message
    if (qcount == 0) {
        const char* msg = "Queue is empty";
        SDL_Surface* text = TTF_RenderUTF8_Blended(get_font_large(), msg, COLOR_GRAY);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, hh / 2 - text->h / 2});
            SDL_FreeSurface(text);
        }
    }

    // Button hints
    if (qcount > 0) {
        GFX_blitButtonGroup((char*[]){"U/D", "SCROLL", NULL}, 0, screen, 0);
        GFX_blitButtonGroup((char*[]){"X", "REMOVE", "A", "START", "B", "BACK", NULL}, 1, screen, 1);
    } else {
        GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
    }
}

// Render YouTube downloading progress
void render_youtube_downloading(SDL_Surface* screen, int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "Downloading...";
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

    const YouTubeDownloadStatus* status = YouTube_getDownloadStatus();

    // Get current item progress from queue
    int current_progress = 0;
    int qcount = 0;
    YouTubeQueueItem* queue = YouTube_queueGet(&qcount);
    if (queue && status->current_index >= 0 && status->current_index < qcount) {
        current_progress = queue[status->current_index].progress_percent;
    }

    // Progress info
    char progress[128];
    snprintf(progress, sizeof(progress), "%d / %d completed", status->completed_count, status->total_items);
    SDL_Surface* prog_text = TTF_RenderUTF8_Blended(get_font_medium(), progress, COLOR_GRAY);
    if (prog_text) {
        SDL_BlitSurface(prog_text, NULL, screen, &(SDL_Rect){(hw - prog_text->w) / 2, hh / 2 - SCALE1(50)});
        SDL_FreeSurface(prog_text);
    }

    // Current track
    if (strlen(status->current_title) > 0) {
        GFX_truncateText(get_font_small(), status->current_title, truncated, hw - SCALE1(PADDING * 4), 0);
        SDL_Surface* curr_text = TTF_RenderUTF8_Blended(get_font_small(), truncated, COLOR_WHITE);
        if (curr_text) {
            SDL_BlitSurface(curr_text, NULL, screen, &(SDL_Rect){(hw - curr_text->w) / 2, hh / 2 - SCALE1(20)});
            SDL_FreeSurface(curr_text);
        }
    }

    // Progress bar for current download
    int bar_w = hw - SCALE1(PADDING * 8);
    int bar_h = SCALE1(16);
    int bar_x = (hw - bar_w) / 2;
    int bar_y = hh / 2 + SCALE1(10);

    // Background bar
    SDL_Rect bg_rect = {bar_x, bar_y, bar_w, bar_h};
    SDL_FillRect(screen, &bg_rect, SDL_MapRGB(screen->format, 60, 60, 60));

    // Progress fill
    int fill_w = (bar_w * current_progress) / 100;
    if (fill_w > 0) {
        SDL_Rect fill_rect = {bar_x, bar_y, fill_w, bar_h};
        SDL_FillRect(screen, &fill_rect, SDL_MapRGB(screen->format, 100, 200, 100));
    }

    // Percentage text
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", current_progress);
    SDL_Surface* pct_text = TTF_RenderUTF8_Blended(get_font_medium(), pct_str, COLOR_WHITE);
    if (pct_text) {
        SDL_BlitSurface(pct_text, NULL, screen, &(SDL_Rect){(hw - pct_text->w) / 2, bar_y + bar_h + SCALE1(8)});
        SDL_FreeSurface(pct_text);
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"B", "CANCEL", NULL}, 1, screen, 1);
}

// Check if YouTube results list has active scrolling (for refresh optimization)
bool youtube_results_needs_scroll_refresh(void) {
    return ScrollText_isScrolling(&youtube_results_scroll_text);
}

// Check if YouTube queue list has active scrolling (for refresh optimization)
bool youtube_queue_needs_scroll_refresh(void) {
    return ScrollText_isScrolling(&youtube_queue_scroll_text);
}

// Render YouTube yt-dlp update progress
void render_youtube_updating(SDL_Surface* screen, int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "Updating yt-dlp";
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

    const YouTubeUpdateStatus* status = YouTube_getUpdateStatus();

    // Current version
    char ver_str[128];
    snprintf(ver_str, sizeof(ver_str), "Current: %s", status->current_version);
    SDL_Surface* ver_text = TTF_RenderUTF8_Blended(get_font_medium(), ver_str, COLOR_GRAY);
    if (ver_text) {
        SDL_BlitSurface(ver_text, NULL, screen, &(SDL_Rect){(hw - ver_text->w) / 2, hh / 2 - SCALE1(50)});
        SDL_FreeSurface(ver_text);
    }

    // Status message
    const char* status_msg = "Checking for updates...";
    if (status->progress_percent >= 30 && status->progress_percent < 50) {
        status_msg = "Checking version...";
    } else if (status->progress_percent >= 50 && status->progress_percent < 80) {
        status_msg = "Downloading update...";
    } else if (status->progress_percent >= 80 && status->progress_percent < 100) {
        status_msg = "Installing...";
    } else if (status->progress_percent >= 100) {
        status_msg = "Update complete!";
    } else if (!status->updating && strlen(status->error_message) > 0) {
        status_msg = status->error_message;
    } else if (!status->updating && !status->update_available && status->progress_percent > 0) {
        status_msg = "Already up to date!";
    }

    SDL_Surface* status_text = TTF_RenderUTF8_Blended(get_font_medium(), status_msg, COLOR_WHITE);
    if (status_text) {
        SDL_BlitSurface(status_text, NULL, screen, &(SDL_Rect){(hw - status_text->w) / 2, hh / 2});
        SDL_FreeSurface(status_text);
    }

    // Latest version (if known)
    if (strlen(status->latest_version) > 0) {
        snprintf(ver_str, sizeof(ver_str), "Latest: %s", status->latest_version);
        SDL_Surface* latest_text = TTF_RenderUTF8_Blended(get_font_small(), ver_str, COLOR_GRAY);
        if (latest_text) {
            SDL_BlitSurface(latest_text, NULL, screen, &(SDL_Rect){(hw - latest_text->w) / 2, hh / 2 + SCALE1(30)});
            SDL_FreeSurface(latest_text);
        }
    }

    // Progress bar
    if (status->updating) {
        int bar_w = hw - SCALE1(PADDING * 8);
        int bar_h = SCALE1(10);
        int bar_x = SCALE1(PADDING * 4);
        int bar_y = hh / 2 + SCALE1(60);

        // Background
        SDL_Rect bg_rect = {bar_x, bar_y, bar_w, bar_h};
        SDL_FillRect(screen, &bg_rect, SDL_MapRGB(screen->format, 64, 64, 64));

        // Progress
        int prog_w = (bar_w * status->progress_percent) / 100;
        SDL_Rect prog_rect = {bar_x, bar_y, prog_w, bar_h};
        SDL_FillRect(screen, &prog_rect, SDL_MapRGB(screen->format, 255, 255, 255));
    }

    // Button hints
    if (status->updating) {
        GFX_blitButtonGroup((char*[]){"B", "CANCEL", NULL}, 1, screen, 1);
    } else {
        GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
    }
}
