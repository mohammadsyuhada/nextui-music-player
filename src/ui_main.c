#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_main.h"
#include "ui_fonts.h"
#include "ui_utils.h"
#include "selfupdate.h"

// Menu items
static const char* menu_items[] = {"Local Files", "Internet Radio", "MP3 Downloader", "About"};
#define MENU_ITEM_COUNT 4

// Label callback for update badge on About menu item
static const char* main_menu_get_label(int index, const char* default_label,
                                        char* buffer, int buffer_size) {
    if (index == 3) {  // About menu item
        const SelfUpdateStatus* status = SelfUpdate_getStatus();
        if (status->update_available) {
            snprintf(buffer, buffer_size, "About (Update available)");
            return buffer;
        }
    }
    return NULL;  // Use default label
}

// Render the main menu
void render_menu(SDL_Surface* screen, int show_setting, int menu_selected) {
    SimpleMenuConfig config = {
        .title = "Music Player",
        .items = menu_items,
        .item_count = MENU_ITEM_COUNT,
        .btn_b_label = "EXIT",
        .get_label = main_menu_get_label,
        .render_badge = NULL
    };
    render_simple_menu(screen, show_setting, menu_selected, &config);
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
    SDL_Surface* msg_surf = TTF_RenderUTF8_Blended(get_font_medium(), msg, COLOR_WHITE);
    if (msg_surf) {
        SDL_BlitSurface(msg_surf, NULL, screen, &(SDL_Rect){(hw - msg_surf->w) / 2, box_y + SCALE1(20)});
        SDL_FreeSurface(msg_surf);
    }

    // Button hints
    const char* hint = "A: Yes   B: No";
    SDL_Surface* hint_surf = TTF_RenderUTF8_Blended(get_font_small(), hint, COLOR_GRAY);
    if (hint_surf) {
        SDL_BlitSurface(hint_surf, NULL, screen, &(SDL_Rect){(hw - hint_surf->w) / 2, box_y + SCALE1(55)});
        SDL_FreeSurface(hint_surf);
    }
}
