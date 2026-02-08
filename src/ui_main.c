#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_main.h"
#include "ui_fonts.h"
#include "ui_utils.h"
#include "ui_icons.h"
#include "selfupdate.h"
#include "module_common.h"

// Menu items
static const char* menu_items[] = {"Local Files", "Online Radio", "Podcasts", "Downloader", "Settings"};
#define MENU_ITEM_COUNT 5

// Label callback for update badge on Settings menu item
static const char* main_menu_get_label(int index, const char* default_label,
                                        char* buffer, int buffer_size) {
    if (index == 4) {  // Settings menu item (now index 4 with Podcasts added)
        const SelfUpdateStatus* status = SelfUpdate_getStatus();
        if (status->update_available) {
            snprintf(buffer, buffer_size, "Settings (Update available)");
            return buffer;
        }
    }
    return NULL;  // Use default label
}

// Render the main menu
void render_menu(SDL_Surface* screen, int show_setting, int menu_selected,
                 char* toast_message, uint32_t toast_time) {
    SimpleMenuConfig config = {
        .title = "Music Player",
        .items = menu_items,
        .item_count = MENU_ITEM_COUNT,
        .btn_b_label = "EXIT",
        .get_label = main_menu_get_label,
        .render_badge = NULL,
        .get_icon = NULL
    };
    render_simple_menu(screen, show_setting, menu_selected, &config);

    // Toast notification
    render_toast(screen, toast_message, toast_time);
}

// Controls help text for each page/state
typedef struct {
    const char* button;
    const char* action;
} ControlHelp;

