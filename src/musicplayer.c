#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "config.h"
#include "player.h"
#include "radio.h"
#include "youtube.h"
#include "selfupdate.h"

// App states
typedef enum {
    STATE_MENU = 0,         // Main menu (Files / Radio / YouTube / Settings)
    STATE_BROWSER,          // File browser
    STATE_PLAYING,          // Playing local file
    STATE_RADIO_LIST,       // Radio station list
    STATE_RADIO_PLAYING,    // Playing radio stream
    STATE_RADIO_ADD,        // Add stations - country selection
    STATE_RADIO_ADD_STATIONS, // Add stations - station selection
    STATE_RADIO_HELP,       // Help/instructions screen
    STATE_YOUTUBE_MENU,     // YouTube sub-menu
    STATE_YOUTUBE_SEARCHING, // Searching in progress
    STATE_YOUTUBE_RESULTS,  // YouTube search results
    STATE_YOUTUBE_QUEUE,    // Download queue view
    STATE_YOUTUBE_DOWNLOADING, // Downloading progress
    STATE_YOUTUBE_UPDATING, // yt-dlp update progress
    STATE_APP_UPDATING,     // App self-update progress
    STATE_ABOUT             // About screen
} AppState;

// Custom fonts for the interface (except buttons)
// Different font variants for different elements
typedef struct {
    TTF_Font* title;      // Track title (Regular)
    TTF_Font* artist;     // Artist name (Medium)
    TTF_Font* album;      // Album name (Bold)
    TTF_Font* time_large; // Time display (Regular large)
    TTF_Font* badge;      // Format badge, small text (Regular small)
    TTF_Font* tiny;       // Genre, bitrate (Regular tiny)
    bool loaded;          // True if custom fonts were loaded
} CustomFonts;

static CustomFonts custom_font = {0};

// File entry
typedef struct {
    char name[256];
    char path[512];
    bool is_dir;
    AudioFormat format;
} FileEntry;

// Browser context
typedef struct {
    char current_path[512];
    FileEntry* entries;
    int entry_count;
    int selected;
    int scroll_offset;
    int items_per_page;
} BrowserContext;

// Menu items
static const char* menu_items[] = {"Local Files", "Internet Radio", "MP3 Downloader", "About"};
#define MENU_ITEM_COUNT 4

// YouTube sub-menu items
static const char* youtube_menu_items[] = {"Search Music", "Download Queue", "Update yt-dlp"};
#define YOUTUBE_MENU_COUNT 3

// YouTube state
static int youtube_menu_selected = 0;
static int youtube_results_selected = 0;
static int youtube_results_scroll = 0;
static int youtube_queue_selected = 0;
static int youtube_queue_scroll = 0;
static YouTubeResult youtube_results[YOUTUBE_MAX_RESULTS];
static int youtube_result_count = 0;
static bool youtube_searching = false;
static char youtube_search_query[256] = "";
static char youtube_toast_message[128] = "";
static uint32_t youtube_toast_time = 0;
#define YOUTUBE_TOAST_DURATION 1500  // 1.5 seconds

// Global state
static bool quit = false;
static AppState app_state = STATE_MENU;
static SDL_Surface* screen;
static BrowserContext browser = {0};
static int menu_selected = 0;
static int radio_selected = 0;
static int radio_scroll = 0;

// Add stations UI state
static int add_country_selected = 0;
static int add_country_scroll = 0;
static int add_station_selected = 0;
static int add_station_scroll = 0;
static const char* add_selected_country_code = NULL;
static bool add_station_checked[256];  // Track selected stations for adding
static int help_scroll = 0;  // Scroll position for help page

// Screen off mode (screen off but audio keeps playing)
static bool screen_off = false;
static bool autosleep_disabled = false;
static uint32_t last_input_time = 0;  // For auto screen-off after inactivity

// Quit confirmation dialog
static bool show_quit_confirm = false;

// Helper function to get current radio station
static RadioStation* get_current_station(void) {
    RadioStation* stations;
    int count = Radio_getStations(&stations);
    if (count > 0 && radio_selected < count) {
        return &stations[radio_selected];
    }
    return NULL;
}

// Shuffle and repeat modes
static bool shuffle_enabled = false;
static bool repeat_enabled = false;

// Music folder
#define MUSIC_PATH SDCARD_PATH "/Music"

static void sigHandler(int sig) {
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        quit = true;
        break;
    default:
        break;
    }
}

// Load custom fonts from pak folder
// Uses different font variants:
// - JetBrainsMono-Regular.ttf for title
// - JetBrainsMono-Medium.ttf for artist
// - JetBrainsMono-Bold.ttf for album
static void load_custom_fonts(void) {
    char regular_path[512], medium_path[512], bold_path[512];

    // Try pak folder first, then current directory
    const char* search_paths[] = {
        "%s/.system/tg5040/paks/Emus/Music Player.pak",
        "."
    };

    bool found = false;
    const char* base_path = NULL;

    for (int i = 0; i < 2 && !found; i++) {
        char test_path[512];
        if (i == 0) {
            snprintf(test_path, sizeof(test_path), search_paths[0], SDCARD_PATH);
        } else {
            strcpy(test_path, search_paths[1]);
        }

        snprintf(regular_path, sizeof(regular_path), "%s/fonts/JetBrainsMono-Regular.ttf", test_path);
        snprintf(medium_path, sizeof(medium_path), "%s/fonts/JetBrainsMono-Medium.ttf", test_path);
        snprintf(bold_path, sizeof(bold_path), "%s/fonts/JetBrainsMono-Bold.ttf", test_path);

        if (access(regular_path, F_OK) == 0 &&
            access(medium_path, F_OK) == 0 &&
            access(bold_path, F_OK) == 0) {
            found = true;
            base_path = test_path;
        }
    }

    if (!found) {
        custom_font.loaded = false;
        return;
    }

    // Load font variants at appropriate sizes
    // Title uses Regular at extra large size (2x artist size)
    custom_font.title = TTF_OpenFont(regular_path, SCALE1(28));
    // Artist uses Medium at medium size
    custom_font.artist = TTF_OpenFont(medium_path, SCALE1(FONT_MEDIUM));
    // Album uses Bold at small size
    custom_font.album = TTF_OpenFont(bold_path, SCALE1(FONT_SMALL));
    // Time display uses Regular at medium size
    custom_font.time_large = TTF_OpenFont(regular_path, SCALE1(FONT_MEDIUM));
    // Badge/small text uses Regular at small size
    custom_font.badge = TTF_OpenFont(regular_path, SCALE1(FONT_SMALL));
    // Tiny text uses Regular at tiny size
    custom_font.tiny = TTF_OpenFont(regular_path, SCALE1(FONT_TINY));

    if (custom_font.title && custom_font.artist && custom_font.album &&
        custom_font.time_large && custom_font.badge && custom_font.tiny) {
        custom_font.loaded = true;
    } else {
        // Failed to load, cleanup partial loads
        if (custom_font.title) { TTF_CloseFont(custom_font.title); custom_font.title = NULL; }
        if (custom_font.artist) { TTF_CloseFont(custom_font.artist); custom_font.artist = NULL; }
        if (custom_font.album) { TTF_CloseFont(custom_font.album); custom_font.album = NULL; }
        if (custom_font.time_large) { TTF_CloseFont(custom_font.time_large); custom_font.time_large = NULL; }
        if (custom_font.badge) { TTF_CloseFont(custom_font.badge); custom_font.badge = NULL; }
        if (custom_font.tiny) { TTF_CloseFont(custom_font.tiny); custom_font.tiny = NULL; }
        custom_font.loaded = false;
    }
}

// Cleanup custom fonts
static void unload_custom_fonts(void) {
    if (custom_font.title) { TTF_CloseFont(custom_font.title); custom_font.title = NULL; }
    if (custom_font.artist) { TTF_CloseFont(custom_font.artist); custom_font.artist = NULL; }
    if (custom_font.album) { TTF_CloseFont(custom_font.album); custom_font.album = NULL; }
    if (custom_font.time_large) { TTF_CloseFont(custom_font.time_large); custom_font.time_large = NULL; }
    if (custom_font.badge) { TTF_CloseFont(custom_font.badge); custom_font.badge = NULL; }
    if (custom_font.tiny) { TTF_CloseFont(custom_font.tiny); custom_font.tiny = NULL; }
    custom_font.loaded = false;
}

// Get font for specific element (custom or system fallback)
// Title font (Regular large) - for track title
static TTF_Font* get_font_title(void) {
    return custom_font.loaded ? custom_font.title : font.large;
}

// Artist font (Medium) - for artist name
static TTF_Font* get_font_artist(void) {
    return custom_font.loaded ? custom_font.artist : font.medium;
}

// Album font (Bold medium) - for album name
static TTF_Font* get_font_album(void) {
    return custom_font.loaded ? custom_font.album : font.medium;
}

// Large font for general use (menus, time display)
static TTF_Font* get_font_large(void) {
    return custom_font.loaded ? custom_font.time_large : font.large;
}

// Medium font for general use (lists, info)
static TTF_Font* get_font_medium(void) {
    return custom_font.loaded ? custom_font.artist : font.medium;
}

// Small font (badges, secondary text)
static TTF_Font* get_font_small(void) {
    return custom_font.loaded ? custom_font.badge : font.small;
}

// Tiny font (genre, bitrate)
static TTF_Font* get_font_tiny(void) {
    return custom_font.loaded ? custom_font.tiny : font.tiny;
}

// Format duration as MM:SS
static void format_time(char* buf, int ms) {
    int total_secs = ms / 1000;
    int mins = total_secs / 60;
    int secs = total_secs % 60;
    sprintf(buf, "%02d:%02d", mins, secs);
}

