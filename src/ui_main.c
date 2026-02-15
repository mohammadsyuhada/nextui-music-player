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
#include "module_menu.h"
#include "resume.h"
#include "background.h"

// Menu items variants (first entry is mutable for Resume/Now Playing swap)
static const char* menu_items_with_first[] = {"Resume", "Library", "Online Radio", "Podcasts", "Settings"};
static const char* menu_items_no_first[] = {"Library", "Online Radio", "Podcasts", "Settings"};

// Cached first_item_mode for callbacks
static int current_first_item_mode = MENU_FIRST_NONE;

// Scroll state for Resume track name
static ScrollTextState resume_scroll = {0};

// Get label for Now Playing based on background player type
static const char* get_now_playing_label(void) {
    switch (Background_getActive()) {
        case BG_MUSIC:  return "Music";
        case BG_RADIO:  return "Radio";
        case BG_PODCAST: return "Podcast";
        default: return "Audio";
    }
}

// Label callback for first item label and Settings update badge
static const char* main_menu_get_label(int index, const char* default_label,
                                        char* buffer, int buffer_size) {
    bool has_first = (current_first_item_mode != MENU_FIRST_NONE);

    // First item: return full label for pill sizing
    if (has_first && index == 0) {
        if (current_first_item_mode == MENU_FIRST_NOW_PLAYING) {
            snprintf(buffer, buffer_size, "Now Playing: %s", get_now_playing_label());
            return buffer;
        }
        // Resume mode
        const char* label = Resume_getLabel();
        if (label) {
            snprintf(buffer, buffer_size, "%s", label);
            return buffer;
        }
    }

    // Settings item: show update badge
    int settings_index = has_first ? 4 : 3;
    if (index == settings_index) {
        const SelfUpdateStatus* status = SelfUpdate_getStatus();
        if (status->update_available) {
            snprintf(buffer, buffer_size, "Settings (Update available)");
            return buffer;
        }
    }
    return NULL;  // Use default label
}

// Custom text rendering for first item: fixed prefix + scrolling text
static bool main_menu_render_text(SDL_Surface* screen, int index, bool selected,
                                   int text_x, int text_y, int max_text_width) {
    if (current_first_item_mode == MENU_FIRST_NONE || index != 0) return false;

    // Only custom-render when selected (for scrolling); default rendering handles non-selected
    if (!selected) return false;

    const char* track_name;
    const char* prefix;

    if (current_first_item_mode == MENU_FIRST_NOW_PLAYING) {
        prefix = "Now Playing: ";
        track_name = get_now_playing_label();
    } else {
        const ResumeState* rs = Resume_getState();
        if (!rs) return false;
        track_name = rs->track_name[0] ? rs->track_name : "Unknown";
        prefix = "Resume: ";
    }
    SDL_Color text_color = Fonts_getListTextColor(true);
    TTF_Font* font = Fonts_getLarge();

    int prefix_width = 0;
    TTF_SizeUTF8(font, prefix, &prefix_width, NULL);

    SDL_Surface* prefix_surf = TTF_RenderUTF8_Blended(font, prefix, text_color);
    if (prefix_surf) {
        SDL_BlitSurface(prefix_surf, NULL, screen, &(SDL_Rect){text_x, text_y});
        SDL_FreeSurface(prefix_surf);
    }

    // Render track name in remaining space with clip rect to prevent overflow
    int remaining_width = max_text_width - prefix_width;
    if (remaining_width > 0) {
        int track_x = text_x + prefix_width;

        // Set clip rect to bound the track name within pill
        SDL_Rect old_clip;
        SDL_GetClipRect(screen, &old_clip);
        SDL_Rect clip = {track_x, text_y, remaining_width, TTF_FontHeight(font)};
        SDL_SetClipRect(screen, &clip);

        // Use software scroll (use_gpu=false) to respect SDL clip rect
        ScrollText_update(&resume_scroll, track_name, font, remaining_width,
                          text_color, screen, track_x, text_y, false);

        // Restore clip rect
        if (old_clip.w > 0 && old_clip.h > 0)
            SDL_SetClipRect(screen, &old_clip);
        else
            SDL_SetClipRect(screen, NULL);
    }

    return true;
}

