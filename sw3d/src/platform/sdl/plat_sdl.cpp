/* plat_sdl.cpp - the SDL2 backend of platform/sw_platform.h (macOS, Linux):
 * window and OpenGL picture, the flight yoke on the mouse, switches on the
 * keyboard, sound, the NOVRAM file, the log.  It also has main().
 *
 * Keys follow platform/windows/plat_win.c:
 *   flight yoke   mouse (the yoke stays where it is put); arrow keys (spring back)
 *   triggers      left mouse button / Left Ctrl (right), right mouse button / Left Alt (left)
 *   thumb buttons Space (right), Shift (left)
 *   coins         5 left, 6 right, 7 aux        start: any trigger, with a credit in
 *   F2 self-test switch (latching), F1 diagnostic, F3 slam, F4 sound test (latching)
 *   F8 mouse capture on/off, F11 fullscreen, Esc quit
 */
#include <SDL.h>
#include <SDL_opengl.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../sw_platform.h"
#include "../../sw3d_capture.h"

#define APP_NAME "SW3D"

/* ---- options -------------------------------------------------------------- */
static std::string base_dir, rom_dir, nv_path, svg_path, script_path;
static int    rev = 2, turbo, hidden, no_nvram;
static double quit_after_ms;
static unsigned long svg_at, dump_at;
static std::string dump_path;
static float  mouse_sens = 0.25f, key_rate = 400.0f, brightness = 1.6f, line_width = 2.0f;
static int    invert_pitch;
static uint8_t dsw0 = 0x84, dsw1 = 0x02;

uint8_t plat_dsw0(void) { return dsw0; }
uint8_t plat_dsw1(void) { return dsw1; }

/* ---- log ------------------------------------------------------------------ */
static FILE *log_fp;
void plat_log(const char *text)
{
    if (log_fp) { fprintf(log_fp, "%s\n", text); fflush(log_fp); }
    fprintf(stderr, "%s\n", text);
}
static void logfmt(const char *fmt, ...)
{
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    plat_log(b);
}