// Check if file is a supported audio format
static bool is_audio_file(const char* filename) {
    AudioFormat fmt = Player_detectFormat(filename);
    return fmt != AUDIO_FORMAT_UNKNOWN;
}

// Free browser entries
static void free_entries(void) {
    if (browser.entries) {
        free(browser.entries);
        browser.entries = NULL;
    }
    browser.entry_count = 0;
}

// Compare function for sorting entries (directories first, then alphabetical)
static int compare_entries(const void* a, const void* b) {
    const FileEntry* ea = (const FileEntry*)a;
    const FileEntry* eb = (const FileEntry*)b;

    // Directories come first
    if (ea->is_dir && !eb->is_dir) return -1;
    if (!ea->is_dir && eb->is_dir) return 1;

    // Alphabetical
    return strcasecmp(ea->name, eb->name);
}

// Load directory contents
static void load_directory(const char* path) {
    free_entries();

    strncpy(browser.current_path, path, sizeof(browser.current_path) - 1);
    browser.selected = 0;
    browser.scroll_offset = 0;

    // Create music folder if it doesn't exist
    if (strcmp(path, MUSIC_PATH) == 0) {
        mkdir(path, 0755);
    }

    DIR* dir = opendir(path);
    if (!dir) {
        LOG_error("Failed to open directory: %s\n", path);
        return;
    }

    // First pass: count entries
    int count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;  // Skip hidden files

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode) || is_audio_file(ent->d_name)) {
            count++;
        }
    }

    // Add parent directory entry if not at root
    bool has_parent = (strcmp(path, MUSIC_PATH) != 0);
    if (has_parent) count++;

    // Allocate
    browser.entries = malloc(sizeof(FileEntry) * count);
    if (!browser.entries) {
        closedir(dir);
        return;
    }

    int idx = 0;

    // Add parent directory
    if (has_parent) {
        strcpy(browser.entries[idx].name, "..");
        char* last_slash = strrchr(browser.current_path, '/');
        if (last_slash) {
            strncpy(browser.entries[idx].path, browser.current_path, last_slash - browser.current_path);
            browser.entries[idx].path[last_slash - browser.current_path] = '\0';
        } else {
            strcpy(browser.entries[idx].path, MUSIC_PATH);
        }
        browser.entries[idx].is_dir = true;
        browser.entries[idx].format = AUDIO_FORMAT_UNKNOWN;
        idx++;
    }

    // Second pass: fill entries
    rewinddir(dir);
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        bool is_dir = S_ISDIR(st.st_mode);
        AudioFormat fmt = AUDIO_FORMAT_UNKNOWN;

        if (!is_dir) {
            fmt = Player_detectFormat(ent->d_name);
            if (fmt == AUDIO_FORMAT_UNKNOWN) continue;
        }

        strncpy(browser.entries[idx].name, ent->d_name, sizeof(browser.entries[idx].name) - 1);
        strncpy(browser.entries[idx].path, full_path, sizeof(browser.entries[idx].path) - 1);
        browser.entries[idx].is_dir = is_dir;
        browser.entries[idx].format = fmt;
        idx++;
    }

    closedir(dir);
    browser.entry_count = idx;

    // Sort entries (but keep ".." at top if present)
    int sort_start = has_parent ? 1 : 0;
    if (browser.entry_count > sort_start + 1) {
        qsort(&browser.entries[sort_start], browser.entry_count - sort_start,
              sizeof(FileEntry), compare_entries);
    }
}

// Get display name for file (without extension)
static void get_display_name(const char* filename, char* out, int max_len) {
    strncpy(out, filename, max_len - 1);
    out[max_len - 1] = '\0';

    // Remove extension for audio files
    char* dot = strrchr(out, '.');
    if (dot && dot != out) {
        *dot = '\0';
    }
}

// Render the file browser
static void render_browser(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title pill
    const char* title = "Music Player";
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

    // File list
    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int list_h = hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE);
    browser.items_per_page = list_h / item_h;

    // Adjust scroll
    if (browser.selected < browser.scroll_offset) {
        browser.scroll_offset = browser.selected;
    }
    if (browser.selected >= browser.scroll_offset + browser.items_per_page) {
        browser.scroll_offset = browser.selected - browser.items_per_page + 1;
    }

    // Render items
    for (int i = 0; i < browser.items_per_page && browser.scroll_offset + i < browser.entry_count; i++) {
        int idx = browser.scroll_offset + i;
        FileEntry* entry = &browser.entries[idx];
        bool selected = (idx == browser.selected);

        int y = list_y + i * item_h;

        // Background pill
        if (selected) {
            SDL_Rect pill_rect = {SCALE1(PADDING), y, hw - SCALE1(PADDING * 2), item_h};
            GFX_blitPill(ASSET_WHITE_PILL, screen, &pill_rect);
        }

        // Icon or folder indicator
        char display[256];
        if (entry->is_dir) {
            snprintf(display, sizeof(display), "[%s]", entry->name);
        } else {
            get_display_name(entry->name, display, sizeof(display));
        }

        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, display, text_color);
        if (text) {
            int max_width = hw - SCALE1(PADDING * 4);
            SDL_Rect src = {0, 0, text->w > max_width ? max_width : text->w, text->h};
            SDL_BlitSurface(text, &src, screen, &(SDL_Rect){SCALE1(PADDING * 2), y + (item_h - text->h) / 2});
            SDL_FreeSurface(text);
        }
    }

    // Scroll indicators
    if (browser.entry_count > browser.items_per_page) {
        int ox = (hw - SCALE1(24)) / 2;
        if (browser.scroll_offset > 0) {
            GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE)});
        }
        if (browser.scroll_offset + browser.items_per_page < browser.entry_count) {
            GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE)});
        }
    }

    // Empty folder message
    if (browser.entry_count == 0) {
        const char* msg = "No music files found";
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_GRAY);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, hh / 2 - text->h / 2});
            SDL_FreeSurface(text);
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"U/D", "SCROLL", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "SELECT", NULL}, 1, screen, 1);
}

// Get format name string
static const char* get_format_name(AudioFormat format) {
    switch (format) {
        case AUDIO_FORMAT_MP3: return "MP3";
        case AUDIO_FORMAT_FLAC: return "FLAC";
        case AUDIO_FORMAT_OGG: return "OGG";
        case AUDIO_FORMAT_WAV: return "WAV";
        case AUDIO_FORMAT_MOD: return "MOD";
        default: return "---";
    }
}

// Count audio files in browser for "X OF Y" display
static int count_audio_files(void) {
    int count = 0;
    for (int i = 0; i < browser.entry_count; i++) {
        if (!browser.entries[i].is_dir) count++;
    }
    return count;
}

// Get current track number (1-based)
static int get_current_track_number(void) {
    int num = 0;
    for (int i = 0; i <= browser.selected && i < browser.entry_count; i++) {
        if (!browser.entries[i].is_dir) num++;
    }
    return num;
}

// Render the now playing screen
static void render_playing(int show_setting) {
    GFX_clear(screen);

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
    int track_num = get_current_track_number();
    int total_tracks = count_audio_files();
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

    // Max widths: artist/album = 50% screen, title = full width
    int max_w_half = (hw - SCALE1(PADDING * 2)) / 2;
    int max_w_full = hw - SCALE1(PADDING * 2);

    // Artist name (Medium font, gray) - max 50% width
    const char* artist = info->artist[0] ? info->artist : "Unknown Artist";
    GFX_truncateText(get_font_artist(), artist, truncated, max_w_half, 0);
    SDL_Surface* artist_surf = TTF_RenderUTF8_Blended(get_font_artist(), truncated, COLOR_GRAY);
    if (artist_surf) {
        SDL_BlitSurface(artist_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
        info_y += artist_surf->h + SCALE1(2);  // Same gap as title-album
        SDL_FreeSurface(artist_surf);
    } else {
        info_y += SCALE1(18);
    }

    // Song title (Regular font extra large, white) - full width
    const char* title = info->title[0] ? info->title : "Unknown Title";
    GFX_truncateText(get_font_title(), title, truncated, max_w_full, 0);
    SDL_Surface* title_surf = TTF_RenderUTF8_Blended(get_font_title(), truncated, COLOR_WHITE);
    if (title_surf) {
        SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
        info_y += title_surf->h + SCALE1(2);  // Smaller gap after title
        SDL_FreeSurface(title_surf);
    } else {
        info_y += SCALE1(32);
    }

    // Album name (Bold font smaller, gray) - max 50% width
    const char* album = info->album[0] ? info->album : "";
    if (album[0]) {
        GFX_truncateText(get_font_album(), album, truncated, max_w_half, 0);
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
static void render_quit_confirm(void) {
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
    SDL_Surface* msg_surf = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_WHITE);
    if (msg_surf) {
        SDL_BlitSurface(msg_surf, NULL, screen, &(SDL_Rect){(hw - msg_surf->w) / 2, box_y + SCALE1(20)});
        SDL_FreeSurface(msg_surf);
    }

    // Button hints
    const char* hint = "A: Yes   B: No";
    SDL_Surface* hint_surf = TTF_RenderUTF8_Blended(font.small, hint, COLOR_GRAY);
    if (hint_surf) {
        SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){(hw - hint_surf->w) / 2, box_y + SCALE1(55)});
        SDL_FreeSurface(hint_surf);
    }
}

