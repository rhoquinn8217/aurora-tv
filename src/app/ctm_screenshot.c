/*
 * Writes what the interface has just drawn to a file.
 *
 * Why this exists: nothing here can see a television. The control port can
 * already report a dialogue's size and where it sits, but not what it looks
 * like, so every judgement about appearance has needed a person with a camera.
 *
 * What it CANNOT do, and this is not a fault to fix: the stream is not in the
 * picture. The decoded video lives on the NDL hardware video plane and the
 * interface is drawn above it, transparent where the video shows through
 * (lv_disp_drv_app.c clears with an alpha of zero). Reading our own texture
 * gets the interface and black behind it, whatever the host is showing.
 */

#include "ctm_screenshot.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <SDL.h>

#include "lvgl.h"
#include "draw/sdl/lv_draw_sdl.h"

#include "logging.h"

/*
 * The app's own directory is writable (the config lives beside it) and
 * world-readable, so an ssh session can collect what we write. /tmp is not a
 * candidate: webOS 26 restricts it to traverse-only, so a write there fails on
 * the newer sets and leaves a misleading downstream error.
 */
static bool shot_dir(char *out, size_t out_len)
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return false;
    }
    if ((size_t) snprintf(out, out_len, "%s/logs", home) >= out_len) {
        return false;
    }
    return true;
}

bool ctm_screenshot_write(const char *name, char *out, size_t out_len)
{
    /*
     * A name from the control port may not steer the write anywhere else: no
     * separators, no walking up. Anything odd falls back to a stamped name.
     */
    char leaf[128];
    if (name != NULL && name[0] != '\0' &&
        strchr(name, '/') == NULL && strchr(name, '\\') == NULL &&
        strstr(name, "..") == NULL) {
        snprintf(leaf, sizeof leaf, "%s", name);
    } else {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        strftime(leaf, sizeof leaf, "shot-%Y%m%d-%H%M%S.bmp", &tm_now);
    }
    if (strstr(leaf, ".bmp") == NULL) {
        size_t used = strlen(leaf);
        snprintf(leaf + used, sizeof leaf - used, ".bmp");
    }

    char dir[256];
    if (!shot_dir(dir, sizeof dir)) {
        snprintf(out, out_len, "no HOME to write under");
        return false;
    }

    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL || disp->driver == NULL || disp->driver->user_data == NULL) {
        snprintf(out, out_len, "the interface has no display yet");
        return false;
    }
    lv_draw_sdl_drv_param_t *param = disp->driver->user_data;
    SDL_Renderer *renderer = param->renderer;
    if (renderer == NULL) {
        snprintf(out, out_len, "the display has no renderer");
        return false;
    }

    const int w = (int) disp->driver->hor_res;
    const int h = (int) disp->driver->ver_res;
    if (w <= 0 || h <= 0) {
        snprintf(out, out_len, "the display has no size");
        return false;
    }

    /*
     * The render target is already the interface's own texture: the driver
     * sets it back after every redraw and when it creates the display, so a
     * read here takes the interface itself rather than a back buffer whose
     * contents after a present are not defined. We do NOT change the target --
     * doing that mid-frame would disturb the next redraw.
     */
    SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (shot == NULL) {
        snprintf(out, out_len, "could not make a surface: %s", SDL_GetError());
        return false;
    }
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                             shot->pixels, shot->pitch) != 0) {
        snprintf(out, out_len, "could not read the interface: %s", SDL_GetError());
        SDL_FreeSurface(shot);
        return false;
    }

    /*
     * Flatten onto black. The interface carries real transparency where the
     * video plane shows through, and a BMP's handling of an alpha channel is
     * not worth relying on: what a reader wants is the interface as the eye
     * sees it against a dark screen.
     */
    SDL_Surface *flat = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_RGB24);
    if (flat == NULL) {
        snprintf(out, out_len, "could not make the flat surface: %s", SDL_GetError());
        SDL_FreeSurface(shot);
        return false;
    }
    SDL_FillRect(flat, NULL, SDL_MapRGB(flat->format, 0, 0, 0));
    SDL_SetSurfaceBlendMode(shot, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(shot, NULL, flat, NULL);
    SDL_FreeSurface(shot);

    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    if (SDL_SaveBMP(flat, path) != 0) {
        snprintf(out, out_len, "could not write %s: %s", path, SDL_GetError());
        SDL_FreeSurface(flat);
        return false;
    }
    SDL_FreeSurface(flat);

    commons_log_info("Screenshot", "wrote %s (%dx%d)", path, w, h);
    snprintf(out, out_len, "%s %dx%d", path, w, h);
    return true;
}