/* ---- time, storage -------------------------------------------------------- */
double plat_now_ms(void)
{
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

int plat_nvram_read(void *buf, unsigned len)
{
    if (no_nvram) return 1;
    FILE *f = fopen(nv_path.c_str(), "rb");
    if (!f) return 1;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : 1;
}

int plat_nvram_write(const void *buf, unsigned len)
{
    if (no_nvram) return 0;
    FILE *f = fopen(nv_path.c_str(), "wb");
    if (!f) return 1;
    size_t n = fwrite(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : 1;
}

/* ---- audio ---------------------------------------------------------------- */
static SDL_AudioDeviceID adev;

int plat_audio_open(int sample_rate)
{
    if (turbo || hidden) return 1;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!adev) { logfmt("audio: %s", SDL_GetError()); return 1; }
    SDL_PauseAudioDevice(adev, 0);
    return 0;
}

void plat_audio_push(const int16_t *pcm, int frames)
{
    if (!adev) return;
    /* more than a quarter of a second waiting: the machine ran ahead, drop it */
    if (SDL_GetQueuedAudioSize(adev) > SW_AUDIO_RATE / 4 * sizeof(int16_t)) return;
    SDL_QueueAudio(adev, pcm, (Uint32)frames * sizeof(int16_t));
}

void plat_audio_close(void)
{
    if (adev) SDL_CloseAudioDevice(adev);
    adev = 0;
}

/* ---- video ----------------------------------------------------------------
 * A picture is the vector generator's lines.  SW3D gives each line end the
 * depth the game's perspective divide used for it (sw3d_capture.h), so the
 * picture can be drawn again from an eye that is somewhere else:
 *
 *   view 0  the picture as the game draws it (2D)
 *   view 1  3D, the eye moved: it sways by itself, or I J K L / U O move it
 *   view 2  3D, side by side for the two eyes
 *   view 3  3D, red/cyan glasses
 *   F6 next view, F7 colour by depth, [ ] eye separation, - = panel distance
 *
 * Lines with no perspective point at either end (scores, messages, the
 * cockpit, the sights) are on a flat panel at hud_depth.
 */
struct vline { float x0, y0, x1, y1; uint32_t rgb; int intensity; float d0, d1; };
struct pline { float x0, y0, x1, y1; uint32_t rgb; float a; };
static std::vector<vline> building, shown;
static std::vector<pline> proj;
static unsigned long pictures;
static SDL_Window *win;
static int   view_mode, depth_colour, sway = 1;
static float eye_sep = 38.0f, hud_depth = 600.0f;
static float eye_x, eye_y, eye_z;               /* view 1: forward, right, up, in the game's units */
static unsigned long n_deep, n_flat;            /* lines of the last picture with and without depth */

void plat_video_begin(void) { building.clear(); }

void plat_video_line(float x0, float y0, float x1, float y1, uint32_t rgb, int intensity)
{
    building.push_back({ x0, y0, x1, y1, rgb, intensity, 0.0f, 0.0f });
}

/* one eye's picture: every line unprojected with its depth and projected again
 * from an eye at (ex forward, ey right, ez up) */
static void project(float ex, float ey, float ez)
{
    const float nearx = 16.0f;
    proj.clear();
    for (const vline &l : shown) {
        float d0 = l.d0 > 0 ? l.d0 : hud_depth, d1 = l.d1 > 0 ? l.d1 : hud_depth;
        /* view space: X depth, Y right, Z up */
        float X0 = d0 - ex, Y0 = (l.x0 - SW3D_CX) / SW3D_KX * d0 / SW3D_FOCAL - ey,
              Z0 = (SW3D_CY - l.y0) / SW3D_KX * d0 / SW3D_FOCAL - ez;
        float X1 = d1 - ex, Y1 = (l.x1 - SW3D_CX) / SW3D_KX * d1 / SW3D_FOCAL - ey,
              Z1 = (SW3D_CY - l.y1) / SW3D_KX * d1 / SW3D_FOCAL - ez;
        if (X0 < nearx && X1 < nearx) continue;
        if (X0 < nearx) { float t = (nearx - X0) / (X1 - X0); X0 = nearx; Y0 += t * (Y1 - Y0); Z0 += t * (Z1 - Z0); }
        if (X1 < nearx) { float t = (nearx - X1) / (X0 - X1); X1 = nearx; Y1 += t * (Y0 - Y1); Z1 += t * (Z0 - Z1); }
        pline p;
        p.x0 = SW3D_CX + SW3D_KX * SW3D_FOCAL * Y0 / X0;
        p.y0 = SW3D_CY - SW3D_KX * SW3D_FOCAL * Z0 / X0;
        p.x1 = SW3D_CX + SW3D_KX * SW3D_FOCAL * Y1 / X1;
        p.y1 = SW3D_CY - SW3D_KX * SW3D_FOCAL * Z1 / X1;
        p.rgb = l.rgb;
        if (depth_colour) {                         /* near red, far blue; the flat panel grey */
            if (l.d0 > 0 || l.d1 > 0) {
                float t = logf((d0 + d1) * 0.5f / 200.0f) / logf(16000.0f / 200.0f);
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                int r = (int)(255 * (1 - t)), g = (int)(255 * (1 - fabsf(2 * t - 1))), b = (int)(255 * t);
                p.rgb = (uint32_t)(r | g << 8 | b << 16);
            } else p.rgb = 0x707070;
        }
        p.a = (float)l.intensity / 255.0f * brightness;
        if (p.a > 1.0f) p.a = 1.0f;
        proj.push_back(p);
    }
}

static void eye_for_view(int eye, float *ex, float *ey, float *ez)
{
    *ex = *ey = *ez = 0.0f;
    if (view_mode == 1) { *ex = eye_x; *ey = eye_y; *ez = eye_z; }
    else if (view_mode >= 2) *ey = (eye ? 0.5f : -0.5f) * eye_sep;
}

static void write_svg(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    int eyes = view_mode == 2 ? 2 : 1;
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d 280\" width=\"%d\" height=\"840\" "
               "preserveAspectRatio=\"none\"><rect width=\"%d\" height=\"280\" fill=\"black\"/>\n",
            250 * eyes, 1000 * eyes, 250 * eyes);
    for (int e = 0; e < eyes; e++) {
        float ex, ey, ez;
        eye_for_view(e, &ex, &ey, &ez);
        project(ex, ey, ez);
        fprintf(f, "<svg x=\"%d\" y=\"0\" width=\"250\" height=\"280\" viewBox=\"0 0 250 280\">\n", 250 * e);
        for (const pline &l : proj)
            fprintf(f, "<line x1=\"%g\" y1=\"%g\" x2=\"%g\" y2=\"%g\" stroke=\"#%02x%02x%02x\" stroke-opacity=\"%.2f\" "
                       "stroke-width=\"0.5\" stroke-linecap=\"round\"/>\n",
                    l.x0, l.y0, l.x1, l.y1, l.rgb & 255, (l.rgb >> 8) & 255, (l.rgb >> 16) & 255, fmin(1.0, 0.25 + l.a));
        fprintf(f, "</svg>\n");
    }
    fprintf(f, "</svg>\n");
    fclose(f);
}