// Render the main menu
static void render_menu(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "Music Player";
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

    // Menu items
    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE + BUTTON_MARGIN);

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int y = list_y + i * item_h;
        bool selected = (i == menu_selected);

        if (selected) {
            SDL_Rect pill_rect = {SCALE1(PADDING), y, hw - SCALE1(PADDING * 2), SCALE1(PILL_SIZE)};
            GFX_blitPill(ASSET_WHITE_PILL, screen, &pill_rect);
        }

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

        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, label, text_color);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){SCALE1(PADDING * 2), y + (SCALE1(PILL_SIZE) - text->h) / 2});
            SDL_FreeSurface(text);
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"U/D", "SELECT", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "EXIT", "A", "OPEN", NULL}, 1, screen, 1);
}

// Render the radio station list
static void render_radio_list(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "Internet Radio";
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

    // Station list
    RadioStation* stations;
    int station_count = Radio_getStations(&stations);

    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int list_h = hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE);
    int items_per_page = list_h / item_h;

    // Adjust scroll
    if (radio_selected < radio_scroll) {
        radio_scroll = radio_selected;
    }
    if (radio_selected >= radio_scroll + items_per_page) {
        radio_scroll = radio_selected - items_per_page + 1;
    }

    for (int i = 0; i < items_per_page && radio_scroll + i < station_count; i++) {
        int idx = radio_scroll + i;
        RadioStation* station = &stations[idx];
        bool selected = (idx == radio_selected);

        int y = list_y + i * item_h;

        if (selected) {
            SDL_Rect pill_rect = {SCALE1(PADDING), y, hw - SCALE1(PADDING * 2), item_h};
            GFX_blitPill(ASSET_WHITE_PILL, screen, &pill_rect);
        }

        // Station name
        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        SDL_Surface* name_text = TTF_RenderUTF8_Blended(font.medium, station->name, text_color);
        if (name_text) {
            int max_width = hw - SCALE1(PADDING * 4);
            SDL_Rect src = {0, 0, name_text->w > max_width ? max_width : name_text->w, name_text->h};
            SDL_BlitSurface(name_text, &src, screen, &(SDL_Rect){SCALE1(PADDING * 2), y + (item_h - name_text->h) / 2});
            SDL_FreeSurface(name_text);
        }

        // Genre (if available)
        if (station->genre[0]) {
            SDL_Color genre_color = selected ? COLOR_GRAY : COLOR_DARK_TEXT;
            SDL_Surface* genre_text = TTF_RenderUTF8_Blended(font.tiny, station->genre, genre_color);
            if (genre_text) {
                SDL_BlitSurface(genre_text, NULL, screen, &(SDL_Rect){hw - genre_text->w - SCALE1(PADDING * 2), y + (item_h - genre_text->h) / 2});
                SDL_FreeSurface(genre_text);
            }
        }
    }

    // Scroll indicators
    if (station_count > items_per_page) {
        int ox = (hw - SCALE1(24)) / 2;
        if (radio_scroll > 0) {
            GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE)});
        }
        if (radio_scroll + items_per_page < station_count) {
            GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE)});
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"Y", "MANAGE STATIONS", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "PLAY", NULL}, 1, screen, 1);
}

// Render the radio playing screen
static void render_radio_playing(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    RadioState state = Radio_getState();
    const RadioMetadata* meta = Radio_getMetadata();
    RadioStation* current_station = get_current_station();
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

    // Now Playing - Title on top (gray), Artist below (gray)
    if (meta->title[0]) {
        // Title line
        GFX_truncateText(get_font_album(), meta->title, truncated, max_w_full, 0);
        SDL_Surface* title_surf = TTF_RenderUTF8_Blended(get_font_album(), truncated, COLOR_GRAY);
        if (title_surf) {
            SDL_BlitSurface(title_surf, NULL, screen, &(SDL_Rect){SCALE1(PADDING), info_y});
            info_y += title_surf->h + SCALE1(2);
            SDL_FreeSurface(title_surf);
        }
    }
    if (meta->artist[0]) {
        // Artist line
        GFX_truncateText(get_font_artist(), meta->artist, truncated, max_w_full, 0);
        SDL_Surface* artist_surf = TTF_RenderUTF8_Blended(get_font_artist(), truncated, COLOR_GRAY);
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
        SDL_Surface* err_text = TTF_RenderUTF8_Blended(font.small, Radio_getError(), (SDL_Color){255, 100, 100, 255});
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
static void render_radio_add(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "Add Stations";
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

    // Subtitle
    const char* subtitle = "Select Country";
    SDL_Surface* sub_text = TTF_RenderUTF8_Blended(font.small, subtitle, COLOR_GRAY);
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
    if (add_country_selected < add_country_scroll) {
        add_country_scroll = add_country_selected;
    }
    if (add_country_selected >= add_country_scroll + items_per_page) {
        add_country_scroll = add_country_selected - items_per_page + 1;
    }

    for (int i = 0; i < items_per_page && add_country_scroll + i < country_count; i++) {
        int idx = add_country_scroll + i;
        const CuratedCountry* country = &countries[idx];
        bool selected = (idx == add_country_selected);

        int y = list_y + i * item_h;

        if (selected) {
            SDL_Rect pill_rect = {SCALE1(PADDING), y, hw - SCALE1(PADDING * 2), item_h};
            GFX_blitPill(ASSET_WHITE_PILL, screen, &pill_rect);
        }

        // Country name
        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        SDL_Surface* name_text = TTF_RenderUTF8_Blended(font.medium, country->name, text_color);
        if (name_text) {
            SDL_BlitSurface(name_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING * 2), y + (item_h - name_text->h) / 2});
            SDL_FreeSurface(name_text);
        }

        // Station count on right
        int station_count = Radio_getCuratedStationCount(country->code);
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "%d stations", station_count);
        SDL_Color count_color = selected ? COLOR_GRAY : COLOR_DARK_TEXT;
        SDL_Surface* count_text = TTF_RenderUTF8_Blended(font.tiny, count_str, count_color);
        if (count_text) {
            SDL_BlitSurface(count_text, NULL, screen, &(SDL_Rect){hw - count_text->w - SCALE1(PADDING * 2), y + (item_h - count_text->h) / 2});
            SDL_FreeSurface(count_text);
        }
    }

    // Scroll indicators
    if (country_count > items_per_page) {
        int ox = (hw - SCALE1(24)) / 2;
        if (add_country_scroll > 0) {
            GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE)});
        }
        if (add_country_scroll + items_per_page < country_count) {
            GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE)});
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"Y", "HELP", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"A", "SELECT", "B", "BACK", NULL}, 1, screen, 1);
}

// Render add stations - station selection screen
static void render_radio_add_stations(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Get country name for title
    const char* country_name = "Stations";
    const CuratedCountry* countries = Radio_getCuratedCountries();
    int country_count = Radio_getCuratedCountryCount();
    for (int i = 0; i < country_count; i++) {
        if (strcmp(countries[i].code, add_selected_country_code) == 0) {
            country_name = countries[i].name;
            break;
        }
    }

    // Title
    int title_width = GFX_truncateText(font.medium, country_name, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
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

    // Get stations for selected country
    int station_count = 0;
    const CuratedStation* stations = Radio_getCuratedStations(add_selected_country_code, &station_count);

    // Count selected stations
    int selected_count = 0;
    for (int i = 0; i < station_count && i < 256; i++) {
        if (add_station_checked[i]) selected_count++;
    }

    // Subtitle with selection count
    char subtitle[64];
    snprintf(subtitle, sizeof(subtitle), "%d selected", selected_count);
    SDL_Surface* sub_text = TTF_RenderUTF8_Blended(font.small, subtitle, COLOR_GRAY);
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
    if (add_station_selected < add_station_scroll) {
        add_station_scroll = add_station_selected;
    }
    if (add_station_selected >= add_station_scroll + items_per_page) {
        add_station_scroll = add_station_selected - items_per_page + 1;
    }

    for (int i = 0; i < items_per_page && add_station_scroll + i < station_count; i++) {
        int idx = add_station_scroll + i;
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
        SDL_Surface* cb_text = TTF_RenderUTF8_Blended(font.small, checkbox, cb_color);
        int cb_width = 0;
        if (cb_text) {
            SDL_BlitSurface(cb_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING * 2), y + (item_h - cb_text->h) / 2});
            cb_width = cb_text->w + SCALE1(6);
            SDL_FreeSurface(cb_text);
        }

        // Station name
        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        SDL_Surface* name_text = TTF_RenderUTF8_Blended(font.medium, station->name, text_color);
        if (name_text) {
            int max_width = hw - SCALE1(PADDING * 4) - cb_width - SCALE1(60);
            SDL_Rect src = {0, 0, name_text->w > max_width ? max_width : name_text->w, name_text->h};
            SDL_BlitSurface(name_text, &src, screen, &(SDL_Rect){SCALE1(PADDING * 2) + cb_width, y + (item_h - name_text->h) / 2});
            SDL_FreeSurface(name_text);
        }

        // Genre on right
        if (station->genre[0]) {
            SDL_Color genre_color = selected ? COLOR_GRAY : COLOR_DARK_TEXT;
            SDL_Surface* genre_text = TTF_RenderUTF8_Blended(font.tiny, station->genre, genre_color);
            if (genre_text) {
                SDL_BlitSurface(genre_text, NULL, screen, &(SDL_Rect){hw - genre_text->w - SCALE1(PADDING * 2), y + (item_h - genre_text->h) / 2});
                SDL_FreeSurface(genre_text);
            }
        }
    }

    // Scroll indicators
    if (station_count > items_per_page) {
        int ox = (hw - SCALE1(24)) / 2;
        if (add_station_scroll > 0) {
            GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING + PILL_SIZE)});
        }
        if (add_station_scroll + items_per_page < station_count) {
            GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE)});
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"X", "SAVE", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"A", "TOGGLE", "B", "BACK", NULL}, 1, screen, 1);
}

