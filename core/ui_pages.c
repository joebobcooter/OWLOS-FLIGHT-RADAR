/* the other pages: the list of flights, setup, the touch alignment */
#include "ui_int.h"
#include <stdlib.h>

/* --- the list: every plane, nearest first --- */

/* the columns were laid out on 320 wide, wider screens spread them. the round glass has room
 * for less: no column names, the name, the height and how far */
#define LX(x) ((x) * GFX_W / 320)
#define LIST_HEAD (RADAR_ROUND ? 0 : 14)

static int list_y(const ui_t *u, int k) { return LIST_Y0 + LIST_HEAD + k * LIST_ROW - u->list_scroll; }

int list_hit(const ui_t *u, int x, int y)
{
    (void)x;
    int b = bar_hit(x, y, 2);
    if (b >= 0) return HIT_BAR + b;
    if (y < LIST_Y0 + LIST_HEAD || y >= LIST_Y1) return HIT_NONE;
    int k = (y - LIST_Y0 - LIST_HEAD + u->list_scroll) / LIST_ROW;
    return k >= 0 && k < u->n_order ? HIT_ROW + k : HIT_NONE;
}

void list_draw(ui_t *u, canvas_t *cv)
{
    char buf[24];
    canvas_t lc;
    if (gfx_sub(cv, LIST_Y0 + LIST_HEAD, LIST_Y1 - 1, &lc)) {
        for (int k = 0; k < u->n_order; k++) {
            int y = list_y(u, k);
            if (!gfx_touches(&lc, y, y + LIST_ROW)) continue;
            const plane_t *p = &u->snap.p[u->order[k]];
            int pressed = u->t_down && !u->t_moved && u->t_hit == HIT_ROW + k;
            if (pressed || p->icao == u->sel) gfx_fill(&lc, PG_L + 4, y, PG_R - PG_L - 8, LIST_ROW - 2, C_PRIM, pressed ? 70 : 35);
            gfx_hline(&lc, PG_L + 6, PG_R - 7, y + LIST_ROW - 1, C_QUIET, 70);
            uint32_t col = alt_colour(u, p);
            silhouette(&lc, p->kind, PG_L + 14.0f, y + 10.0f, p->track, 0.9f, col, 255);
            char d[16];
#if RADAR_ROUND
            font_text_clip(&lc, &FONT_SMALL, PG_L + 26, y + 6, p->flight[0] ? p->flight : p->reg, C_FOCUS, 250, 58);
            fmt_alt(u, p, buf, sizeof buf, 1);
            font_text(&lc, &FONT_SMALL, PG_L + 88, y + 6, buf, col, 245);
            fmt_dist(u, p->dist, d, sizeof d);
            char *sp = strchr(d, ' ');
            if (sp) *sp = 0;                            // the number, the unit is in the title
            font_text(&lc, &FONT_SMALL, PG_R - 6 - font_width(&FONT_SMALL, d), y + 6, d, C_DIM, 235);
            continue;
#endif
            font_text_clip(&lc, &FONT_MEDIUM, 28, y + 4, p->flight[0] ? p->flight : p->reg, C_FOCUS, 250, LX(102) - 32);
            font_text_clip(&lc, &FONT_SMALL, LX(102), y + 6, p->type[0] ? p->type : "----", C_DIM, 235, 34);
            fmt_alt(u, p, buf, sizeof buf, 1);
            font_text(&lc, &FONT_SMALL, LX(140), y + 6, buf, col, 245);
            snprintf(buf, sizeof buf, p->has_spd ? "%d KT" : "--", (int)lroundf(p->gs));
            font_text(&lc, &FONT_SMALL, LX(190), y + 6, buf, C_PRIM, 235);
            fmt_dist(u, p->dist, d, sizeof d);
            snprintf(buf, sizeof buf, "%s %s", d, compass8(p->brg));
            font_text(&lc, &FONT_SMALL, GFX_W - 8 - font_width(&FONT_SMALL, buf), y + 6, buf, C_DIM, 235);
        }
        if (!u->n_order) micro_text(&lc, PG_L + 12, LIST_Y0 + 24, "NOTHING IN RANGE", C_DIM, 230, RADAR_ROUND ? 1 : 2);
    }
    char d[16], right[24];
    fmt_dist(u, range_km(&u->cfg), d, sizeof d);
    snprintf(right, sizeof right, "%d IN %s", u->n_order, d);
    page_header(cv, "FLIGHTS", right, C_FOCUS);
    if (!RADAR_ROUND && gfx_touches(cv, LIST_Y0, LIST_Y0 + 12)) {
        static const char *const H[5] = { "FLIGHT", "TYPE", "HEIGHT", "SPEED", "DISTANCE" };
        const int X[5] = { 28, LX(102), LX(140), LX(190), 0 };
        for (int i = 0; i < 4; i++) micro_text(cv, X[i], LIST_Y0 + 4, H[i], C_QUIET, 230, 1);
        micro_text(cv, GFX_W - 8 - micro_width(H[4], 1), LIST_Y0 + 4, H[4], C_QUIET, 230, 1);
    }
    if (RADAR_ROUND) snprintf(right, sizeof right, "ZOOM");            // the round bar is short
    else snprintf(right, sizeof right, "ZOOM %s", d);
    const char *L[2] = { right, "BACK" };
    bar(u, cv, L, 2, -1);
}

