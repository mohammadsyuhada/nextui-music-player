#ifndef __UI_MAIN_H__
#define __UI_MAIN_H__

#include <SDL2/SDL.h>

// Render the main menu (Local Files, Internet Radio, MP3 Downloader, About)
void render_menu(SDL_Surface* screen, int show_setting, int menu_selected);

// Render quit confirmation dialog overlay
void render_quit_confirm(SDL_Surface* screen);

#endif