// Render help/instructions screen
static void render_radio_help(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "How to Add Stations";
    int title_width = GFX_truncateText(font.medium, title, truncated, hw - SCALE1(PADDING * 4), SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), title_width, SCALE1(PILL_SIZE)});

    SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.medium, truncated, COLOR_GRAY);
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
        "- MP3 and AAC formats supported",
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
    if (help_scroll > max_scroll) help_scroll = max_scroll;
    if (help_scroll < 0) help_scroll = 0;

    // Render lines with scroll offset
    int text_y = content_start_y - help_scroll;
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
        TTF_Font* use_font = font.small;

        // Highlight special lines
        if (strstr(lines[i], "Example:") || strstr(lines[i], "Notes:")) {
            color = COLOR_GRAY;
        } else if (lines[i][0] == '-') {
            color = COLOR_GRAY;
            use_font = font.tiny;
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
        if (help_scroll > 0) {
            GFX_blitAsset(ASSET_SCROLL_UP, NULL, screen, &(SDL_Rect){ox, content_start_y - SCALE1(12)});
        }
        if (help_scroll < max_scroll) {
            GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, hh - button_area_h - SCALE1(16)});
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
}

// Render YouTube sub-menu
static void render_youtube_menu(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "MP3 Downloader";
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

    // Version info
    char version_str[64];
    snprintf(version_str, sizeof(version_str), "yt-dlp: %s", YouTube_getVersion());
    SDL_Surface* ver_text = TTF_RenderUTF8_Blended(font.tiny, version_str, COLOR_GRAY);
    if (ver_text) {
        SDL_BlitSurface(ver_text, NULL, screen, &(SDL_Rect){hw - ver_text->w - SCALE1(PADDING), SCALE1(PADDING + 8)});
        SDL_FreeSurface(ver_text);
    }

    // Menu items
    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE + BUTTON_MARGIN);

    for (int i = 0; i < YOUTUBE_MENU_COUNT; i++) {
        int y = list_y + i * item_h;
        bool selected = (i == youtube_menu_selected);

        if (selected) {
            SDL_Rect pill_rect = {SCALE1(PADDING), y, hw - SCALE1(PADDING * 2), SCALE1(PILL_SIZE)};
            GFX_blitPill(ASSET_WHITE_PILL, screen, &pill_rect);
        }

        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.large, youtube_menu_items[i], text_color);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){SCALE1(PADDING * 2), y + (SCALE1(PILL_SIZE) - text->h) / 2});
            SDL_FreeSurface(text);
        }

        // Show queue count next to Download Queue
        if (i == 1) {
            int qcount = YouTube_queueCount();
            if (qcount > 0) {
                char count_str[16];
                snprintf(count_str, sizeof(count_str), "(%d)", qcount);
                SDL_Surface* count_text = TTF_RenderUTF8_Blended(font.small, count_str, selected ? COLOR_BLACK : COLOR_GRAY);
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
static void render_youtube_searching(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    // Title
    const char* title = "Music Downloader";
    SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){(hw - title_text->w) / 2, SCALE1(PADDING * 2)});
        SDL_FreeSurface(title_text);
    }

    // Searching message
    char search_msg[300];
    snprintf(search_msg, sizeof(search_msg), "Searching for: %s", youtube_search_query);
    SDL_Surface* query_text = TTF_RenderUTF8_Blended(font.medium, search_msg, COLOR_GRAY);
    if (query_text) {
        int qx = (hw - query_text->w) / 2;
        if (qx < SCALE1(PADDING)) qx = SCALE1(PADDING);
        SDL_BlitSurface(query_text, NULL, screen, &(SDL_Rect){qx, hh / 2 - SCALE1(30)});
        SDL_FreeSurface(query_text);
    }

    // Loading indicator
    const char* loading = "Please wait...";
    SDL_Surface* load_text = TTF_RenderUTF8_Blended(font.medium, loading, COLOR_WHITE);
    if (load_text) {
        SDL_BlitSurface(load_text, NULL, screen, &(SDL_Rect){(hw - load_text->w) / 2, hh / 2 + SCALE1(10)});
        SDL_FreeSurface(load_text);
    }

    // No button hints during search - it's blocking
}

// Render YouTube search results
static void render_youtube_results(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    char title[128];
    snprintf(title, sizeof(title), "Results: %s", youtube_search_query);
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

    // Results list
    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int list_h = hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE);
    int items_per_page = list_h / item_h;

    // Adjust scroll
    if (youtube_results_selected < youtube_results_scroll) {
        youtube_results_scroll = youtube_results_selected;
    }
    if (youtube_results_selected >= youtube_results_scroll + items_per_page) {
        youtube_results_scroll = youtube_results_selected - items_per_page + 1;
    }

    for (int i = 0; i < items_per_page && youtube_results_scroll + i < youtube_result_count; i++) {
        int idx = youtube_results_scroll + i;
        YouTubeResult* result = &youtube_results[idx];
        bool selected = (idx == youtube_results_selected);
        bool in_queue = YouTube_isInQueue(result->video_id);

        int y = list_y + i * item_h;

        if (selected) {
            SDL_Rect pill_rect = {SCALE1(PADDING), y, hw - SCALE1(PADDING * 2), item_h};
            GFX_blitPill(ASSET_WHITE_PILL, screen, &pill_rect);
        }

        int title_x = SCALE1(PADDING * 2);

        // Show indicator if already in queue
        if (in_queue) {
            SDL_Surface* indicator = TTF_RenderUTF8_Blended(font.tiny, "[+]", selected ? COLOR_BLACK : COLOR_GRAY);
            if (indicator) {
                SDL_BlitSurface(indicator, NULL, screen, &(SDL_Rect){title_x, y + (item_h - indicator->h) / 2});
                title_x += indicator->w + SCALE1(4);
                SDL_FreeSurface(indicator);
            }
        }

        // Title
        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        GFX_truncateText(font.medium, result->title, truncated, hw - title_x - SCALE1(PADDING * 4), 0);
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, truncated, text_color);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){title_x, y + (item_h - text->h) / 2});
            SDL_FreeSurface(text);
        }

        // Duration
        if (result->duration_sec > 0) {
            char dur[16];
            int m = result->duration_sec / 60;
            int s = result->duration_sec % 60;
            snprintf(dur, sizeof(dur), "%d:%02d", m, s);
            SDL_Surface* dur_text = TTF_RenderUTF8_Blended(font.tiny, dur, selected ? COLOR_BLACK : COLOR_GRAY);
            if (dur_text) {
                SDL_BlitSurface(dur_text, NULL, screen, &(SDL_Rect){hw - dur_text->w - SCALE1(PADDING * 2), y + (item_h - dur_text->h) / 2});
                SDL_FreeSurface(dur_text);
            }
        }
    }

    // Empty results message
    if (youtube_result_count == 0) {
        const char* msg = youtube_searching ? "Searching..." : "No results found";
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_GRAY);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){(hw - text->w) / 2, hh / 2 - text->h / 2});
            SDL_FreeSurface(text);
        }
    }

    // Toast notification
    if (youtube_toast_message[0] != '\0') {
        uint32_t now = SDL_GetTicks();
        if (now - youtube_toast_time < YOUTUBE_TOAST_DURATION) {
            // Draw toast at bottom center
            SDL_Surface* toast_text = TTF_RenderUTF8_Blended(font.medium, youtube_toast_message, COLOR_WHITE);
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
        } else {
            youtube_toast_message[0] = '\0';  // Clear toast
        }
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"U/D", "SCROLL", NULL}, 0, screen, 0);

    // Dynamic hint based on queue status
    const char* action_hint = "ADD";
    if (youtube_result_count > 0) {
        YouTubeResult* selected_result = &youtube_results[youtube_results_selected];
        if (YouTube_isInQueue(selected_result->video_id)) {
            action_hint = "REMOVE";
        }
    }
    GFX_blitButtonGroup((char*[]){"A", (char*)action_hint, "B", "BACK", NULL}, 1, screen, 1);
}