/* --- setup: one row a setting, what it does under its name. the rows sit in tabs: how it looks,
 * what it shows and where from, the LED. the tabs are the bottom bar's buttons, BACK the last --- */

enum { S_RANGE, S_GROUND, S_WATCH, S_DIST, S_ALT, S_UP, S_FLIP, S_LABELS, S_THEME, S_LOOK, S_MAP,
       S_FL_LO, S_FL_HI, S_BLOCK, S_AUTOPILOT, S_VECTORS, S_BEAM, S_CLOCK, S_LOST, S_CONFLICT,
       S_SOURCE, S_SERVER, S_HOME, S_WIFI,
#if RADAR_HAS_LED
       S_LED_SOS, S_LED_RANGE, S_LED_UP, S_LED_DOWN, S_LED_NEAR, S_LED_NEAR_KM, S_LED_NODATA, S_LED_LEVEL, S_LED_HOLD,
       S_LED_TEST,
#if RADAR_LED_RED_PIN
       S_LED_RED,
#endif
#endif
       S_TOUCH, S_ABOUT, S_ROWS };
enum { K_NUM, K_BTN, K_INFO };
/* the tabs, in the order of the bar. the round glass's bar has no room for a fourth and it has no
 * gear: the radar's rows go under VIEW there */
#if RADAR_ROUND
enum { T_VIEW, T_DATA, T_LED, T_RADAR = T_VIEW };
#define TABS 2
#else
enum { T_VIEW, T_DATA, T_RADAR, T_LED };
#define TABS (RADAR_HAS_LED ? 4 : 3)
#endif