void plat_video_present(void)
{
    /* give every line end its depth while this picture's points are still held */
    n_deep = n_flat = 0;
    for (vline &l : building) {
        l.d0 = sw3d_depth_at(l.x0, l.y0);
        l.d1 = sw3d_depth_at(l.x1, l.y1);
        if (l.d0 > 0 && l.d1 <= 0) l.d1 = l.d0;     /* one end clipped or joined to the panel */
        if (l.d1 > 0 && l.d0 <= 0) l.d0 = l.d1;
        if (l.d0 > 0) n_deep++; else n_flat++;
    }
    shown.swap(building);
    pictures++;
    if (svg_at && pictures == svg_at && !svg_path.empty()) write_svg(svg_path.c_str());
    if (dump_at && pictures == dump_at) {           /* testing: the math events and the picture, as text */
        FILE *f = fopen(dump_path.c_str(), "w");
        const sw3d_event *ev;
        int n = sw3d_events(&ev);
        for (int i = 0; f && i < n; i++) {
            const sw3d_event *e = &ev[i];
            fprintf(f, "E%lu %02X bic %03X div %d / %d = %d  pre %d %d %d | %d %d %d  post %d %d %d | %d %d %d  blk %d %d %d %d -> %d %d %d %d\n",
                    pictures - e->tag, e->prog, e->bic, e->dividend, e->divisor, e->quotient,
                    e->pre[0], e->pre[1], e->pre[2], e->pre[3], e->pre[4], e->pre[5],
                    e->post[0], e->post[1], e->post[2], e->post[3], e->post[4], e->post[5],
                    e->blk_pre[0], e->blk_pre[1], e->blk_pre[2], e->blk_pre[3],
                    e->blk_post[0], e->blk_post[1], e->blk_post[2], e->blk_post[3]);
        }
        for (const vline &l : shown)
            if (f) fprintf(f, "L %g %g %g %g %06x %d %g %g\n", l.x0, l.y0, l.x1, l.y1, l.rgb & 0xffffff, l.intensity, l.d0, l.d1);
        if (f) fclose(f);
    }
    sw3d_set_tag((uint32_t)pictures);
    if (pictures > 8) sw3d_drop_before((uint32_t)pictures - 8);
}

static void draw_proj(int mono)
{
    for (int pass = 0; pass < 2; pass++) {
        glBegin(pass ? GL_POINTS : GL_LINES);
        for (const pline &l : proj) {
            bool dot = fabsf(l.x0 - l.x1) < 0.01f && fabsf(l.y0 - l.y1) < 0.01f;
            if (dot != (pass == 1)) continue;
            if (mono) glColor4f(1, 1, 1, l.a);
            else glColor4f((l.rgb & 255) / 255.0f, ((l.rgb >> 8) & 255) / 255.0f, ((l.rgb >> 16) & 255) / 255.0f, l.a);
            glVertex2f(l.x0, l.y0);
            if (!dot) glVertex2f(l.x1, l.y1);
        }
        glEnd();
    }
}