// Main menu controls (A/B shown in footer)
static const ControlHelp main_menu_controls[] = {
    {"Up/Down", "Navigate"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// File browser controls (A/B shown in footer)
static const ControlHelp browser_controls[] = {
    {"Up/Down", "Navigate"},
    {"X", "Delete File"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Music player controls (A/B shown in footer)
static const ControlHelp player_controls[] = {
    {"X", "Toggle Shuffle"},
    {"Y", "Toggle Repeat"},
    {"Up/R1", "Next Track"},
    {"Down/L1", "Prev Track"},
    {"Left/Right", "Seek"},
    {"L2/L3", "Toggle Visualizer"},
    {"R2/R3", "Visualizer Color"},
    {"Select", "Screen Off"},
    {"Select + A", "Wake Screen"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Radio list controls (A/B shown in footer)
static const ControlHelp radio_list_controls[] = {
    {"Up/Down", "Navigate"},
    {"Y", "Manage Stations"},
    {"X", "Delete Station"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Radio playing controls (B shown in footer)
static const ControlHelp radio_playing_controls[] = {
    {"Up/R1", "Next Station"},
    {"Down/L1", "Prev Station"},
    {"Select", "Screen Off"},
    {"Select + A", "Wake Screen"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Radio manage stations controls - country list (A/B shown in footer)
static const ControlHelp radio_manage_controls[] = {
    {"Up/Down", "Navigate"},
    {"Y", "Manual Setup Help"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Radio browse stations controls - station list (A/B shown in footer)
static const ControlHelp radio_browse_controls[] = {
    {"Up/Down", "Navigate"},
    {"A", "Add/Remove Station"},
    {"Y", "Manual Setup Help"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Podcast menu controls (shows subscribed podcasts)
static const ControlHelp podcast_menu_controls[] = {
    {"Up/Down", "Navigate"},
    {"X", "Unsubscribe"},
    {"Y", "Manage Podcasts"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Podcast manage menu controls
static const ControlHelp podcast_manage_controls[] = {
    {"Up/Down", "Navigate"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Podcast subscriptions list controls
static const ControlHelp podcast_subscriptions_controls[] = {
    {"Up/Down", "Navigate"},
    {"X", "Unsubscribe"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Podcast top shows controls
static const ControlHelp podcast_top_shows_controls[] = {
    {"Up/Down", "Navigate"},
    {"A", "Subscribe/Unsubscribe"},
    {"X", "Refresh List"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Podcast search results controls
static const ControlHelp podcast_search_controls[] = {
    {"Up/Down", "Navigate"},
    {"A", "Subscribe/Unsubscribe"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Podcast episodes list controls
static const ControlHelp podcast_episodes_controls[] = {
    {"Up/Down", "Navigate"},
    {"Y", "Refresh Episodes"},
    {"X", "Mark Played/Unplayed"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Podcast playing controls
static const ControlHelp podcast_playing_controls[] = {
    {"Left", "Rewind 10s"},
    {"Right", "Forward 30s"},
    {"Select", "Screen Off"},
    {"Select + A", "Wake Screen"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// YouTube menu controls (A/B shown in footer)
static const ControlHelp youtube_menu_controls[] = {
    {"Up/Down", "Navigate"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// YouTube results controls (A/B shown in footer)
static const ControlHelp youtube_results_controls[] = {
    {"Up/Down", "Navigate"},
    {"B", "Back"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// YouTube queue controls (A/B/X shown in footer)
static const ControlHelp youtube_queue_controls[] = {
    {"Up/Down", "Navigate"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// About page controls (A/B shown in footer)
static const ControlHelp about_controls[] = {
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Settings menu controls
static const ControlHelp settings_controls[] = {
    {"Up/Down", "Navigate"},
    {"Left/Right", "Change Value"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Generic/default controls
static const ControlHelp default_controls[] = {
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Render controls help dialog overlay
void render_controls_help(SDL_Surface* screen, int app_state) {
    int hw = screen->w;
    int hh = screen->h;

    // Select controls based on state
    const ControlHelp* controls;
    const char* page_title;

    // AppState enum values:
    // STATE_MENU=0, STATE_BROWSER=1, STATE_PLAYING=2, STATE_RADIO_LIST=3,
    // STATE_RADIO_PLAYING=4, STATE_RADIO_ADD=5, STATE_RADIO_ADD_STATIONS=6,
    // STATE_RADIO_HELP=7, STATE_PODCAST_MENU=8, STATE_PODCAST_MANAGE=9,
    // STATE_PODCAST_SUBSCRIPTIONS=10, STATE_PODCAST_TOP_SHOWS=11,
    // STATE_PODCAST_SEARCH_RESULTS=12, STATE_PODCAST_EPISODES=13,
    // STATE_PODCAST_PLAYING=14, STATE_PODCAST_DOWNLOADS=15,
    // STATE_DOWNLOADER_MENU=16, STATE_DOWNLOADER_SEARCHING=17, STATE_DOWNLOADER_RESULTS=18,
    // STATE_DOWNLOADER_QUEUE=19, STATE_DOWNLOADER_DOWNLOADING=20, STATE_DOWNLOADER_UPDATING=21,
    // STATE_APP_UPDATING=22, STATE_ABOUT=23
    switch (app_state) {
        case 0:  // STATE_MENU
            controls = main_menu_controls;
            page_title = "Main Menu";
            break;
        case 1:  // STATE_BROWSER
            controls = browser_controls;
            page_title = "File Browser";
            break;
        case 2:  // STATE_PLAYING
            controls = player_controls;
            page_title = "Music Player";
            break;
        case 3:  // STATE_RADIO_LIST
            controls = radio_list_controls;
            page_title = "Radio Stations";
            break;
        case 4:  // STATE_RADIO_PLAYING
            controls = radio_playing_controls;
            page_title = "Radio Player";
            break;
        case 5:  // STATE_RADIO_ADD
            controls = radio_manage_controls;
            page_title = "Manage Stations";
            break;
        case 6:  // STATE_RADIO_ADD_STATIONS
            controls = radio_browse_controls;
            page_title = "Browse Stations";
            break;
        case 8:  // STATE_PODCAST_MENU (legacy)
        case 30: // PODCAST_INTERNAL_MENU
            controls = podcast_menu_controls;
            page_title = "Podcasts";
            break;
        case 31: // PODCAST_INTERNAL_MANAGE
            controls = podcast_manage_controls;
            page_title = "Manage Podcasts";
            break;
        case 32: // PODCAST_INTERNAL_SUBSCRIPTIONS
            controls = podcast_subscriptions_controls;
            page_title = "Subscriptions";
            break;
        case 33: // PODCAST_INTERNAL_TOP_SHOWS
            controls = podcast_top_shows_controls;
            page_title = "Top Shows";
            break;
        case 34: // PODCAST_INTERNAL_SEARCH_RESULTS
            controls = podcast_search_controls;
            page_title = "Search Results";
            break;
        case 35: // PODCAST_INTERNAL_EPISODES
            controls = podcast_episodes_controls;
            page_title = "Episodes";
            break;
        case 36: // PODCAST_INTERNAL_BUFFERING
            controls = default_controls;
            page_title = "Buffering";
            break;
        case 15: // STATE_PODCAST_PLAYING (legacy)
        case 37: // PODCAST_INTERNAL_PLAYING
            controls = podcast_playing_controls;
            page_title = "Podcast Player";
            break;
        case 16: // STATE_DOWNLOADER_MENU
            controls = youtube_menu_controls;
            page_title = "Downloader";
            break;
        case 18: // STATE_DOWNLOADER_RESULTS
            controls = youtube_results_controls;
            page_title = "Search Results";
            break;
        case 19: // STATE_DOWNLOADER_QUEUE
            controls = youtube_queue_controls;
            page_title = "Download Queue";
            break;
        case 23: // STATE_ABOUT
            controls = about_controls;
            page_title = "About";
            break;
        case 40: // SETTINGS_INTERNAL_MENU
            controls = settings_controls;
            page_title = "Settings";
            break;
        case 41: // SETTINGS_INTERNAL_ABOUT
            controls = about_controls;
            page_title = "About";
            break;
        default:
            controls = default_controls;
            page_title = "Controls";
            break;
    }

    // Count controls
    int control_count = 0;
    while (controls[control_count].button != NULL) {
        control_count++;
    }

    // Dialog box dimensions
    int line_height = SCALE1(18);
    int box_w = SCALE1(240);
    int hint_gap = SCALE1(15);  // Gap between controls and hint
    int box_h = SCALE1(60) + (control_count * line_height) + hint_gap;
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

    // Controls list
    int left_margin = box_x + SCALE1(15);

    // Title text (left aligned)
    SDL_Surface* title_surf = TTF_RenderUTF8_Blended(Fonts_getMedium(), page_title, COLOR_WHITE);
    if (title_surf) {
        SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){left_margin, box_y + SCALE1(10)});
        SDL_FreeSurface(title_surf);
    }

    int y_offset = box_y + SCALE1(35);
    int right_col = box_x + SCALE1(90);

    for (int i = 0; i < control_count; i++) {
        // Button name
        SDL_Surface* btn_surf = TTF_RenderUTF8_Blended(Fonts_getSmall(), controls[i].button, COLOR_GRAY);
        if (btn_surf) {
            SDL_BlitSurface(btn_surf, NULL, screen, &(SDL_Rect){left_margin, y_offset});
            SDL_FreeSurface(btn_surf);
        }

        // Action description
        SDL_Surface* action_surf = TTF_RenderUTF8_Blended(Fonts_getSmall(), controls[i].action, COLOR_WHITE);
        if (action_surf) {
            SDL_BlitSurface(action_surf, NULL, screen, &(SDL_Rect){right_col, y_offset});
            SDL_FreeSurface(action_surf);
        }

        y_offset += line_height;
    }

    // Button hint at bottom (left aligned, same gap as title from top)
    const char* hint = "Press any button to close";
    SDL_Surface* hint_surf = TTF_RenderUTF8_Blended(Fonts_getSmall(), hint, COLOR_GRAY);
    if (hint_surf) {
        int hint_y = box_y + box_h - SCALE1(10) - hint_surf->h;
        SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){left_margin, hint_y});
        SDL_FreeSurface(hint_surf);
    }
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
    SDL_Surface* msg_surf = TTF_RenderUTF8_Blended(Fonts_getMedium(), msg, COLOR_WHITE);
    if (msg_surf) {
        SDL_BlitSurface(msg_surf, NULL, screen, &(SDL_Rect){(hw - msg_surf->w) / 2, box_y + SCALE1(20)});
        SDL_FreeSurface(msg_surf);
    }

    // Button hints
    const char* hint = "A: Yes   B: No";
    SDL_Surface* hint_surf = TTF_RenderUTF8_Blended(Fonts_getSmall(), hint, COLOR_GRAY);
    if (hint_surf) {
        SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){(hw - hint_surf->w) / 2, box_y + SCALE1(55)});
        SDL_FreeSurface(hint_surf);
    }
}

// Render delete confirmation dialog overlay
void render_delete_confirm(SDL_Surface* screen, const char* filename) {
    int hw = screen->w;
    int hh = screen->h;

    // Dialog box (centered) - wider to accommodate filename
    int box_w = SCALE1(280);
    int box_h = SCALE1(110);
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

    // Title text
    const char* title = "Delete File?";
    SDL_Surface* title_surf = TTF_RenderUTF8_Blended(Fonts_getMedium(), title, COLOR_WHITE);
    if (title_surf) {
        SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){(hw - title_surf->w) / 2, box_y + SCALE1(15)});
        SDL_FreeSurface(title_surf);
    }

    // Filename text (truncated if needed)
    char truncated[64];
    GFX_truncateText(Fonts_getSmall(), filename, truncated, box_w - SCALE1(20), 0);
    SDL_Surface* name_surf = TTF_RenderUTF8_Blended(Fonts_getSmall(), truncated, COLOR_GRAY);
    if (name_surf) {
        SDL_BlitSurface(name_surf, NULL, screen, &(SDL_Rect){(hw - name_surf->w) / 2, box_y + SCALE1(45)});
        SDL_FreeSurface(name_surf);
    }

    // Button hints
    const char* hint = "A: Yes   B: No";
    SDL_Surface* hint_surf = TTF_RenderUTF8_Blended(Fonts_getSmall(), hint, COLOR_GRAY);
    if (hint_surf) {
        SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){(hw - hint_surf->w) / 2, box_y + SCALE1(75)});
        SDL_FreeSurface(hint_surf);
    }
}

// Render screen off hint message (shown before screen turns off)
void render_screen_off_hint(SDL_Surface* screen) {
    int hw = screen->w;
    int hh = screen->h;

    // Fill entire screen with black
    SDL_FillRect(screen, NULL, RGB_BLACK);

    // Render hint message centered
    const char* msg = "Press SELECT + A to wake screen";
    SDL_Surface* msg_surf = TTF_RenderUTF8_Blended(Fonts_getMedium(), msg, COLOR_WHITE);
    if (msg_surf) {
        SDL_BlitSurface(msg_surf, NULL, screen, &(SDL_Rect){(hw - msg_surf->w) / 2, (hh - msg_surf->h) / 2});
        SDL_FreeSurface(msg_surf);
    }
}
