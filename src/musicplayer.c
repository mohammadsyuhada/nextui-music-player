#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <msettings.h>

#include "psa/crypto.h"
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "config.h"
#include "player.h"
#include "selfupdate.h"

// UI modules
#include "ui_fonts.h"
#include "ui_icons.h"

// Module architecture
#include "module_common.h"
#include "module_menu.h"
#include "module_player.h"
#include "module_radio.h"
#include "module_podcast.h"
#include "module_downloader.h"
#include "module_system.h"
#include "module_settings.h"
#include "settings.h"

// Global quit flag
static bool quit = false;
static SDL_Surface* screen;

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

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    InitSettings();
    screen = GFX_init(MODE_MAIN);
    PAD_init();
    PWR_init();
    WIFI_init();
    psa_crypto_init();

    // Load bundled fonts
    Fonts_load();

    // Load icons (if available)
    Icons_init();

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    // Seed random number generator for shuffle
    srand((unsigned int)time(NULL));

    // Initialize player core
    if (Player_init() != 0) {
        LOG_error("Failed to initialize audio player\n");
        goto cleanup;
    }

    // At startup, set software volume based on output device
    if (Player_isBluetoothActive() || Player_isUSBDACActive()) {
        // Use cubic curve for perceptual volume (human hearing is logarithmic)
        float v = GetVolume() / 20.0f;
        Player_setVolume(v * v * v);
    } else {
        Player_setVolume(1.0f);
    }

    // Initialize self-update module
    SelfUpdate_init(".");
    SelfUpdate_checkForUpdate();

    // Initialize common module (global input handling)
    ModuleCommon_init();

    // Initialize app-specific settings
    Settings_init();

    // Main application loop
    while (!quit) {
        // Run main menu - returns selected item or MENU_QUIT
        int selection = MenuModule_run(screen);

        if (selection == MENU_QUIT) {
            quit = true;
            continue;
        }

        // Run the selected module
        ModuleExitReason reason = MODULE_EXIT_TO_MENU;

        switch (selection) {
            case MENU_LOCAL_FILES:
                reason = PlayerModule_run(screen);
                break;
            case MENU_RADIO:
                reason = RadioModule_run(screen);
                break;
            case MENU_PODCAST:
                reason = PodcastModule_run(screen);
                break;
            case MENU_DOWNLOADER:
                reason = DownloaderModule_run(screen);
                break;
            case MENU_SETTINGS:
                reason = SettingsModule_run(screen);
                break;
        }

        if (reason == MODULE_EXIT_QUIT) {
            quit = true;
        }
    }

cleanup:
    Settings_quit();
    ModuleCommon_quit();
    SelfUpdate_cleanup();
    Player_quit();
    Icons_quit();
    Fonts_unload();

    QuitSettings();
    PWR_quit();
    PAD_quit();
    GFX_quit();

    return EXIT_SUCCESS;
}