static void draw(void)
{
    int w, h;
    SDL_GL_GetDrawableSize(win, &w, &h);
    glViewport(0, 0, w, h);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 250.0, 280.0, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POINT_SMOOTH);

    if (view_mode == 1 && sway) {                   /* the eye drifts in a slow circle */
        double t = plat_now_ms() / 1000.0;
        eye_y = 90.0f * (float)sin(t * 1.3);
        eye_z = 45.0f * (float)cos(t * 1.3);
    }

    int eyes = view_mode >= 2 ? 2 : 1;
    for (int e = 0; e < eyes; e++) {
        /* the tube is 4:3, landscape; letterbox it in the window (or in its half) */
        int aw = view_mode == 2 ? w / 2 : w, ax = view_mode == 2 ? e * (w / 2) : 0;
        int vw = aw, vh = aw * 3 / 4;
        if (vh > h) { vh = h; vw = h * 4 / 3; }
        glViewport(ax + (aw - vw) / 2, (h - vh) / 2, vw, vh);
        glEnable(GL_SCISSOR_TEST);
        glScissor(ax + (aw - vw) / 2, (h - vh) / 2, vw, vh);
        float lw = line_width * (float)vh / 840.0f;
        if (lw < 1.0f) lw = 1.0f;
        glLineWidth(lw);
        glPointSize(lw * 1.5f);
        if (view_mode == 3) glColorMask(e == 0, e == 1, e == 1, 1);     /* left red, right cyan */
        float ex, ey, ez;
        eye_for_view(e, &ex, &ey, &ez);
        project(ex, ey, ez);
        draw_proj(view_mode == 3);
        glDisable(GL_SCISSOR_TEST);
    }
    glColorMask(1, 1, 1, 1);
    SDL_GL_SwapWindow(win);
}

static void show_title(void)
{
    static const char *names[4] = { "2D (original)", "3D - moving eye", "3D - side by side", "3D - red/cyan glasses" };
    char t[200];
    snprintf(t, sizeof t, APP_NAME " - Star Wars - %s%s   [F6 view, F7 depth colours]", names[view_mode],
             depth_colour ? " - depth colours" : "");
    if (win) SDL_SetWindowTitle(win, t);
}

/* ---- input ---------------------------------------------------------------- */
static int    g_quit, captured = 1, sw_test, sw_snd_test, fullscreen;
static double yoke_x = 128.0, yoke_y = 128.0, last_poll_ms;
static int    mouse_dx, mouse_dy, mouse_l, mouse_r;

static void set_capture(int on)
{
    captured = on;
    if (win) SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
}

static void pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: g_quit = 1; break;
        case SDL_MOUSEMOTION:
            if (captured) { mouse_dx += e.motion.xrel; mouse_dy += e.motion.yrel; }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (e.type == SDL_MOUSEBUTTONDOWN && !captured) { set_capture(1); break; }
            if (e.button.button == SDL_BUTTON_LEFT)  mouse_l = e.type == SDL_MOUSEBUTTONDOWN;
            if (e.button.button == SDL_BUTTON_RIGHT) mouse_r = e.type == SDL_MOUSEBUTTONDOWN;
            break;
        case SDL_KEYDOWN:
            if (e.key.repeat) break;
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE: g_quit = 1; break;
            case SDLK_F2:  sw_test = !sw_test; break;
            case SDLK_F4:  sw_snd_test = !sw_snd_test; break;
            case SDLK_F8:  set_capture(!captured); break;
            case SDLK_F6:  view_mode = (view_mode + 1) % 4; show_title(); break;
            case SDLK_F7:  depth_colour = !depth_colour; show_title(); break;
            case SDLK_F5:  sway = !sway; eye_x = eye_y = eye_z = 0; break;
            case SDLK_LEFTBRACKET:  eye_sep = fmaxf(0.0f, eye_sep - 4.0f); break;
            case SDLK_RIGHTBRACKET: eye_sep += 4.0f; break;
            case SDLK_MINUS:  hud_depth = fmaxf(100.0f, hud_depth - 50.0f); break;
            case SDLK_EQUALS: hud_depth += 50.0f; break;
            case SDLK_F11:
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                break;
            default: break;
            }
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { mouse_l = mouse_r = 0; }
            break;
        default: break;
        }
    }
}