static const struct { const char *label, *desc; int kind, tab; } ROWS[S_ROWS] = {
    { "RANGE",       "or drag inside the scope",     K_NUM, T_VIEW },
    { "ON GROUND",   "planes parked or taxiing",     K_BTN, T_DATA },
    { "WATCH",       "",                             UI_WIFI_PAGE ? K_BTN : K_INFO, T_DATA },   /* planes to follow */
    { "DISTANCE",    "km, nautical miles or miles",  K_BTN, T_VIEW },
    { "ALTITUDE",    "feet or metres",               K_BTN, T_VIEW },
    { "MAP UP",      "or drag the scope's rim",      K_NUM, T_VIEW },
    { "SCREEN",      "mounted upside down?",         K_BTN, T_VIEW },
    { "LABELS",      "names beside the planes",      K_BTN, T_VIEW },
    { "COLOURS",     "five colour themes",           K_BTN, T_VIEW },
    { "LOOK",        "radar, flat map, map's colours", K_BTN, T_VIEW },
    { "MAP",         "OpenStreetMap under the scope", K_BTN, T_VIEW },
    { "LOWEST",      "planes under this are hidden", K_NUM, T_RADAR },
    { "HIGHEST",     "planes over this are hidden",  K_NUM, T_RADAR },
    { "DATA BLOCK",  "labels: climbing and speed",   K_BTN, T_RADAR },
    { "AUTOPILOT",   "labels: the height it is set to", K_BTN, T_RADAR },
    { "VECTORS",     "a line ahead of each plane",   K_BTN, T_RADAR },
    { "BEAM STEPS",  "planes move as the beam passes", K_BTN, T_RADAR },
    { "CLOCK",       "the time in UTC",              K_BTN, T_RADAR },
    { "LOST PLANES", "dim when no fresh position",   K_BTN, T_RADAR },
    { "CONFLICT",    "two planes closing turn red",  K_BTN, T_RADAR },
    { "DATA",        "free, or FR24 with a key",     K_BTN, T_DATA },
    { "FREE SERVER", "tried first, others back up",  K_BTN, T_DATA },
    { "HOME",        "",                             UI_WIFI_PAGE ? K_BTN : K_INFO, T_DATA },   /* typed on the WiFi keyboard */
    { "WIFI",        "",                             UI_WIFI_PAGE ? K_BTN : K_INFO, T_DATA },
#if RADAR_HAS_LED
    /* the LED on the back (core/light.h): each event on or off, in the order they win */
    { "SOS",         "7500 7600 7700 round home: red", K_BTN, T_LED },
    { "IN RANGE",    "a watched plane comes: green", K_BTN, T_LED },
    { "TAKE-OFF",    "a watched plane takes off: blue", K_BTN, T_LED },
    { "LANDING",     "a watched plane lands: cyan",  K_BTN, T_LED },
    { "ANY NEAR",    "any plane in the air near: white", K_BTN, T_LED },
    { "HOW NEAR",    "from home, for ANY NEAR",      K_BTN, T_LED },
    { "NO DATA",     "no WiFi or no answer: amber",  K_BTN, T_LED },
    { "BRIGHTNESS",  "low for the night",            K_BTN, T_LED },
    { "HOW LONG",    "till touched, 10 s, while it lasts", K_BTN, T_LED },
    { "TEST",        "red, green, blue in turn",     K_BTN, T_LED },
#if RADAR_LED_RED_PIN
    { "RED ON",      "no red in TEST: the other",    K_BTN, T_LED },   /* GPIO 4 or 22: light.h */
#endif
#endif
    { "TOUCH",       "align the touch screen",       K_BTN, T_VIEW },
    { "ABOUT",       "",                             RADAR_HAS_TOUCH ? K_BTN : K_INFO, T_DATA },   /* the version, the site */
};

#define SB_X  (PG_R - 84)                     /* the keys of a row */
#define SB_W  38
#define SK_H  (RADAR_ROUND ? 26 : 30)
#define SK_Y  ((ROW_H - SK_H) / 2)

// the k-th row seen in the tab shown: its S_ number, -1 past the last
static int row_at(const ui_t *u, int k)
{
    for (int i = 0; i < S_ROWS; i++) if (ROWS[i].tab == u->setup_tab && k-- == 0) return i;
    return -1;
}

int setup_rows(const ui_t *u)
{
    int n = 0;
    for (int i = 0; i < S_ROWS; i++) n += ROWS[i].tab == u->setup_tab;
    return n;
}

static int row_y(const ui_t *u, int k) { return LIST_Y0 + k * ROW_H - u->setup_scroll; }

int setup_hit(const ui_t *u, int x, int y)
{
    int b = bar_hit(x, y, TABS + 1);
    if (b >= 0) return HIT_BAR + b;
    if (y < LIST_Y0 || y > BAR_Y - 3) return HIT_NONE;
    int i = row_at(u, (y - LIST_Y0 + u->setup_scroll) / ROW_H);
    if (i < 0 || x < SB_X - 4) return HIT_NONE;
    if (ROWS[i].kind == K_NUM) return x < SB_X + SB_W + 2 ? HIT_SDEC + i : HIT_SINC + i;
    if (ROWS[i].kind == K_BTN) return HIT_SBTN + i;
    return HIT_NONE;
}

int setup_repeats(int hit) { return hit >= HIT_SDEC && hit < HIT_SBTN; }

