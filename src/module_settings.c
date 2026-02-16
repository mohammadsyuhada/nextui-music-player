#include "defines.h"
#include "api.h"
#include "module_common.h"
#include "module_settings.h"
#include "ui_main.h"
#include "settings.h"
#include "selfupdate.h"
#include "downloader.h"
#include "ui_settings.h"
#include "ui_system.h"
#include "wifi.h"
#include "album_art.h"

// Internal states
typedef enum {
    SETTINGS_STATE_MENU,
    SETTINGS_STATE_CLEAR_CACHE_CONFIRM,
    SETTINGS_STATE_ABOUT,
    SETTINGS_STATE_UPDATING,
    SETTINGS_STATE_UPDATING_YTDLP
} SettingsState;

// Settings menu items
#define SETTINGS_ITEM_SCREEN_OFF    0
#define SETTINGS_ITEM_BASS_FILTER   1
#define SETTINGS_ITEM_SOFT_LIMITER  2
#define SETTINGS_ITEM_CLEAR_CACHE   3
#define SETTINGS_ITEM_UPDATE_YTDLP  4
#define SETTINGS_ITEM_ABOUT         5
#define SETTINGS_ITEM_COUNT         6

// Internal app state constants for controls help
// These match the pattern used in ui_main.c
#define SETTINGS_INTERNAL_MENU      40
#define SETTINGS_INTERNAL_ABOUT     41