void plat_input_poll(plat_inputs *in)
{
    memset(in, 0, sizeof *in);
    in->pitch = in->yaw = 0x80;
    if (hidden) { in->quit = (uint8_t)g_quit; return; }

    const Uint8 *key = SDL_GetKeyboardState(NULL);
    double now = plat_now_ms();
    double dt = last_poll_ms > 0.0 ? now - last_poll_ms : 0.0;
    last_poll_ms = now;
    if (dt > 100.0) dt = 100.0;

    /* the mouse moves the yoke, which stays where it is put */
    yoke_x += (double)mouse_dx * mouse_sens;
    yoke_y += (double)mouse_dy * mouse_sens * (invert_pitch ? -1.0 : 1.0);
    mouse_dx = mouse_dy = 0;

    /* the arrow keys push it, and it springs back to centre when they are let go */
    int kx = (key[SDL_SCANCODE_RIGHT] ? 1 : 0) - (key[SDL_SCANCODE_LEFT] ? 1 : 0);
    int ky = (key[SDL_SCANCODE_DOWN] ? 1 : 0) - (key[SDL_SCANCODE_UP] ? 1 : 0);
    static int keys_had;
    double step = key_rate * dt / 1000.0;
    if (kx || ky) {
        keys_had = 1;
        yoke_x += kx * step;
        yoke_y += ky * step * (invert_pitch ? -1.0 : 1.0);
    } else if (keys_had) {
        double dx = 128.0 - yoke_x, dy = 128.0 - yoke_y;
        if (fabs(dx) <= step && fabs(dy) <= step) { yoke_x = yoke_y = 128.0; keys_had = 0; }
        else {
            yoke_x += dx > 0 ? fmin(step, dx) : fmax(-step, dx);
            yoke_y += dy > 0 ? fmin(step, dy) : fmax(-step, dy);
        }
    }
    if (yoke_x < 0.0) yoke_x = 0.0;
    if (yoke_x > 255.0) yoke_x = 255.0;
    if (yoke_y < 0.0) yoke_y = 0.0;
    if (yoke_y > 255.0) yoke_y = 255.0;
    in->yaw = (uint8_t)yoke_x;
    in->pitch = (uint8_t)yoke_y;

    if (view_mode == 1) {                            /* I J K L move the eye, U O push it forward and back */
        float mv = (float)(dt * 0.25);
        if (key[SDL_SCANCODE_J] || key[SDL_SCANCODE_L] || key[SDL_SCANCODE_I] || key[SDL_SCANCODE_K] ||
            key[SDL_SCANCODE_U] || key[SDL_SCANCODE_O]) sway = 0;
        eye_y += mv * ((key[SDL_SCANCODE_L] ? 1 : 0) - (key[SDL_SCANCODE_J] ? 1 : 0));
        eye_z += mv * ((key[SDL_SCANCODE_I] ? 1 : 0) - (key[SDL_SCANCODE_K] ? 1 : 0));
        eye_x += mv * ((key[SDL_SCANCODE_O] ? 1 : 0) - (key[SDL_SCANCODE_U] ? 1 : 0));
    }

    in->fire_r  = (uint8_t)(mouse_l || key[SDL_SCANCODE_LCTRL] || key[SDL_SCANCODE_1]);
    in->fire_l  = (uint8_t)(mouse_r || key[SDL_SCANCODE_LALT]);
    in->thumb_r = key[SDL_SCANCODE_SPACE];
    in->thumb_l = (uint8_t)(key[SDL_SCANCODE_LSHIFT] || key[SDL_SCANCODE_RSHIFT]);
    in->coin_l  = key[SDL_SCANCODE_5];
    in->coin_r  = key[SDL_SCANCODE_6];
    in->coin_aux = key[SDL_SCANCODE_7];
    in->slam    = key[SDL_SCANCODE_F3];
    in->diag    = key[SDL_SCANCODE_F1];
    in->test    = (uint8_t)sw_test;
    in->snd_test = (uint8_t)sw_snd_test;
    in->quit    = (uint8_t)g_quit;
}