#if RADAR_HAS_LED
// the LED event a row switches on and off, -1 for the rows that are not one
static int led_event(int i)
{
    switch (i) {
    case S_LED_SOS:    return LIGHT_SOS;
    case S_LED_RANGE:  return LIGHT_RANGE;
    case S_LED_UP:     return LIGHT_UP;
    case S_LED_DOWN:   return LIGHT_DOWN;
    case S_LED_NEAR:   return LIGHT_NEAR;
    case S_LED_NODATA: return LIGHT_NODATA;
    default:           return -1;
    }
}
#endif

static void value_text(const ui_t *u, int i, char *b, int sz)
{
    const fr_cfg_t *c = &u->cfg;
    static const char *const DU[3] = { "KM", "NM", "MI" };
    b[0] = 0;
    switch (i) {
    case S_RANGE:  fmt_dist(u, range_km(c), b, sz); break;
    case S_GROUND: snprintf(b, (size_t)sz, c->ground ? "SHOW" : "HIDE"); break;
    case S_DIST:   snprintf(b, (size_t)sz, "%s", DU[c->dist_unit < 3 ? c->dist_unit : 0]); break;
    case S_ALT:    snprintf(b, (size_t)sz, c->alt_unit ? "M" : "FT"); break;
    case S_UP:     if (c->up) snprintf(b, (size_t)sz, "%03d\xC2\xB0", c->up); else snprintf(b, (size_t)sz, "NORTH"); break;
    case S_FLIP:   snprintf(b, (size_t)sz, c->flip ? "FLIPPED" : "NORMAL"); break;
    case S_LABELS: snprintf(b, (size_t)sz, c->labels ? "PICKED" : "ALL"); break;
    case S_THEME:  snprintf(b, (size_t)sz, "%s", UI_THEME[u->theme].name); break;
    case S_LOOK:   snprintf(b, (size_t)sz, "%s", LOOK_NAME[c->look < LOOKS ? c->look : 0]); break;
    case S_MAP:    snprintf(b, (size_t)sz, c->map ? "ON" : "OFF"); break;
    case S_FL_LO:
    case S_FL_HI: {
        int fl = i == S_FL_LO ? u->opt.fl_lo : u->opt.fl_hi;
        if (fl) fmt_height(u, fl * 100, b, sz);
        else snprintf(b, (size_t)sz, "ALL");
        break;
    }
    case S_BLOCK:     snprintf(b, (size_t)sz, u->opt.block ? "ON" : "OFF"); break;
    case S_AUTOPILOT: snprintf(b, (size_t)sz, u->opt.autopilot ? "ON" : "OFF"); break;
    case S_VECTORS:   snprintf(b, (size_t)sz, u->opt.vectors ? "ON" : "OFF"); break;
    case S_BEAM:      snprintf(b, (size_t)sz, u->opt.beam_step ? "ON" : "OFF"); break;
    case S_CLOCK:     snprintf(b, (size_t)sz, u->opt.clock ? "ON" : "OFF"); break;
    case S_LOST:      snprintf(b, (size_t)sz, u->opt.lost ? "ON" : "OFF"); break;
    case S_CONFLICT:  snprintf(b, (size_t)sz, u->opt.conflict ? "ON" : "OFF"); break;
    case S_SOURCE: snprintf(b, (size_t)sz, c->source == SRC_FR24 ? "FR24" : "FREE"); break;
    case S_SERVER: snprintf(b, (size_t)sz, "%s", SERVER_NAME[c->server < FREE_SERVERS ? c->server : 0]); break;
    case S_TOUCH:  snprintf(b, (size_t)sz, "ALIGN"); break;
    case S_WIFI:   snprintf(b, (size_t)sz, u->net.configured ? "CHANGE" : "SET UP"); break;
    case S_HOME:   snprintf(b, (size_t)sz, "CHANGE"); break;
    case S_WATCH:  snprintf(b, (size_t)sz, u->watch.n ? "CHANGE" : "SET UP"); break;
    case S_ABOUT:  if (ui_update(u)) snprintf(b, (size_t)sz, "NEW %s", u->newest); else snprintf(b, (size_t)sz, "OPEN"); break;
#if RADAR_HAS_LED
    case S_LED_SOS: case S_LED_RANGE: case S_LED_UP: case S_LED_DOWN: case S_LED_NEAR: case S_LED_NODATA:
        snprintf(b, (size_t)sz, u->light.on[led_event(i)] ? "ON" : "OFF");
        break;
    case S_LED_NEAR_KM: snprintf(b, (size_t)sz, "%d %s", light_near_units(&u->light), DU[c->dist_unit < 3 ? c->dist_unit : 0]); break;
    case S_LED_LEVEL: {
        static const char *const L[LIGHT_LEVELS] = { "LOW", "MID", "HIGH" };
        snprintf(b, (size_t)sz, "%s", L[u->light.level < LIGHT_LEVELS ? u->light.level : LIGHT_MID]);
        break;
    }
    case S_LED_HOLD: {
        static const char *const H[LIGHT_HOLDS] = { "TOUCH", "10 S", "WHILE" };
        snprintf(b, (size_t)sz, "%s", H[u->light.hold < LIGHT_HOLDS ? u->light.hold : LIGHT_TOUCH]);
        break;
    }
    case S_LED_TEST: snprintf(b, (size_t)sz, "TEST"); break;
#if RADAR_LED_RED_PIN
    case S_LED_RED:  snprintf(b, (size_t)sz, "GPIO %d", light_red_gpio(&u->light)); break;
#endif
#endif
    default: break;
    }
}

