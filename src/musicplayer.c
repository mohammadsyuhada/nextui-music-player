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
#include "spectrum.h"
#include "radio.h"
#include "radio_album_art.h"
#include "youtube.h"
#include "selfupdate.h"

// UI modules
#include "ui_fonts.h"
#include "ui_utils.h"
#include "browser.h"
#include "ui_album_art.h"
#include "ui_main.h"
#include "ui_music.h"
#include "ui_radio.h"
#include "ui_youtube.h"
#include "ui_system.h"

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

// FileEntry and BrowserContext are now in browser.h

// Menu item count (menu_items moved to ui_main.c)
#define MENU_ITEM_COUNT 4

// YouTube menu count (youtube_menu_items moved to ui_youtube.c)
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

// Helper function to load directory using Browser module
static void load_directory(const char* path) {
    Browser_loadDirectory(&browser, path, MUSIC_PATH);
}

// Render functions are now in UI modules (ui_music.h, ui_radio.h, ui_youtube.h, ui_system.h)
// See: ui_music.c, ui_radio.c, ui_youtube.c, ui_system.c

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
    // System volume is 0-20, software volume is 0.0-1.0
    Player_setVolume(GetVolume() / 20.0f);

    Spectrum_init();
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
        // System volume is 0-20, software volume is 0.0-1.0
        if (PAD_justRepeated(BTN_PLUS)) {
            // Get current volume from software volume (0.0-1.0) and convert to 0-20
            int vol = (int)(Player_getVolume() * 20.0f + 0.5f);
            vol = (vol < 20) ? vol + 1 : 20;
            // Skip SetVolume() for Bluetooth as it can block
            if (!Player_isBluetoothActive()) {
                SetVolume(vol);
            }
            // Apply software volume immediately (works for all output devices)
            Player_setVolume(vol / 20.0f);
        }
        else if (PAD_justRepeated(BTN_MINUS)) {
            // Get current volume from software volume (0.0-1.0) and convert to 0-20
            int vol = (int)(Player_getVolume() * 20.0f + 0.5f);
            vol = (vol > 0) ? vol - 1 : 0;
            // Skip SetVolume() for Bluetooth as it can block
            if (!Player_isBluetoothActive()) {
                SetVolume(vol);
            }
            // Apply software volume immediately (works for all output devices)
            Player_setVolume(vol / 20.0f);
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
            // Clear all GPU layers so dialog is not obscured
            GFX_clearLayers(LAYER_SCROLLTEXT);
            PLAT_clearLayers(LAYER_SPECTRUM);
            PLAT_clearLayers(LAYER_PLAYTIME);
            PLAT_GPU_Flip();  // Apply layer clears
            PlayTime_clear();  // Reset playtime state
            dirty = 1;
        }
        // Handle input based on state
        else if (app_state == STATE_MENU) {
            if (PAD_justRepeated(BTN_UP)) {
                menu_selected = (menu_selected > 0) ? menu_selected - 1 : MENU_ITEM_COUNT - 1;
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                menu_selected = (menu_selected < MENU_ITEM_COUNT - 1) ? menu_selected + 1 : 0;
                dirty = 1;
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
            if (PAD_justRepeated(BTN_UP) && browser.entry_count > 0) {
                browser.selected = (browser.selected > 0) ? browser.selected - 1 : browser.entry_count - 1;
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN) && browser.entry_count > 0) {
                browser.selected = (browser.selected < browser.entry_count - 1) ? browser.selected + 1 : 0;
                dirty = 1;
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
                    GFX_clearLayers(LAYER_SCROLLTEXT);  // Clear scroll layer when leaving browser
                    app_state = STATE_MENU;
                    dirty = 1;
                }
            }

            // Animate scroll without full redraw (GPU mode)
            if (browser_needs_scroll_refresh()) {
                browser_animate_scroll();
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
                        int audio_count = Browser_countAudioFiles(&browser);
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
                        // Clear all GPU layers when leaving player
                        GFX_clearLayers(LAYER_SCROLLTEXT);
                        PLAT_clearLayers(LAYER_SPECTRUM);
                        PLAT_clearLayers(LAYER_PLAYTIME);
                        PLAT_GPU_Flip();
                        PlayTime_clear();  // Reset playtime state
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
                    cleanup_album_art_background();  // Clear cached background when stopping
                    // Clear all GPU layers when leaving player
                    GFX_clearLayers(LAYER_SCROLLTEXT);
                    PLAT_clearLayers(LAYER_SPECTRUM);
                    PLAT_clearLayers(LAYER_PLAYTIME);
                    PLAT_GPU_Flip();  // Apply layer clears
                    // Reset playtime state
                    PlayTime_clear();
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
                else if (PAD_justPressed(BTN_L3) || PAD_justPressed(BTN_L2)) {
                    // Toggle spectrum visibility (L3 or L2)
                    Spectrum_toggleVisibility();
                    dirty = 1;
                }
                else if (PAD_justPressed(BTN_R3) || PAD_justPressed(BTN_R2)) {
                    // Cycle spectrum color style (R3 or R2)
                    Spectrum_cycleStyle();
                    dirty = 1;
                }
                else if (PAD_tappedSelect(SDL_GetTicks())) {
                    // Toggle screen off manually
                    screen_off = true;
                    PLAT_enableBacklight(0);
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
                            int audio_count = Browser_countAudioFiles(&browser);
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
                            // Clear all GPU layers when leaving player
                            GFX_clearLayers(LAYER_SCROLLTEXT);
                            PLAT_clearLayers(LAYER_SPECTRUM);
                            PLAT_clearLayers(LAYER_PLAYTIME);
                            PLAT_GPU_Flip();
                            PlayTime_clear();  // Reset playtime state
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

                }
            }

            // Animate player title scroll (GPU mode, no screen redraw needed)
            // Skip animations when screen is off to save battery
            if (!screen_off) {
                if (player_needs_scroll_refresh()) {
                    player_animate_scroll();
                }

                // Animate spectrum visualizer (GPU mode)
                if (Spectrum_needsRefresh()) {
                    Spectrum_renderGPU();
                }

                // Update playtime display (GPU mode, updates once per second)
                if (PlayTime_needsRefresh()) {
                    PlayTime_renderGPU();
                }
            }
        }
        else if (app_state == STATE_RADIO_LIST) {
            RadioStation* stations;
            int station_count = Radio_getStations(&stations);

            if (PAD_justRepeated(BTN_UP) && station_count > 0) {
                radio_selected = (radio_selected > 0) ? radio_selected - 1 : station_count - 1;
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN) && station_count > 0) {
                radio_selected = (radio_selected < station_count - 1) ? radio_selected + 1 : 0;
                dirty = 1;
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
                    cleanup_album_art_background();  // Clear cached background when stopping
                    RadioStatus_clear();  // Clear GPU status layer
                    app_state = STATE_RADIO_LIST;
                    if (autosleep_disabled) {
                        PWR_enableAutosleep();
                        autosleep_disabled = false;
                    }
                    dirty = 1;
                }
                else if (PAD_tappedSelect(SDL_GetTicks())) {
                    // Toggle screen off manually
                    screen_off = true;
                    PLAT_enableBacklight(0);
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

                // Update status and buffer via GPU layer (no full refresh needed)
                if (RadioStatus_needsRefresh()) {
                    RadioStatus_renderGPU();
                }
            }
        }
        else if (app_state == STATE_RADIO_ADD) {
            // Country selection screen
            int country_count = Radio_getCuratedCountryCount();

            if (PAD_justRepeated(BTN_UP) && country_count > 0) {
                add_country_selected = (add_country_selected > 0) ? add_country_selected - 1 : country_count - 1;
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN) && country_count > 0) {
                add_country_selected = (add_country_selected < country_count - 1) ? add_country_selected + 1 : 0;
                dirty = 1;
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

            if (PAD_justRepeated(BTN_UP) && station_count > 0) {
                add_station_selected = (add_station_selected > 0) ? add_station_selected - 1 : station_count - 1;
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN) && station_count > 0) {
                add_station_selected = (add_station_selected < station_count - 1) ? add_station_selected + 1 : 0;
                dirty = 1;
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
                youtube_menu_selected = (youtube_menu_selected > 0) ? youtube_menu_selected - 1 : YOUTUBE_MENU_COUNT - 1;
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN)) {
                youtube_menu_selected = (youtube_menu_selected < YOUTUBE_MENU_COUNT - 1) ? youtube_menu_selected + 1 : 0;
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_A)) {
                if (youtube_menu_selected == 0) {
                    // Search Music - open keyboard
                    char* query = YouTube_openKeyboard("Search:");
                    // Reset button state and re-poll to prevent keyboard B press from triggering menu back
                    PAD_reset();
                    PAD_poll();
                    PAD_reset();
                    if (query && strlen(query) > 0) {
                        strncpy(youtube_search_query, query, sizeof(youtube_search_query) - 1);
                        youtube_search_query[sizeof(youtube_search_query) - 1] = '\0';
                        youtube_searching = true;
                        youtube_results_selected = -1;  // No selection initially
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
            if (PAD_justRepeated(BTN_UP) && youtube_result_count > 0) {
                if (youtube_results_selected < 0) {
                    youtube_results_selected = youtube_result_count - 1;  // From no selection, go to last
                } else {
                    youtube_results_selected = (youtube_results_selected > 0) ? youtube_results_selected - 1 : youtube_result_count - 1;
                }
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN) && youtube_result_count > 0) {
                if (youtube_results_selected < 0) {
                    youtube_results_selected = 0;  // From no selection, go to first
                } else {
                    youtube_results_selected = (youtube_results_selected < youtube_result_count - 1) ? youtube_results_selected + 1 : 0;
                }
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_A) && youtube_result_count > 0 && youtube_results_selected >= 0) {
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
                GFX_clearLayers(LAYER_SCROLLTEXT);  // Clear scroll layer when leaving
                app_state = STATE_YOUTUBE_MENU;
                dirty = 1;
            }

            // Animate scroll without full redraw (GPU mode)
            if (youtube_results_needs_scroll_refresh()) {
                youtube_results_animate_scroll();
            }
        }
        else if (app_state == STATE_YOUTUBE_QUEUE) {
            int qcount = YouTube_queueCount();
            if (PAD_justRepeated(BTN_UP) && qcount > 0) {
                youtube_queue_selected = (youtube_queue_selected > 0) ? youtube_queue_selected - 1 : qcount - 1;
                dirty = 1;
            }
            else if (PAD_justRepeated(BTN_DOWN) && qcount > 0) {
                youtube_queue_selected = (youtube_queue_selected < qcount - 1) ? youtube_queue_selected + 1 : 0;
                dirty = 1;
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
                GFX_clearLayers(LAYER_SCROLLTEXT);  // Clear scroll layer when leaving
                app_state = STATE_YOUTUBE_MENU;
                dirty = 1;
            }

            // Animate scroll without full redraw (GPU mode)
            if (youtube_queue_needs_scroll_refresh()) {
                youtube_queue_animate_scroll();
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
            // Clear scroll layer on any full redraw - states with scrolling will re-render it
            GFX_clearLayers(LAYER_SCROLLTEXT);

            // Skip state rendering when quit dialog is shown (GPU layers already cleared)
            if (show_quit_confirm) {
                // Just render the dialog overlay on black background
                GFX_clear(screen);
                render_quit_confirm(screen);
                GFX_flip(screen);
                dirty = 0;
            }
            else switch (app_state) {
                case STATE_MENU:
                    render_menu(screen, show_setting, menu_selected);
                    break;
                case STATE_BROWSER:
                    render_browser(screen, show_setting, &browser);
                    break;
                case STATE_PLAYING:
                    render_playing(screen, show_setting, &browser, shuffle_enabled, repeat_enabled);
                    break;
                case STATE_RADIO_LIST:
                    render_radio_list(screen, show_setting, radio_selected, &radio_scroll);
                    break;
                case STATE_RADIO_PLAYING:
                    render_radio_playing(screen, show_setting, radio_selected);
                    break;
                case STATE_RADIO_ADD:
                    render_radio_add(screen, show_setting, add_country_selected, &add_country_scroll);
                    break;
                case STATE_RADIO_ADD_STATIONS:
                    render_radio_add_stations(screen, show_setting, add_selected_country_code,
                                              add_station_selected, &add_station_scroll, add_station_checked);
                    break;
                case STATE_RADIO_HELP:
                    render_radio_help(screen, show_setting, &help_scroll);
                    break;
                case STATE_YOUTUBE_MENU:
                    render_youtube_menu(screen, show_setting, youtube_menu_selected);
                    break;
                case STATE_YOUTUBE_SEARCHING:
                    render_youtube_searching(screen, show_setting, youtube_search_query);
                    break;
                case STATE_YOUTUBE_RESULTS:
                    render_youtube_results(screen, show_setting, youtube_search_query,
                                           youtube_results, youtube_result_count,
                                           youtube_results_selected, &youtube_results_scroll,
                                           youtube_toast_message, youtube_toast_time, youtube_searching);
                    break;
                case STATE_YOUTUBE_QUEUE:
                    render_youtube_queue(screen, show_setting, youtube_queue_selected, &youtube_queue_scroll);
                    break;
                case STATE_YOUTUBE_DOWNLOADING:
                    render_youtube_downloading(screen, show_setting);
                    break;
                case STATE_YOUTUBE_UPDATING:
                    render_youtube_updating(screen, show_setting);
                    break;
                case STATE_APP_UPDATING:
                    render_app_updating(screen, show_setting);
                    break;
                case STATE_ABOUT:
                    render_about(screen, show_setting);
                    break;
            }

            if (show_setting) {
                GFX_blitHardwareHints(screen, show_setting);
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

    // Clear all GPU layers on exit
    GFX_clearLayers(LAYER_SCROLLTEXT);
    PLAT_clearLayers(LAYER_SPECTRUM);
    PLAT_clearLayers(LAYER_PLAYTIME);
    PLAT_clearLayers(LAYER_BUFFER);

    SelfUpdate_cleanup();
    YouTube_cleanup();
    Radio_quit();
    cleanup_album_art_background();  // Clean up cached background surface
    Spectrum_quit();
    Player_quit();
    Browser_freeEntries(&browser);
    unload_custom_fonts();

    QuitSettings();
    PWR_quit();
    PAD_quit();
    GFX_quit();

    return EXIT_SUCCESS;
}