/* ---- main ----------------------------------------------------------------- */
static int usage(void)
{
    fprintf(stderr,
        APP_NAME " - Star Wars (Atari, 1983), the C port, on SDL2\n"
        "  sw3d [--rev 1|2] [--romdir DIR] [--nvram FILE] [--no-nvram] [--fullscreen]\n"
        "  view:    [--view 0|1|2|3] (2D, 3D moving eye, side by side, red/cyan) [--depth-colours] [--eye FWD RIGHT UP]\n"
        "  testing: [--script FILE] [--quit-after-ms MS] [--turbo] [--hidden] [--svg N FILE] [--dump3d N FILE]\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_TIMER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    char *bp = SDL_GetBasePath();
    base_dir = bp ? bp : "./";
    SDL_free(bp);
    rom_dir = base_dir + "roms";
    nv_path = base_dir + "starwars.nv";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "--rev") && i + 1 < argc)    rev = atoi(argv[++i]);
        else if (!strcmp(a, "--romdir") && i + 1 < argc) rom_dir = argv[++i];
        else if (!strcmp(a, "--nvram") && i + 1 < argc)  nv_path = argv[++i];
        else if (!strcmp(a, "--no-nvram"))               no_nvram = 1;
        else if (!strcmp(a, "--fullscreen"))             fullscreen = 1;
        else if (!strcmp(a, "--script") && i + 1 < argc) script_path = argv[++i];
        else if (!strcmp(a, "--quit-after-ms") && i + 1 < argc) quit_after_ms = atof(argv[++i]);
        else if (!strcmp(a, "--turbo"))                  turbo = 1;
        else if (!strcmp(a, "--hidden"))                 hidden = 1;
        else if (!strcmp(a, "--view") && i + 1 < argc)   view_mode = atoi(argv[++i]) & 3;
        else if (!strcmp(a, "--depth-colours"))          depth_colour = 1;
        else if (!strcmp(a, "--eye") && i + 3 < argc)    { sway = 0; eye_x = (float)atof(argv[++i]); eye_y = (float)atof(argv[++i]); eye_z = (float)atof(argv[++i]); }
        else if (!strcmp(a, "--dump3d") && i + 2 < argc) { dump_at = strtoul(argv[++i], NULL, 0); dump_path = argv[++i]; sw3d_set_debug(1); }
        else if (!strcmp(a, "--svg") && i + 2 < argc)    { svg_at = strtoul(argv[++i], NULL, 0); svg_path = argv[++i]; }
        else return usage();
    }
    if (turbo) hidden = 1;
    if (turbo || hidden || !script_path.empty()) no_nvram = 1;

    log_fp = fopen((base_dir + "sw3d.log").c_str(), "w");

    if (!hidden) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { logfmt("SDL: %s", SDL_GetError()); return 1; }
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        win = SDL_CreateWindow(APP_NAME " - Star Wars", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 900, flags);
        if (!win) {                                 /* no multisampling here: try without */
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
            win = SDL_CreateWindow(APP_NAME " - Star Wars", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 900, flags);
        }
        if (!win) { logfmt("window: %s", SDL_GetError()); return 1; }
        if (!SDL_GL_CreateContext(win)) { logfmt("OpenGL: %s", SDL_GetError()); return 1; }
        SDL_GL_SetSwapInterval(1);
        logfmt("OpenGL %s on %s", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
        set_capture(1);
        show_title();
    }

    sw_app_opts o;
    memset(&o, 0, sizeof o);
    o.rev = rev;
    o.rom_dir = rom_dir.c_str();
    o.script = script_path.empty() ? NULL : script_path.c_str();
    o.turbo = turbo;
    o.pokey_volume = 100;
    o.speech_volume = 100;
    logfmt("machine: %s, ROM revision %d, images in %s", sw_app_name(), o.rev, o.rom_dir);
    if (sw_app_init(&o)) {
        logfmt("%s", sw_app_error());
        if (win) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, sw_app_error(), win);
        return 1;
    }

    sw_app_stats st;
    double t_start = plat_now_ms(), machine_ms = 0.0;
    while (!g_quit) {
        double now;
        if (!hidden) pump_events();
        if (turbo) {
            sw_app_run(0.0);
            sw_app_get_stats(&st);
            machine_ms = (double)st.cycles / 1512.0;
            now = t_start + machine_ms;             /* --quit-after-ms counts machine time */
        } else {
            now = plat_now_ms();
            sw_app_run(now);
            sw_app_get_stats(&st);
        }
        if (st.script_ended) g_quit = 1;
        if (quit_after_ms > 0.0 && now - t_start >= quit_after_ms) g_quit = 1;
        if (!hidden) draw();                        /* vsync paces the loop */
        else if (!turbo) SDL_Delay(1);
    }

    sw_app_get_stats(&st);
    double wall = (plat_now_ms() - t_start) / 1000.0;
    logfmt("%s: %lu passes, %llu cycles (%.3f s of machine time) in %.3f s; %lu pictures, %d vectors in the last; "
           "%lu irqs (%lu lost), %lu resets, %lu watchdog bites, %lu swi",
           sw_app_name(), st.passes, (unsigned long long)st.cycles, (double)st.cycles / 1512000.0, wall,
           st.pictures, st.points, st.irqs, st.irqs_lost, st.resets, st.watchdog_bites, st.swi);
    sw_app_exit();
    if (log_fp) fclose(log_fp);
    SDL_Quit();
    return 0;
}