static void small_key(ui_t *u, canvas_t *cv, int x, int y, int hit, int plus)
{
    int pressed = u->t_down && !u->t_moved && u->t_hit == hit;
    chamfer_box(cv, x, y, SB_W, SK_H, pressed ? C_PRIM : C_PANEL, pressed ? 150 : 170, pressed ? C_FOCUS : C_PRIM, 210);
    float cx = x + SB_W / 2.0f, cy = y + SK_H / 2.0f;
    uint32_t c = pressed ? C_FOCUS : C_PRIM;
    gfx_line(cv, cx - 7, cy, cx + 7, cy, 2.4f, c, 255);
    if (plus) gfx_line(cv, cx, cy - 7, cx, cy + 7, 2.4f, c, 255);
}

void setup_draw(ui_t *u, canvas_t *cv)
{
    const int y1 = BAR_Y - 3;
    canvas_t lc;
    if (gfx_sub(cv, LIST_Y0, y1, &lc)) {
        char v[32], d[112];
        for (int k = 0, i; (i = row_at(u, k)) >= 0; k++) {
            int y = row_y(u, k);
            if (!gfx_touches(&lc, y, y + ROW_H)) continue;
            gfx_hline(&lc, PG_L + 6, PG_R - 7, y + ROW_H - 1, C_QUIET, 90);
            font_text(&lc, RADAR_ROUND ? &FONT_SMALL : &FONT_MEDIUM, PG_L + 10, y + 4, ROWS[i].label,
                      ROWS[i].kind == K_INFO ? C_DIM : C_PRIM, 255);
            const char *desc = ROWS[i].desc;
            if (i == S_HOME) {
                if (u->cfg.place[0]) snprintf(d, sizeof d, "%s%s%s, %.3f %.3f", u->cfg.place, u->cfg.country[0] ? ", " : "",
                                              u->cfg.country, (double)u->cfg.lat, (double)u->cfg.lon);
                else snprintf(d, sizeof d, UI_WIFI_PAGE ? "%.4f %.4f" : "%.4f %.4f, from the site", (double)u->cfg.lat,
                              (double)u->cfg.lon);
                desc = d;
            } else if (i == S_WIFI) {
                if (!u->net.configured) snprintf(d, sizeof d, UI_WIFI_PAGE ? "not set: SET UP, or from the site" : "not set: send it from the site");
                else snprintf(d, sizeof d, "%s, %s", u->net.ssid, u->net.wifi ? u->net.ip : "not connected");
                desc = d;
            } else if (i == S_WATCH) {                  // what the planes on it are doing
                const watchlist_t *w = &u->watch;
                if (!w->n) snprintf(d, sizeof d, UI_WIFI_PAGE ? "planes to follow: none" : "from the site");
                else snprintf(d, sizeof d, RADAR_ROUND ? "%d: %d flying, %d near" : "%d set, %d flying, %d in range", w->n,
                              watch_count(w, WATCH_AIR), watch_near(w));   // the round glass has a short row
                desc = d;
            } else if (i == S_ABOUT) {                  // the version, and what the site has
                const char *v = ui_version ? ui_version : "?";
                int n = !u->newest[0] ? snprintf(d, sizeof d, "version %s", v)
                      : ui_update(u) ? snprintf(d, sizeof d, "version %s, %s on the site", v, u->newest)
                      : snprintf(d, sizeof d, "version %s, the newest", v);
                if (!RADAR_HAS_TOUCH && n > 0 && n < (int)sizeof d)   // no ABOUT to open: the key here
                    snprintf(d + n, sizeof d - (size_t)n, ", FR24 key %s", u->net.has_key ? "stored" : "none");
                desc = d;
            }
            value_text(u, i, v, sizeof v);
#if RADAR_ROUND
            /* the round glass: no room for what a row does. under the name: a number's value,
             * or what an information row says */
            const char *under = ROWS[i].kind == K_NUM ? v : ROWS[i].kind == K_INFO ? desc : NULL;
            if (under) font_text_clip(&lc, &FONT_SMALL, PG_L + 10, y + 17, under, ROWS[i].kind == K_NUM ? C_FOCUS : C_DIM, 240,
                                      ROWS[i].kind == K_INFO ? PG_R - PG_L - 20 : SB_X - PG_L - 14);
#else
            font_text_clip(&lc, &FONT_SMALL, 10, y + 22, desc, C_DIM, 235, (ROWS[i].kind == K_INFO ? GFX_W - 20 : SB_X - 16));
            if (ROWS[i].kind == K_NUM) font_text(&lc, &FONT_MEDIUM, SB_X - 8 - font_width(&FONT_MEDIUM, v), y + 6, v, C_FOCUS, 250);
#endif
            if (ROWS[i].kind == K_NUM) {
                small_key(u, &lc, SB_X, y + SK_Y, HIT_SDEC + i, 0);
                small_key(u, &lc, SB_X + SB_W + 4, y + SK_Y, HIT_SINC + i, 1);
            } else if (ROWS[i].kind == K_BTN) {
                /* a switch that is on is lit: ON and OFF in the same box read as nothing happened */
                int pressed = u->t_down && !u->t_moved && u->t_hit == HIT_SBTN + i, on = !strcmp(v, "ON");
                button_font(&lc, SB_X, y + SK_Y, 2 * SB_W + 4, SK_H, v, pressed ? 2 : on, &FONT_SMALL);
            }
        }
        int total = setup_rows(u) * ROW_H, view = y1 - LIST_Y0;
        if (total > view) {                            // a tab that fits has no scroll bar
            int th = view * view / total, ty = LIST_Y0 + (view - th) * u->setup_scroll / (total - view);
            gfx_fill(&lc, PG_R - 3, ty, 2, th, C_QUIET, 200);
        }
    }
    page_header(cv, "SETUP", RADAR_ROUND ? NULL : "DRAG TO SCROLL", C_FOCUS);
    static const char *const L[] = { "VIEW", "DATA",
#if !RADAR_ROUND
                                     "RADAR",
#endif
#if RADAR_HAS_LED
                                     "LED",
#endif
                                     "BACK" };
    bar(u, cv, L, TABS + 1, u->setup_tab);
}

