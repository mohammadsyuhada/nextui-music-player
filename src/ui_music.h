#ifndef __UI_MUSIC_H__
#define __UI_MUSIC_H__

#include <SDL2/SDL.h>
#include <stdbool.h>
#include "browser.h"
#include "player.h"

// Render the file browser screen
void render_browser(SDL_Surface* screen, int show_setting, BrowserContext* browser);

// Render the now playing screen
void render_playing(SDL_Surface* screen, int show_setting, BrowserContext* browser,
                    bool shuffle_enabled, bool repeat_enabled);

// Check if browser list has active scrolling (for refresh optimization)
bool browser_needs_scroll_refresh(void);

#endif