// Render YouTube download queue
static void render_youtube_queue(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;
    char truncated[256];

    // Title
    const char* title = "Download Queue";
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

    // Queue list
    int qcount = 0;
    YouTubeQueueItem* queue = YouTube_queueGet(&qcount);

    int list_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int list_h = hh - list_y - SCALE1(PADDING + BUTTON_SIZE + BUTTON_MARGIN);
    int item_h = SCALE1(PILL_SIZE);
    int items_per_page = list_h / item_h;

    // Adjust scroll
    if (youtube_queue_selected < youtube_queue_scroll) {
        youtube_queue_scroll = youtube_queue_selected;
    }
    if (youtube_queue_selected >= youtube_queue_scroll + items_per_page) {
        youtube_queue_scroll = youtube_queue_selected - items_per_page + 1;
    }

    for (int i = 0; i < items_per_page && youtube_queue_scroll + i < qcount; i++) {
        int idx = youtube_queue_scroll + i;
        YouTubeQueueItem* item = &queue[idx];
        bool selected = (idx == youtube_queue_selected);

        int y = list_y + i * item_h;

        if (selected) {
            SDL_Rect pill_rect = {SCALE1(PADDING), y, hw - SCALE1(PADDING * 2), item_h};
            GFX_blitPill(ASSET_WHITE_PILL, screen, &pill_rect);
        }

        // Status indicator (only for non-pending items)
        const char* status_str = NULL;
        SDL_Color status_color = COLOR_GRAY;
        switch (item->status) {
            case YOUTUBE_STATUS_PENDING: status_str = NULL; break;  // No prefix for pending
            case YOUTUBE_STATUS_DOWNLOADING: status_str = NULL; break;  // Show progress bar instead
            case YOUTUBE_STATUS_COMPLETE: status_str = "[OK]"; break;
            case YOUTUBE_STATUS_FAILED: status_str = "[X]"; break;
        }

        int title_x = SCALE1(PADDING * 2);  // Default position
        if (status_str) {
            SDL_Surface* status_text = TTF_RenderUTF8_Blended(font.tiny, status_str, selected ? COLOR_BLACK : status_color);
            if (status_text) {
                SDL_BlitSurface(status_text, NULL, screen, &(SDL_Rect){SCALE1(PADDING * 2), y + (item_h - status_text->h) / 2});
                title_x = SCALE1(PADDING * 2) + status_text->w + SCALE1(8);
                SDL_FreeSurface(status_text);
            }
        }

        // Title
        SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
        int title_max_w = hw - title_x - SCALE1(PADDING * 2);

        // Reserve space for progress bar if downloading
        if (item->status == YOUTUBE_STATUS_DOWNLOADING) {
            title_max_w -= SCALE1(80);  // Reserve space for progress bar
        }

        GFX_truncateText(font.medium, item->title, truncated, title_max_w, 0);
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, truncated, text_color);
        if (text) {
            SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){title_x, y + (item_h - text->h) / 2});
            SDL_FreeSurface(text);
        }

        // Progress bar for downloading items
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
            SDL_Surface* pct_text = TTF_RenderUTF8_Blended(font.tiny, pct_str, selected ? COLOR_BLACK : COLOR_WHITE);
            if (pct_text) {
                SDL_BlitSurface(pct_text, NULL, screen, &(SDL_Rect){bar_x - pct_text->w - SCALE1(4), y + (item_h - pct_text->h) / 2});
                SDL_FreeSurface(pct_text);
            }
        }
    }

    // Empty queue message
    if (qcount == 0) {
        const char* msg = "Queue is empty";
        SDL_Surface* text = TTF_RenderUTF8_Blended(font.medium, msg, COLOR_GRAY);
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
static void render_youtube_downloading(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

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

    // Title
    const char* title = "Downloading...";
    SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){(hw - title_text->w) / 2, SCALE1(PADDING * 2)});
        SDL_FreeSurface(title_text);
    }

    // Progress info
    char progress[128];
    snprintf(progress, sizeof(progress), "%d / %d completed", status->completed_count, status->total_items);
    SDL_Surface* prog_text = TTF_RenderUTF8_Blended(font.medium, progress, COLOR_GRAY);
    if (prog_text) {
        SDL_BlitSurface(prog_text, NULL, screen, &(SDL_Rect){(hw - prog_text->w) / 2, hh / 2 - SCALE1(50)});
        SDL_FreeSurface(prog_text);
    }

    // Current track
    if (strlen(status->current_title) > 0) {
        char truncated[256];
        GFX_truncateText(font.small, status->current_title, truncated, hw - SCALE1(PADDING * 4), 0);
        SDL_Surface* curr_text = TTF_RenderUTF8_Blended(font.small, truncated, COLOR_WHITE);
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
    SDL_Surface* pct_text = TTF_RenderUTF8_Blended(font.medium, pct_str, COLOR_WHITE);
    if (pct_text) {
        SDL_BlitSurface(pct_text, NULL, screen, &(SDL_Rect){(hw - pct_text->w) / 2, bar_y + bar_h + SCALE1(8)});
        SDL_FreeSurface(pct_text);
    }

    // Button hints
    GFX_blitButtonGroup((char*[]){"B", "CANCEL", NULL}, 1, screen, 1);
}