void setup_open_radar(ui_t *u)
{
    u->view = VIEW_SETUP;
    u->setup_tab = T_RADAR;
    u->setup_scroll = 0;
}

// a step of ten flight levels. HIGHEST's "no limit" is above its top, LOWEST's under its bottom
static int16_t fl_step(int fl, int dir, int high)
{
    int v = fl ? fl : high ? SCOPE_FL_MAX + 10 : 0;
    v += dir * 10;
    if (v > SCOPE_FL_MAX) return (int16_t)(high ? 0 : SCOPE_FL_MAX);
    if (v < 10) return (int16_t)(high ? 10 : 0);
    return (int16_t)v;
}

void setup_act(ui_t *u, int hit, int longp, ui_cmd_t *cmd)
{
    if (hit >= HIT_BAR && hit < HIT_BAR + TABS) {     // a tab, from its top
        u->setup_tab = hit - HIT_BAR;
        u->setup_scroll = 0;
        return;
    }
    if (hit == HIT_BAR + TABS) { if (!longp) u->view = VIEW_MAIN; return; }
    int i, dir = 0;
    if (hit >= HIT_SBTN) i = hit - HIT_SBTN;
    else if (hit >= HIT_SINC) { i = hit - HIT_SINC; dir = 1; }
    else if (hit >= HIT_SDEC) { i = hit - HIT_SDEC; dir = -1; }
    else return;
    if (i < 0 || i >= S_ROWS || longp) return;
    fr_cfg_t *c = &u->cfg;
    switch (i) {
    case S_RANGE:
        c->range = range_step(c->range, dir);
        cmd->refetch = 1;
        break;
    case S_GROUND:                                     // the fetch task takes them out of the sky
        c->ground ^= 1;
        cmd->refetch = 1;
        break;
    case S_DIST: {                                     // the scope keeps about its size in the new unit
        float km = range_km(c);
        c->dist_unit = (uint8_t)((c->dist_unit + 1) % 3);
        c->range = range_snap(km / unit_km(c->dist_unit));
        cmd->refetch = 1;
        break;
    }
    case S_ALT:    c->alt_unit ^= 1; break;
    case S_UP:     c->up = (int16_t)(((c->up + dir * 15) % 360 + 360) % 360); break;
    case S_FLIP:
        c->flip ^= 1;
        toast(u, c->flip ? "SCREEN FLIPPED" : "SCREEN NORMAL");
        break;
    case S_LABELS: c->labels ^= 1; break;
    case S_THEME:
        c->theme = (uint8_t)((u->theme + 1) % UI_THEMES);
        ui_set_theme(u, c->theme);
        break;
    case S_LOOK:   c->look = (uint8_t)((c->look + 1) % LOOKS); cmd->refetch = 1; break;
    case S_MAP:    c->map ^= 1; break;
    case S_FL_LO:                                      // the two never cross
        u->opt.fl_lo = fl_step(u->opt.fl_lo, dir, 0);
        if (u->opt.fl_hi && u->opt.fl_lo > u->opt.fl_hi) u->opt.fl_lo = u->opt.fl_hi;
        cmd->opt = 1;
        return;
    case S_FL_HI:
        u->opt.fl_hi = fl_step(u->opt.fl_hi, dir, 1);
        if (u->opt.fl_hi && u->opt.fl_lo > u->opt.fl_hi) u->opt.fl_hi = u->opt.fl_lo;
        cmd->opt = 1;
        return;
    case S_BLOCK:     u->opt.block ^= 1; cmd->opt = 1; return;
    case S_AUTOPILOT: u->opt.autopilot ^= 1; cmd->opt = 1; return;
    case S_VECTORS:   u->opt.vectors ^= 1; cmd->opt = 1; return;
    case S_BEAM:      u->opt.beam_step ^= 1; cmd->opt = 1; return;
    case S_CLOCK:     u->opt.clock ^= 1; cmd->opt = 1; return;
    case S_LOST:      u->opt.lost ^= 1; cmd->opt = 1; return;
    case S_CONFLICT:  u->opt.conflict ^= 1; cmd->opt = 1; return;
    case S_SOURCE:
        if (c->source == SRC_FREE && !u->net.has_key) { toast(u, "NO FR24 KEY: SEND IT FROM THE SITE"); return; }
        c->source = c->source == SRC_FREE ? SRC_FR24 : SRC_FREE;
        cmd->refetch = 1;
        break;
    case S_SERVER: c->server = (uint8_t)((c->server + 1) % FREE_SERVERS); cmd->refetch = 1; break;
    case S_TOUCH:  ui_start_calibration(u); return;
    case S_WIFI:   wifi_open(u, cmd); return;
    case S_HOME:   home_open(u); return;
    case S_WATCH:  watch_open(u); return;
    case S_ABOUT:  about_open(u); return;
#if RADAR_HAS_LED
    case S_LED_SOS: case S_LED_RANGE: case S_LED_UP: case S_LED_DOWN: case S_LED_NEAR: case S_LED_NODATA:
        u->light.on[led_event(i)] ^= 1;
        cmd->light = 1;                                // the LED's own settings, not fr_cfg_t
        return;
    case S_LED_NEAR_KM: u->light.near = (uint8_t)((u->light.near + 1) % LIGHT_NEARS); cmd->light = 1; return;
    case S_LED_LEVEL: u->light.level = (uint8_t)((u->light.level + 1) % LIGHT_LEVELS); cmd->light = 1; cmd->light_test = 1; return;
    case S_LED_HOLD:  u->light.hold = (uint8_t)((u->light.hold + 1) % LIGHT_HOLDS); cmd->light = 1; return;
    case S_LED_TEST:
        cmd->light_test = 1;
        toast(u, "LED: RED, GREEN, BLUE");
        return;
#if RADAR_LED_RED_PIN
    case S_LED_RED:                                    // the other pin, and the test shows if it is red now
        u->light.red = (uint8_t)((u->light.red + 1) % LIGHT_REDS);
        cmd->light = 1;
        cmd->light_test = 1;
        toast(u, "LED: RED, GREEN, BLUE");
        return;
#endif
#endif
    default: return;
    }
    cmd->cfg = 1;
}