ModuleExitReason SettingsModule_run(SDL_Surface* screen) {
    SettingsState state = SETTINGS_STATE_MENU;
    int menu_selected = 0;
    int dirty = 1;
    int show_setting = 0;

    while (1) {
        PAD_poll();

        // Handle global input first
        int app_state = (state == SETTINGS_STATE_MENU) ? SETTINGS_INTERNAL_MENU : SETTINGS_INTERNAL_ABOUT;
        GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, app_state);
        if (global.should_quit) {
            return MODULE_EXIT_QUIT;
        }
        if (global.input_consumed) {
            if (global.dirty) dirty = 1;
            GFX_sync();
            continue;
        }

        // State-specific handling
        switch (state) {
            case SETTINGS_STATE_MENU:
                // Navigation
                if (PAD_justPressed(BTN_UP)) {
                    if (menu_selected > 0) {
                        menu_selected--;
                        dirty = 1;
                    }
                }
                else if (PAD_justPressed(BTN_DOWN)) {
                    if (menu_selected < SETTINGS_ITEM_COUNT - 1) {
                        menu_selected++;
                        dirty = 1;
                    }
                }
                // Left/Right for cyclable settings
                else if (PAD_justPressed(BTN_LEFT)) {
                    if (menu_selected == SETTINGS_ITEM_SCREEN_OFF) {
                        Settings_cycleScreenOffPrev();
                        dirty = 1;
                    } else if (menu_selected == SETTINGS_ITEM_BASS_FILTER) {
                        Settings_cycleBassFilterPrev();
                        dirty = 1;
                    } else if (menu_selected == SETTINGS_ITEM_SOFT_LIMITER) {
                        Settings_cycleSoftLimiterPrev();
                        dirty = 1;
                    }
                }
                else if (PAD_justPressed(BTN_RIGHT)) {
                    if (menu_selected == SETTINGS_ITEM_SCREEN_OFF) {
                        Settings_cycleScreenOffNext();
                        dirty = 1;
                    } else if (menu_selected == SETTINGS_ITEM_BASS_FILTER) {
                        Settings_cycleBassFilterNext();
                        dirty = 1;
                    } else if (menu_selected == SETTINGS_ITEM_SOFT_LIMITER) {
                        Settings_cycleSoftLimiterNext();
                        dirty = 1;
                    }
                }
                // A button
                else if (PAD_justPressed(BTN_A)) {
                    switch (menu_selected) {
                        case SETTINGS_ITEM_SCREEN_OFF:
                            // A also cycles the value (convenience)
                            Settings_cycleScreenOffNext();
                            dirty = 1;
                            break;
                        case SETTINGS_ITEM_BASS_FILTER:
                            Settings_cycleBassFilterNext();
                            dirty = 1;
                            break;
                        case SETTINGS_ITEM_SOFT_LIMITER:
                            Settings_cycleSoftLimiterNext();
                            dirty = 1;
                            break;
                        case SETTINGS_ITEM_CLEAR_CACHE:
                            state = SETTINGS_STATE_CLEAR_CACHE_CONFIRM;
                            dirty = 1;
                            break;
                        case SETTINGS_ITEM_UPDATE_YTDLP:
                            if (Downloader_init() == 0 && Wifi_ensureConnected(screen, show_setting)) {
                                Downloader_startUpdate();
                                state = SETTINGS_STATE_UPDATING_YTDLP;
                            }
                            dirty = 1;
                            break;
                        case SETTINGS_ITEM_ABOUT:
                            state = SETTINGS_STATE_ABOUT;
                            dirty = 1;
                            break;
                    }
                }
                // B button - back to main menu
                else if (PAD_justPressed(BTN_B)) {
                    return MODULE_EXIT_TO_MENU;
                }
                break;

            case SETTINGS_STATE_CLEAR_CACHE_CONFIRM:
                if (PAD_justPressed(BTN_A)) {
                    // Confirm - clear the cache
                    album_art_clear_disk_cache();
                    state = SETTINGS_STATE_MENU;
                    dirty = 1;
                }
                else if (PAD_justPressed(BTN_B)) {
                    // Cancel
                    state = SETTINGS_STATE_MENU;
                    dirty = 1;
                }
                break;

            case SETTINGS_STATE_ABOUT:
                SelfUpdate_update();
                const SelfUpdateStatus* status = SelfUpdate_getStatus();

                // Keep refreshing while checking for updates
                if (status->state == SELFUPDATE_STATE_CHECKING) {
                    dirty = 1;
                }

                if (PAD_justPressed(BTN_A)) {
                    if (status->update_available) {
                        SelfUpdate_startUpdate();
                        state = SETTINGS_STATE_UPDATING;
                        dirty = 1;
                    } else if (status->state != SELFUPDATE_STATE_CHECKING) {
                        if (Wifi_ensureConnected(screen, show_setting)) {
                            SelfUpdate_checkForUpdate();
                        }
                        dirty = 1;
                    }
                }
                else if (PAD_justPressed(BTN_B)) {
                    state = SETTINGS_STATE_MENU;
                    dirty = 1;
                }
                break;

            case SETTINGS_STATE_UPDATING:
                // Disable autosleep during update
                ModuleCommon_setAutosleepDisabled(true);

                SelfUpdate_update();
                const SelfUpdateStatus* update_status = SelfUpdate_getStatus();
                SelfUpdateState update_state = update_status->state;

                if (update_state == SELFUPDATE_STATE_COMPLETED) {
                    if (PAD_justPressed(BTN_A)) {
                        // Quit to apply update
                        ModuleCommon_setAutosleepDisabled(false);
                        return MODULE_EXIT_QUIT;
                    }
                }
                else if (PAD_justPressed(BTN_B)) {
                    if (update_state == SELFUPDATE_STATE_DOWNLOADING) {
                        SelfUpdate_cancelUpdate();
                    }
                    ModuleCommon_setAutosleepDisabled(false);
                    state = SETTINGS_STATE_ABOUT;
                    dirty = 1;
                }

                // Always redraw during update
                dirty = 1;
                break;

            case SETTINGS_STATE_UPDATING_YTDLP:
                Downloader_update();
                const DownloaderUpdateStatus* ytdlp_status = Downloader_getUpdateStatus();

                if (!ytdlp_status->updating) {
                    state = SETTINGS_STATE_MENU;
                }

                if (PAD_justPressed(BTN_B)) {
                    if (ytdlp_status->updating) {
                        Downloader_cancelUpdate();
                    }
                    state = SETTINGS_STATE_MENU;
                }

                dirty = 1;
                break;
        }

        // Handle power management
        ModuleCommon_PWR_update(&dirty, &show_setting);

        // Render
        if (dirty) {
            switch (state) {
                case SETTINGS_STATE_MENU:
                    render_settings_menu(screen, show_setting, menu_selected);
                    break;
                case SETTINGS_STATE_CLEAR_CACHE_CONFIRM:
                    render_settings_menu(screen, show_setting, menu_selected);
                    render_confirmation_dialog(screen, NULL, "Clear album art cache?");
                    break;
                case SETTINGS_STATE_ABOUT:
                    render_about(screen, show_setting);
                    break;
                case SETTINGS_STATE_UPDATING:
                    render_app_updating(screen, show_setting);
                    break;
                case SETTINGS_STATE_UPDATING_YTDLP:
                    render_ytdlp_updating(screen, show_setting);
                    break;
            }

            if (show_setting) {
                GFX_blitHardwareHints(screen, show_setting);
            }

            GFX_flip(screen);
            dirty = 0;
        } else {
            GFX_sync();
        }
    }
}