// Render YouTube yt-dlp update progress
static void render_youtube_updating(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    const YouTubeUpdateStatus* status = YouTube_getUpdateStatus();

    // Title
    const char* title = "Updating yt-dlp";
    SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){(hw - title_text->w) / 2, SCALE1(PADDING * 2)});
        SDL_FreeSurface(title_text);
    }

    // Current version
    char ver_str[128];
    snprintf(ver_str, sizeof(ver_str), "Current: %s", status->current_version);
    SDL_Surface* ver_text = TTF_RenderUTF8_Blended(font.medium, ver_str, COLOR_GRAY);
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

    SDL_Surface* status_text = TTF_RenderUTF8_Blended(font.medium, status_msg, COLOR_WHITE);
    if (status_text) {
        SDL_BlitSurface(status_text, NULL, screen, &(SDL_Rect){(hw - status_text->w) / 2, hh / 2});
        SDL_FreeSurface(status_text);
    }

    // Latest version (if known)
    if (strlen(status->latest_version) > 0) {
        snprintf(ver_str, sizeof(ver_str), "Latest: %s", status->latest_version);
        SDL_Surface* latest_text = TTF_RenderUTF8_Blended(font.small, ver_str, COLOR_GRAY);
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

// Render the app update screen
static void render_app_updating(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    const SelfUpdateStatus* status = SelfUpdate_getStatus();
    SelfUpdateState state = status->state;

    // Title - "App Update"
    const char* title = "App Update";
    SDL_Surface* title_text = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
    if (title_text) {
        SDL_BlitSurface(title_text, NULL, screen, &(SDL_Rect){(hw - title_text->w) / 2, SCALE1(PADDING * 3)});
        SDL_FreeSurface(title_text);
    }

    // Version info: "v0.1.0 → v0.2.0"
    char ver_str[128];
    if (strlen(status->latest_version) > 0) {
        snprintf(ver_str, sizeof(ver_str), "v%s  ->  %s", status->current_version, status->latest_version);
    } else {
        snprintf(ver_str, sizeof(ver_str), "v%s", status->current_version);
    }
    SDL_Surface* ver_text = TTF_RenderUTF8_Blended(font.medium, ver_str, COLOR_GRAY);
    if (ver_text) {
        SDL_BlitSurface(ver_text, NULL, screen, &(SDL_Rect){(hw - ver_text->w) / 2, SCALE1(PADDING * 3 + 35)});
        SDL_FreeSurface(ver_text);
    }

    // Release notes area with word wrapping
    int notes_y = hh / 2 - SCALE1(30);
    int notes_max_lines = 4;
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

        char wrapped_lines[4][128];
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
                TTF_SizeUTF8(font.small, test_line, &text_w, &text_h);
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
                SDL_Surface* line_text = TTF_RenderUTF8_Blended(font.small, wrapped_lines[i], COLOR_WHITE);
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
static void render_about(int show_setting) {
    GFX_clear(screen);

    int hw = screen->w;
    int hh = screen->h;

    // Hardware status
    if (hw >= SCALE1(320)) {
        GFX_blitHardwareGroup(screen, show_setting);
    }

    // App name
    const char* app_name = "NextUI Music Player";
    SDL_Surface* name_text = TTF_RenderUTF8_Blended(font.large, app_name, COLOR_WHITE);
    if (name_text) {
        SDL_BlitSurface(name_text, NULL, screen, &(SDL_Rect){(hw - name_text->w) / 2, SCALE1(PADDING * 3)});
        SDL_FreeSurface(name_text);
    }

    // Version
    char version_str[64];
    snprintf(version_str, sizeof(version_str), "Version %s", SelfUpdate_getVersion());
    SDL_Surface* ver_text = TTF_RenderUTF8_Blended(font.medium, version_str, COLOR_GRAY);
    if (ver_text) {
        SDL_BlitSurface(ver_text, NULL, screen, &(SDL_Rect){(hw - ver_text->w) / 2, SCALE1(PADDING * 3 + 35)});
        SDL_FreeSurface(ver_text);
    }

    // Description
    int info_y = hh / 2 - SCALE1(30);
    const char* desc_lines[] = {
        "Local music playback",
        "Internet radio streaming",
        "YouTube music downloads",
        NULL
    };

    for (int i = 0; desc_lines[i] != NULL; i++) {
        SDL_Surface* line_text = TTF_RenderUTF8_Blended(font.small, desc_lines[i], COLOR_WHITE);
        if (line_text) {
            SDL_BlitSurface(line_text, NULL, screen, &(SDL_Rect){(hw - line_text->w) / 2, info_y + i * SCALE1(20)});
            SDL_FreeSurface(line_text);
        }
    }

    // GitHub URL
    const char* github_url = "github.com/mohammadsyuhada/nextui-music-player";
    SDL_Surface* url_text = TTF_RenderUTF8_Blended(font.tiny, github_url, COLOR_GRAY);
    if (url_text) {
        SDL_BlitSurface(url_text, NULL, screen, &(SDL_Rect){(hw - url_text->w) / 2, hh - SCALE1(PILL_SIZE + PADDING * 3) - url_text->h});
        SDL_FreeSurface(url_text);
    }

    // Show update available message if there's an update
    const SelfUpdateStatus* status = SelfUpdate_getStatus();
    if (status->update_available) {
        char update_msg[128];
        snprintf(update_msg, sizeof(update_msg), "Update available: %s", status->latest_version);
        SDL_Surface* update_text = TTF_RenderUTF8_Blended(font.small, update_msg, (SDL_Color){100, 255, 100, 255});
        if (update_text) {
            SDL_BlitSurface(update_text, NULL, screen, &(SDL_Rect){(hw - update_text->w) / 2, hh - SCALE1(PILL_SIZE + PADDING * 5) - update_text->h});
            SDL_FreeSurface(update_text);
        }
    }

    // Button hints - show UPDATE button if update available
    if (status->update_available) {
        GFX_blitButtonGroup((char*[]){"A", "UPDATE", "B", "BACK", NULL}, 1, screen, 1);
    } else {
        GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
    }
}

int main(int argc, char* argv[]) {
    InitSettings();
    PWR_setCPUSpeed(CPU_SPEED_MENU);
    screen = GFX_init(MODE_MAIN);
    PAD_init();
    PWR_init();
    WIFI_init();

    // Load custom fonts (if available)
    load_custom_fonts();

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    // Seed random number generator for shuffle
    srand((unsigned int)time(NULL));

    // Initialize player and radio
    if (Player_init() != 0) {
        LOG_error("Failed to initialize audio player\n");
        goto cleanup;
    }
    // Sync software volume with system volume at startup
    Player_setVolume(GetVolume() / 100.0f);

    Radio_init();
    YouTube_init();

    // Initialize self-update module (current directory is pak root)
    // Version is read from state/app_version.txt
    SelfUpdate_init(".");

    // Auto-check for updates on startup (non-blocking)
    SelfUpdate_checkForUpdate();

    // Create Music folder if it doesn't exist
    mkdir(MUSIC_PATH, 0755);

    // Load initial directory
    load_directory(MUSIC_PATH);

    int dirty = 1;
    int show_setting = 0;

    while (!quit) {
        uint32_t frame_start = SDL_GetTicks();
        PAD_poll();

        // Handle volume buttons - works in all states
        if (PAD_justRepeated(BTN_PLUS)) {
            // Get current volume from software volume (0.0-1.0) and convert to 0-100
            int vol = (int)(Player_getVolume() * 100.0f + 0.5f);
            vol = (vol < 100) ? vol + 5 : 100;
            // Skip SetVolume() for Bluetooth as it can block
            if (!Player_isBluetoothActive()) {
                SetVolume(vol);
            }
            // Apply software volume immediately (works for all output devices)
            Player_setVolume(vol / 100.0f);
        }
        else if (PAD_justRepeated(BTN_MINUS)) {
            // Get current volume from software volume (0.0-1.0) and convert to 0-100
            int vol = (int)(Player_getVolume() * 100.0f + 0.5f);
            vol = (vol > 0) ? vol - 5 : 0;
            // Skip SetVolume() for Bluetooth as it can block
            if (!Player_isBluetoothActive()) {
                SetVolume(vol);
            }
            // Apply software volume immediately (works for all output devices)
            Player_setVolume(vol / 100.0f);
        }

        // Handle quit confirmation dialog
        if (show_quit_confirm) {
            if (PAD_justPressed(BTN_A)) {
                // Confirm quit
                quit = true;
            }
            else if (PAD_justPressed(BTN_B) || PAD_justPressed(BTN_START)) {
                // Cancel quit
                show_quit_confirm = false;
                dirty = 1;
            }
            // Skip other input handling while dialog is shown
        }
        // Handle START button to show quit confirmation
        else if (PAD_justPressed(BTN_START)) {
            show_quit_confirm = true;
            dirty = 1;
        }
        // Handle input based on state
        else if (app_state == STATE_MENU) {
            if (PAD_justRepeated(BTN_UP)) {
                if (menu_selected > 0) {
                    menu_selected--;
                    dirty = 1;
                }
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                if (menu_selected < MENU_ITEM_COUNT - 1) {
                    menu_selected++;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_A)) {
                if (menu_selected == 0) {
                    // Music Files - reload directory to pick up new downloads
                    load_directory(browser.current_path[0] ? browser.current_path : MUSIC_PATH);
                    app_state = STATE_BROWSER;
                    dirty = 1;
                } else if (menu_selected == 1) {
                    // Internet Radio
                    app_state = STATE_RADIO_LIST;
                    dirty = 1;
                } else if (menu_selected == 2) {
                    // Music Downloader
                    if (YouTube_isAvailable()) {
                        app_state = STATE_YOUTUBE_MENU;
                        youtube_menu_selected = 0;
                        dirty = 1;
                    }
                } else if (menu_selected == 3) {
                    // About
                    app_state = STATE_ABOUT;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_B)) {
                quit = true;
            }
        }
        else if (app_state == STATE_BROWSER) {
            if (PAD_justRepeated(BTN_UP)) {
                if (browser.selected > 0) {
                    browser.selected--;
                    dirty = 1;
                }
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                if (browser.selected < browser.entry_count - 1) {
                    browser.selected++;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_A) && browser.entry_count > 0) {
                FileEntry* entry = &browser.entries[browser.selected];
                if (entry->is_dir) {
                    // Copy path before load_directory frees browser.entries
                    char path_copy[512];
                    strncpy(path_copy, entry->path, sizeof(path_copy) - 1);
                    path_copy[sizeof(path_copy) - 1] = '\0';
                    load_directory(path_copy);
                    dirty = 1;
                } else {
                    // Load and play the file
                    if (Player_load(entry->path) == 0) {
                        Player_play();
                                                app_state = STATE_PLAYING;
                        last_input_time = SDL_GetTicks();  // Start screen-off timer
                        dirty = 1;
                    }
                }
            }
            else if (PAD_justPressed(BTN_B)) {
                // Go up a directory or back to menu
                if (strcmp(browser.current_path, MUSIC_PATH) != 0) {
                    char* last_slash = strrchr(browser.current_path, '/');
                    if (last_slash) {
                        *last_slash = '\0';
                        load_directory(browser.current_path);
                        dirty = 1;
                    }
                } else {
                    app_state = STATE_MENU;
                    dirty = 1;
                }
            }
        }
        else if (app_state == STATE_PLAYING) {
            // Disable autosleep while playing
            if (!autosleep_disabled) {
                PWR_disableAutosleep();
                autosleep_disabled = true;
            }

            // Handle screen off mode - any button wakes screen
            if (screen_off) {
                if (PAD_anyPressed()) {
                    screen_off = false;
                    PLAT_enableBacklight(1);
                    last_input_time = SDL_GetTicks();  // Reset timer on wake
                    dirty = 1;
                }
                // Still update player and process audio while screen is off
                Player_update();

                // Check if track ended while screen off
                if (Player_getState() == PLAYER_STATE_STOPPED) {
                    bool found_next = false;

                    if (repeat_enabled) {
                        // Repeat current track
                        if (Player_load(browser.entries[browser.selected].path) == 0) {
                            Player_play();
                            found_next = true;
                        }
                    } else if (shuffle_enabled) {
                        // Pick a random track
                        int audio_count = count_audio_files();
                        if (audio_count > 1) {
                            int random_idx = rand() % audio_count;
                            int count = 0;
                            for (int i = 0; i < browser.entry_count; i++) {
                                if (!browser.entries[i].is_dir) {
                                    if (count == random_idx && i != browser.selected) {
                                        browser.selected = i;
                                        if (Player_load(browser.entries[i].path) == 0) {
                                            Player_play();
                                            found_next = true;
                                        }
                                        break;
                                    }
                                    count++;
                                }
                            }
                            // Fallback if we picked same track
                            if (!found_next) {
                                for (int i = 0; i < browser.entry_count; i++) {
                                    if (!browser.entries[i].is_dir && i != browser.selected) {
                                        browser.selected = i;
                                        if (Player_load(browser.entries[i].path) == 0) {
                                            Player_play();
                                            found_next = true;
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    } else {
                        // Normal: advance to next track
                        for (int i = browser.selected + 1; i < browser.entry_count; i++) {
                            if (!browser.entries[i].is_dir) {
                                browser.selected = i;
                                if (Player_load(browser.entries[i].path) == 0) {
                                    Player_play();
                                                                        found_next = true;
                                }
                                break;
                            }
                        }
                    }

                    // If no next track, wake screen and go back
                    if (!found_next && Player_getState() == PLAYER_STATE_STOPPED) {
                        screen_off = false;
                        PLAT_enableBacklight(1);
                        app_state = STATE_BROWSER;
                        if (autosleep_disabled) {
                            PWR_enableAutosleep();
                            autosleep_disabled = false;
                        }
                        dirty = 1;
                    }
                }
            }
            else {
                // Normal input handling when screen is on
                // Reset input timer on any button press
                if (PAD_anyPressed()) {
                    last_input_time = SDL_GetTicks();
                }

                if (PAD_justPressed(BTN_A)) {
                    Player_togglePause();
                    dirty = 1;
                }
                else if (PAD_justPressed(BTN_B)) {
                    Player_stop();
                    app_state = STATE_BROWSER;
                    // Re-enable autosleep when leaving playing state
                    if (autosleep_disabled) {
                        PWR_enableAutosleep();
                        autosleep_disabled = false;
                    }
                    dirty = 1;
                }
                else if (PAD_justRepeated(BTN_LEFT)) {
                    // Seek backward 5 seconds
                    int pos = Player_getPosition();
                    Player_seek(pos - 5000);
                    dirty = 1;
                }
                else if (PAD_justRepeated(BTN_RIGHT)) {
                    // Seek forward 5 seconds
                    int pos = Player_getPosition();
                    Player_seek(pos + 5000);
                    dirty = 1;
                }
                else if (PAD_justPressed(BTN_DOWN) || PAD_justPressed(BTN_L1)) {
                    // Previous track (Down or L1)
                    for (int i = browser.selected - 1; i >= 0; i--) {
                        if (!browser.entries[i].is_dir) {
                            Player_stop();
                            browser.selected = i;
                            if (Player_load(browser.entries[i].path) == 0) {
                                Player_play();
                            }
                            dirty = 1;
                            break;
                        }
                    }
                }
                else if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_R1)) {
                    // Next track (Up or R1)
                    for (int i = browser.selected + 1; i < browser.entry_count; i++) {
                        if (!browser.entries[i].is_dir) {
                            Player_stop();
                            browser.selected = i;
                            if (Player_load(browser.entries[i].path) == 0) {
                                Player_play();
                                                            }
                            dirty = 1;
                            break;
                        }
                    }
                }
                else if (PAD_justPressed(BTN_X)) {
                    // Toggle shuffle
                    shuffle_enabled = !shuffle_enabled;
                    dirty = 1;
                }
                else if (PAD_justPressed(BTN_Y)) {
                    // Toggle repeat
                    repeat_enabled = !repeat_enabled;
                    dirty = 1;
                }

                // Check if track ended (only if still in playing state - not if user pressed back)
                if (app_state == STATE_PLAYING) {
                    Player_update();
                    if (Player_getState() == PLAYER_STATE_STOPPED) {
                        bool found_next = false;

                        if (repeat_enabled) {
                            // Repeat current track
                            if (Player_load(browser.entries[browser.selected].path) == 0) {
                                Player_play();
                                found_next = true;
                            }
                        } else if (shuffle_enabled) {
                            // Pick a random track
                            int audio_count = count_audio_files();
                            if (audio_count > 1) {
                                int random_idx = rand() % audio_count;
                                int count = 0;
                                for (int i = 0; i < browser.entry_count; i++) {
                                    if (!browser.entries[i].is_dir) {
                                        if (count == random_idx && i != browser.selected) {
                                            browser.selected = i;
                                            if (Player_load(browser.entries[i].path) == 0) {
                                                Player_play();
                                                found_next = true;
                                            }
                                            break;
                                        }
                                        count++;
                                    }
                                }
                                // Fallback if we picked same track
                                if (!found_next) {
                                    for (int i = 0; i < browser.entry_count; i++) {
                                        if (!browser.entries[i].is_dir && i != browser.selected) {
                                            browser.selected = i;
                                            if (Player_load(browser.entries[i].path) == 0) {
                                                Player_play();
                                                found_next = true;
                                            }
                                            break;
                                        }
                                    }
                                }
                            }
                        } else {
                            // Normal: advance to next track
                            for (int i = browser.selected + 1; i < browser.entry_count; i++) {
                                if (!browser.entries[i].is_dir) {
                                    browser.selected = i;
                                    if (Player_load(browser.entries[i].path) == 0) {
                                        Player_play();
                                                                                found_next = true;
                                    }
                                    break;
                                }
                            }
                        }

                        dirty = 1;

                        // If no next track, go back to browser
                        if (!found_next && Player_getState() == PLAYER_STATE_STOPPED) {
                            app_state = STATE_BROWSER;
                            if (autosleep_disabled) {
                                PWR_enableAutosleep();
                                autosleep_disabled = false;
                            }
                        }
                    }

                    // Auto screen-off after inactivity (only while playing)
                    if (Player_getState() == PLAYER_STATE_PLAYING) {
                        uint32_t screen_timeout_ms = CFG_getScreenTimeoutSecs() * 1000;
                        if (screen_timeout_ms > 0 && last_input_time > 0) {
                            uint32_t now = SDL_GetTicks();
                            if (now - last_input_time >= screen_timeout_ms) {
                                screen_off = true;
                                PLAT_enableBacklight(0);
                            }
                        }
                    }

                    // Always redraw in playing state for visualization
                    dirty = 1;
                }
            }
        }
        else if (app_state == STATE_RADIO_LIST) {
            RadioStation* stations;
            int station_count = Radio_getStations(&stations);

            if (PAD_justRepeated(BTN_UP)) {
                if (radio_selected > 0) {
                    radio_selected--;
                    dirty = 1;
                }
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                if (radio_selected < station_count - 1) {
                    radio_selected++;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_A) && station_count > 0) {
                // Start playing the selected station
                if (Radio_play(stations[radio_selected].url) == 0) {
                    app_state = STATE_RADIO_PLAYING;
                    last_input_time = SDL_GetTicks();  // Start screen-off timer
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_B)) {
                app_state = STATE_MENU;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_Y)) {
                // Open Add Stations screen
                add_country_selected = 0;
                add_country_scroll = 0;
                app_state = STATE_RADIO_ADD;
                dirty = 1;
            }
        }
        else if (app_state == STATE_RADIO_PLAYING) {
            // Disable autosleep while playing radio
            if (!autosleep_disabled) {
                PWR_disableAutosleep();
                autosleep_disabled = true;
            }

            // Handle screen off mode - any button wakes screen
            if (screen_off) {
                if (PAD_anyPressed()) {
                    screen_off = false;
                    PLAT_enableBacklight(1);
                    last_input_time = SDL_GetTicks();  // Reset timer on wake
                    dirty = 1;
                }
                // Still update radio while screen is off
                Radio_update();
            }
            else {
                // Reset input timer on any button press
                if (PAD_anyPressed()) {
                    last_input_time = SDL_GetTicks();
                }

                // Station switching with UP/DOWN
                if (PAD_justPressed(BTN_UP) || PAD_justPressed(BTN_R1)) {
                    // Next station
                    RadioStation* stations;
                    int station_count = Radio_getStations(&stations);
                    if (station_count > 1) {
                        radio_selected = (radio_selected + 1) % station_count;
                        Radio_stop();
                        Radio_play(stations[radio_selected].url);
                        dirty = 1;
                    }
                }
                else if (PAD_justPressed(BTN_DOWN) || PAD_justPressed(BTN_L1)) {
                    // Previous station
                    RadioStation* stations;
                    int station_count = Radio_getStations(&stations);
                    if (station_count > 1) {
                        radio_selected = (radio_selected - 1 + station_count) % station_count;
                        Radio_stop();
                        Radio_play(stations[radio_selected].url);
                        dirty = 1;
                    }
                }
                else if (PAD_justPressed(BTN_B)) {
                    Radio_stop();
                    app_state = STATE_RADIO_LIST;
                    if (autosleep_disabled) {
                        PWR_enableAutosleep();
                        autosleep_disabled = false;
                    }
                    dirty = 1;
                }

                // Update radio state
                Radio_update();

                // Auto screen-off after inactivity (only while playing)
                if (Radio_getState() == RADIO_STATE_PLAYING) {
                    uint32_t screen_timeout_ms = CFG_getScreenTimeoutSecs() * 1000;
                    if (screen_timeout_ms > 0 && last_input_time > 0) {
                        uint32_t now = SDL_GetTicks();
                        if (now - last_input_time >= screen_timeout_ms) {
                            screen_off = true;
                            PLAT_enableBacklight(0);
                        }
                    }
                }

                // Always redraw for visualization
                dirty = 1;
            }
        }
        else if (app_state == STATE_RADIO_ADD) {
            // Country selection screen
            int country_count = Radio_getCuratedCountryCount();

            if (PAD_justRepeated(BTN_UP)) {
                if (add_country_selected > 0) {
                    add_country_selected--;
                    dirty = 1;
                }
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                if (add_country_selected < country_count - 1) {
                    add_country_selected++;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_A) && country_count > 0) {
                // Select country and go to station selection
                const CuratedCountry* countries = Radio_getCuratedCountries();
                add_selected_country_code = countries[add_country_selected].code;
                add_station_selected = 0;
                add_station_scroll = 0;
                // Initialize checked states based on existing stations
                memset(add_station_checked, 0, sizeof(add_station_checked));
                int sc = 0;
                const CuratedStation* cs = Radio_getCuratedStations(add_selected_country_code, &sc);
                for (int i = 0; i < sc && i < 256; i++) {
                    add_station_checked[i] = Radio_stationExists(cs[i].url);
                }
                app_state = STATE_RADIO_ADD_STATIONS;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                app_state = STATE_RADIO_LIST;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_Y)) {
                // Go to help screen
                app_state = STATE_RADIO_HELP;
                dirty = 1;
            }
        }
        else if (app_state == STATE_RADIO_ADD_STATIONS) {
            // Station selection screen
            int station_count = 0;
            const CuratedStation* stations = Radio_getCuratedStations(add_selected_country_code, &station_count);

            if (PAD_justRepeated(BTN_UP)) {
                if (add_station_selected > 0) {
                    add_station_selected--;
                    dirty = 1;
                }
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                if (add_station_selected < station_count - 1) {
                    add_station_selected++;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_A) && station_count > 0) {
                // Toggle station selection (allow toggling all stations)
                if (add_station_selected < 256) {
                    add_station_checked[add_station_selected] = !add_station_checked[add_station_selected];
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_X)) {
                // Add/remove stations based on checked state
                int added = 0;
                int removed = 0;
                for (int i = 0; i < station_count && i < 256; i++) {
                    bool exists = Radio_stationExists(stations[i].url);
                    if (add_station_checked[i] && !exists) {
                        // Add new station
                        if (Radio_addStation(stations[i].name, stations[i].url, stations[i].genre, stations[i].slogan) >= 0) {
                            added++;
                        }
                    } else if (!add_station_checked[i] && exists) {
                        // Remove unchecked station
                        if (Radio_removeStationByUrl(stations[i].url)) {
                            removed++;
                        }
                    }
                }
                if (added > 0 || removed > 0) {
                    Radio_saveStations();
                }
                // Clear selections and go back
                memset(add_station_checked, 0, sizeof(add_station_checked));
                app_state = STATE_RADIO_LIST;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                app_state = STATE_RADIO_ADD;
                dirty = 1;
            }
        }
        else if (app_state == STATE_RADIO_HELP) {
            // Help screen - scrolling and back button
            int scroll_step = SCALE1(18);  // Same as line_h
            if (PAD_justRepeated(BTN_UP)) {
                if (help_scroll > 0) {
                    help_scroll -= scroll_step;
                    if (help_scroll < 0) help_scroll = 0;
                    dirty = 1;
                }
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                help_scroll += scroll_step;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                help_scroll = 0;  // Reset scroll when leaving
                app_state = STATE_RADIO_ADD;
                dirty = 1;
            }
        }
        else if (app_state == STATE_YOUTUBE_MENU) {
            if (PAD_justRepeated(BTN_UP)) {
                if (youtube_menu_selected > 0) {
                    youtube_menu_selected--;
                    dirty = 1;
                }
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                if (youtube_menu_selected < YOUTUBE_MENU_COUNT - 1) {
                    youtube_menu_selected++;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_A)) {
                if (youtube_menu_selected == 0) {
                    // Search Music - open keyboard
                    char* query = YouTube_openKeyboard("Search:");
                    if (query && strlen(query) > 0) {
                        strncpy(youtube_search_query, query, sizeof(youtube_search_query) - 1);
                        youtube_search_query[sizeof(youtube_search_query) - 1] = '\0';
                        youtube_searching = true;
                        youtube_results_selected = 0;
                        youtube_results_scroll = 0;
                        youtube_result_count = 0;
                        app_state = STATE_YOUTUBE_SEARCHING;
                    }
                    if (query) free(query);
                    dirty = 1;
                } else if (youtube_menu_selected == 1) {
                    // Download Queue
                    youtube_queue_selected = 0;
                    youtube_queue_scroll = 0;
                    app_state = STATE_YOUTUBE_QUEUE;
                    dirty = 1;
                } else if (youtube_menu_selected == 2) {
                    // Update yt-dlp
                    YouTube_startUpdate();
                    app_state = STATE_YOUTUBE_UPDATING;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_B)) {
                app_state = STATE_MENU;
                dirty = 1;
            }
        }
        else if (app_state == STATE_YOUTUBE_RESULTS) {
            if (PAD_justRepeated(BTN_UP)) {
                if (youtube_results_selected > 0) {
                    youtube_results_selected--;
                    dirty = 1;
                }
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                if (youtube_results_selected < youtube_result_count - 1) {
                    youtube_results_selected++;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_A) && youtube_result_count > 0) {
                // Toggle add/remove from queue
                YouTubeResult* result = &youtube_results[youtube_results_selected];
                if (YouTube_isInQueue(result->video_id)) {
                    // Remove from queue
                    if (YouTube_queueRemoveById(result->video_id) == 0) {
                        snprintf(youtube_toast_message, sizeof(youtube_toast_message), "Removed from queue");
                    } else {
                        snprintf(youtube_toast_message, sizeof(youtube_toast_message), "Failed to remove");
                    }
                } else {
                    // Add to queue
                    int added = YouTube_queueAdd(result->video_id, result->title);
                    if (added == 1) {
                        snprintf(youtube_toast_message, sizeof(youtube_toast_message), "Added to queue!");
                    } else if (added == -1) {
                        snprintf(youtube_toast_message, sizeof(youtube_toast_message), "Queue is full");
                    }
                }
                youtube_toast_time = SDL_GetTicks();
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                youtube_toast_message[0] = '\0';  // Clear toast
                app_state = STATE_YOUTUBE_MENU;
                dirty = 1;
            }
        }
        else if (app_state == STATE_YOUTUBE_QUEUE) {
            int qcount = YouTube_queueCount();
            if (PAD_justRepeated(BTN_UP)) {
                if (youtube_queue_selected > 0) {
                    youtube_queue_selected--;
                    dirty = 1;
                }
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                if (youtube_queue_selected < qcount - 1) {
                    youtube_queue_selected++;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_A) && qcount > 0) {
                // Start downloading
                if (YouTube_downloadStart() == 0) {
                    app_state = STATE_YOUTUBE_DOWNLOADING;
                }
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_X) && qcount > 0) {
                // Remove selected item
                YouTube_queueRemove(youtube_queue_selected);
                if (youtube_queue_selected >= YouTube_queueCount() && youtube_queue_selected > 0) {
                    youtube_queue_selected--;
                }
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                app_state = STATE_YOUTUBE_MENU;
                dirty = 1;
            }
        }
        else if (app_state == STATE_YOUTUBE_DOWNLOADING) {
            YouTube_update();
            const YouTubeDownloadStatus* status = YouTube_getDownloadStatus();
            if (status->state != YOUTUBE_STATE_DOWNLOADING) {
                // Download finished
                app_state = STATE_YOUTUBE_QUEUE;
            }
            if (PAD_justPressed(BTN_B)) {
                // Cancel download
                YouTube_downloadStop();
                app_state = STATE_YOUTUBE_QUEUE;
            }
            dirty = 1;  // Always redraw during download
        }
        else if (app_state == STATE_YOUTUBE_UPDATING) {
            YouTube_update();
            const YouTubeUpdateStatus* status = YouTube_getUpdateStatus();
            if (PAD_justPressed(BTN_B)) {
                if (status->updating) {
                    // Cancel update
                    YouTube_cancelUpdate();
                }
                app_state = STATE_YOUTUBE_MENU;
                dirty = 1;
            }
            dirty = 1;  // Always redraw during update
        }
        else if (app_state == STATE_APP_UPDATING) {
            // Disable autosleep during update to prevent screen turning off
            if (!autosleep_disabled) {
                PWR_disableAutosleep();
                autosleep_disabled = true;
            }

            SelfUpdate_update();
            const SelfUpdateStatus* status = SelfUpdate_getStatus();
            SelfUpdateState state = status->state;

            if (state == SELFUPDATE_STATE_COMPLETED) {
                if (PAD_justPressed(BTN_A)) {
                    // Restart app - autosleep will be re-enabled in cleanup
                    quit = true;
                }
                // No "Later" option - force restart after successful update
            }
            else if (PAD_justPressed(BTN_B)) {
                if (state == SELFUPDATE_STATE_DOWNLOADING) {
                    // Cancel update
                    SelfUpdate_cancelUpdate();
                }
                // Re-enable autosleep when leaving update screen
                if (autosleep_disabled) {
                    PWR_enableAutosleep();
                    autosleep_disabled = false;
                }
                app_state = STATE_ABOUT;
                dirty = 1;
            }
            dirty = 1;  // Always redraw during update
        }
        else if (app_state == STATE_ABOUT) {
            if (PAD_justPressed(BTN_A)) {
                // Start update if available
                const SelfUpdateStatus* status = SelfUpdate_getStatus();
                if (status->update_available) {
                    SelfUpdate_startUpdate();
                    app_state = STATE_APP_UPDATING;
                    dirty = 1;
                }
            }
            else if (PAD_justPressed(BTN_B)) {
                app_state = STATE_MENU;
                dirty = 1;
            }
        }

        PWR_update(&dirty, &show_setting, NULL, NULL);

        // Skip rendering when screen is off to save power
        if (dirty && !screen_off) {
            switch (app_state) {
                case STATE_MENU:
                    render_menu(show_setting);
                    break;
                case STATE_BROWSER:
                    render_browser(show_setting);
                    break;
                case STATE_PLAYING:
                    render_playing(show_setting);
                    break;
                case STATE_RADIO_LIST:
                    render_radio_list(show_setting);
                    break;
                case STATE_RADIO_PLAYING:
                    render_radio_playing(show_setting);
                    break;
                case STATE_RADIO_ADD:
                    render_radio_add(show_setting);
                    break;
                case STATE_RADIO_ADD_STATIONS:
                    render_radio_add_stations(show_setting);
                    break;
                case STATE_RADIO_HELP:
                    render_radio_help(show_setting);
                    break;
                case STATE_YOUTUBE_MENU:
                    render_youtube_menu(show_setting);
                    break;
                case STATE_YOUTUBE_SEARCHING:
                    render_youtube_searching(show_setting);
                    break;
                case STATE_YOUTUBE_RESULTS:
                    render_youtube_results(show_setting);
                    break;
                case STATE_YOUTUBE_QUEUE:
                    render_youtube_queue(show_setting);
                    break;
                case STATE_YOUTUBE_DOWNLOADING:
                    render_youtube_downloading(show_setting);
                    break;
                case STATE_YOUTUBE_UPDATING:
                    render_youtube_updating(show_setting);
                    break;
                case STATE_APP_UPDATING:
                    render_app_updating(show_setting);
                    break;
                case STATE_ABOUT:
                    render_about(show_setting);
                    break;
            }

            if (show_setting) {
                GFX_blitHardwareHints(screen, show_setting);
            }

            // Render quit confirmation dialog overlay if shown
            if (show_quit_confirm) {
                render_quit_confirm();
            }

            GFX_flip(screen);
            dirty = 0;

            // Keep refreshing while toast is visible
            if (app_state == STATE_YOUTUBE_RESULTS && youtube_toast_message[0] != '\0') {
                if (SDL_GetTicks() - youtube_toast_time < YOUTUBE_TOAST_DURATION) {
                    dirty = 1;
                }
            }

            // Perform YouTube search after rendering the searching screen
            if (app_state == STATE_YOUTUBE_SEARCHING && youtube_searching) {
                youtube_result_count = YouTube_search(youtube_search_query, youtube_results, YOUTUBE_MAX_RESULTS);
                youtube_searching = false;
                if (youtube_result_count > 0) {
                    app_state = STATE_YOUTUBE_RESULTS;
                    // Reset button state to prevent auto-add from lingering keyboard button press
                    PAD_reset();
                } else {
                    // No results or error - go back to menu
                    app_state = STATE_YOUTUBE_MENU;
                }
                dirty = 1;
            }
        } else if (!screen_off) {
            GFX_sync();
        }
    }

cleanup:
    // Ensure screen is back on and autosleep is re-enabled
    if (screen_off) {
        PLAT_enableBacklight(1);
        screen_off = false;
    }
    if (autosleep_disabled) {
        PWR_enableAutosleep();
        autosleep_disabled = false;
    }

    SelfUpdate_cleanup();
    YouTube_cleanup();
    Radio_quit();
    Player_quit();
    free_entries();
    unload_custom_fonts();

    QuitSettings();
    PWR_quit();
    PAD_quit();
    GFX_quit();

    return EXIT_SUCCESS;
}