/* --- touch alignment --- */

void cal_draw(ui_t *u, canvas_t *cv)
{
    char buf[32];
    int s = u->cal_step < CAL_POINTS ? u->cal_step : CAL_POINTS - 1;
    float x = (float)CAL_PT[s][0], y = (float)CAL_PT[s][1];
    float f = (float)(u->now % 1000u) / 1000.0f;
    gfx_ring(cv, x, y, 6.0f + 14.0f * f, 1.2f, C_FOCUS, (int)(200 * (1.0f - f)));
    gfx_line(cv, x - 14, y, x + 14, y, 1.2f, C_FOCUS, 240);
    gfx_line(cv, x, y - 14, x, y + 14, 1.2f, C_FOCUS, 240);
    gfx_ring(cv, x, y, 6.0f, 1.2f, C_PRIM, 240);
    if (u->t_down) gfx_disc(cv, x, y, 5.0f, C_FOCUS, 200);
    int ty = CAL_PT[s][1] <= GFX_H / 2 ? GFX_H / 2 + 26 : GFX_H / 2 - 66;
    const char *t1 = "TOUCH ALIGNMENT";
    micro_text(cv, GFX_W / 2 - micro_width(t1, 2) / 2, ty, t1, C_FOCUS, 250, 2);
    const char *t2 = "PRESS AND HOLD THE CENTRE OF THE CROSS";
    micro_text(cv, GFX_W / 2 - micro_width(t2, 1) / 2, ty + 20, t2, C_DIM, 235, 1);
    snprintf(buf, sizeof buf, "POINT %d OF %d", s + 1, CAL_POINTS);
    micro_text(cv, GFX_W / 2 - micro_width(buf, 1) / 2, ty + 34, buf, C_PRIM, 240, 1);
    if (ui_screen_name) {
        /* a board sold with different screens: this page is the first thing after flashing,
         * the place to say how to pick the other ones */
        const char *t4 = "SCREEN WRONG? PRESS BOOT";
        micro_text(cv, GFX_W / 2 - micro_width(t4, 1) / 2, ty + 48, t4, C_PRIM, 240, 1);
        char t5[48];
        snprintf(t5, sizeof t5, "NOW %s   HOLD BOOT: SKIP", ui_screen_name);
        micro_text(cv, GFX_W / 2 - micro_width(t5, 1) / 2, ty + 57, t5, C_QUIET, 230, 1);
    }
#if RADAR_HAS_BUTTON
    else {
        const char *t4 = "BOOT BUTTON: SKIP";
        micro_text(cv, GFX_W / 2 - micro_width(t4, 1) / 2, ty + 48, t4, C_QUIET, 230, 1);
    }
#endif
}