// Render the main menu
void render_menu(SDL_Surface* screen, int show_setting, int menu_selected,
                 char* toast_message, uint32_t toast_time, int first_item_mode) {
    current_first_item_mode = first_item_mode;
    bool has_first = (first_item_mode != MENU_FIRST_NONE);

    // Update the first item label based on mode
    if (first_item_mode == MENU_FIRST_NOW_PLAYING) {
        menu_items_with_first[0] = "Now Playing";
    } else {
        menu_items_with_first[0] = "Resume";
    }

    const char** items = has_first ? menu_items_with_first : menu_items_no_first;
    int count = has_first ? 5 : 4;

    SimpleMenuConfig config = {
        .title = "Music Player",
        .items = items,
        .item_count = count,
        .btn_b_label = "EXIT",
        .get_label = main_menu_get_label,
        .render_badge = NULL,
        .get_icon = NULL,
        .render_text = main_menu_render_text
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
    {"X", "Clear History/Playback"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// File browser controls (A/B shown in footer)
static const ControlHelp browser_controls[] = {
    {"Up/Down", "Navigate"},
    {"Y", "Add to Playlist"},
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
    {"R2/R3", "Toggle Lyrics"},
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

// Playlist list controls (A/B shown in footer)
static const ControlHelp playlist_list_controls[] = {
    {"Up/Down", "Navigate"},
    {"X", "Delete Playlist"},
    {"Start (hold)", "Exit App"},
    {NULL, NULL}
};

// Playlist detail controls (A/B shown in footer)
static const ControlHelp playlist_detail_controls[] = {
    {"Up/Down", "Navigate"},
    {"X", "Remove Track"},
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
        case 50: // PLAYLIST_LIST_HELP_STATE
            controls = playlist_list_controls;
            page_title = "Playlists";
            break;
        case 51: // PLAYLIST_DETAIL_HELP_STATE
            controls = playlist_detail_controls;
            page_title = "Playlist Tracks";
            break;
        case 55: // LIBRARY_MENU_HELP_STATE
            controls = main_menu_controls;
            page_title = "Library";
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

    // Dialog box
    int line_height = SCALE1(18);
    int hint_gap = SCALE1(15);
    int box_h = SCALE1(60) + (control_count * line_height) + hint_gap;
    DialogBox db = render_dialog_box(screen, SCALE1(240), box_h);

    // Title text (left aligned)
    SDL_Surface* title_surf = TTF_RenderUTF8_Blended(Fonts_getMedium(), page_title, COLOR_WHITE);
    if (title_surf) {
        SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){db.content_x, db.box_y + SCALE1(10)});
        SDL_FreeSurface(title_surf);
    }

    int y_offset = db.box_y + SCALE1(35);
    int right_col = db.box_x + SCALE1(90);

    for (int i = 0; i < control_count; i++) {
        // Button name
        SDL_Surface* btn_surf = TTF_RenderUTF8_Blended(Fonts_getSmall(), controls[i].button, COLOR_GRAY);
        if (btn_surf) {
            SDL_BlitSurface(btn_surf, NULL, screen, &(SDL_Rect){db.content_x, y_offset});
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
        int hint_y = db.box_y + db.box_h - SCALE1(10) - hint_surf->h;
        SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){db.content_x, hint_y});
        SDL_FreeSurface(hint_surf);
    }
}

// Render confirmation dialog overlay (title + optional content + "A: Yes  B: No")
void render_confirmation_dialog(SDL_Surface* screen, const char* content, const char* title) {
    bool has_content = content && content[0];
    int box_h = has_content ? SCALE1(110) : SCALE1(90);
    DialogBox db = render_dialog_box(screen, SCALE1(280), box_h);
    int hw = screen->w;

    // Title text
    if (!title) title = "Delete File?";
    int title_y = has_content ? db.box_y + SCALE1(15) : db.box_y + SCALE1(20);
    SDL_Surface* title_surf = TTF_RenderUTF8_Blended(Fonts_getMedium(), title, COLOR_WHITE);
    if (title_surf) {
        SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){(hw - title_surf->w) / 2, title_y});
        SDL_FreeSurface(title_surf);
    }

    // Content text (truncated if needed)
    if (has_content) {
        char truncated[64];
        GFX_truncateText(Fonts_getSmall(), content, truncated, db.box_w - SCALE1(20), 0);
        SDL_Surface* name_surf = TTF_RenderUTF8_Blended(Fonts_getSmall(), truncated, COLOR_GRAY);
        if (name_surf) {
            SDL_BlitSurface(name_surf, NULL, screen, &(SDL_Rect){(hw - name_surf->w) / 2, db.box_y + SCALE1(45)});
            SDL_FreeSurface(name_surf);
        }
    }

    // Button hints
    int hint_y = has_content ? db.box_y + SCALE1(75) : db.box_y + SCALE1(55);
    const char* hint = "A: Yes   B: No";
    SDL_Surface* hint_surf = TTF_RenderUTF8_Blended(Fonts_getSmall(), hint, COLOR_GRAY);
    if (hint_surf) {
        SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){(hw - hint_surf->w) / 2, hint_y});
        SDL_FreeSurface(hint_surf);
    }
}

// Check if Resume scroll needs continuous redraw (software scroll mode)
bool menu_needs_scroll_redraw(void) {
    // Needs redraw if scrolling is active OR about to start (delay -> active transition)
    return ScrollText_isScrolling(&resume_scroll) || ScrollText_needsRender(&resume_scroll);
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
