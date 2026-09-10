/* ups_ui_screens.c - рендер экранов. Чистые функции: состояние -> фреймбуфер.
 * Никакого доступа к периферии, никаких задержек, никакого float.
 *
 * Компоновка кадра 160x160:
 *   0..14    заголовок (режим + индикатор АКБ)
 *   14..80   компактная мнемосхема
 *   80..104  текущие показания (две строки)
 *   104..160 меню настроек SETUP (пред./текущий/след. пункт + подсказка)
 */
#include "ups_ui.h"
#include <string.h>

/* Геометрия мнемосхемы (в пикселях кадра 160x160). */
#define YM   46      /* основная шина     */
#define YB   26      /* шина байпаса      */
#define X1   16      /* узел входа        */
#define X2   80      /* узел DC / БАТ     */
#define X3  144      /* узел выхода       */

enum { S_A, S_F, S_G, S_H, S_I, S_J, S_B, S_C, S_D, S_E, S_K, S_N };

typedef struct {
    uint8_t vert;      /* 0 = горизонтальный, 1 = вертикальный */
    int16_t a, b;      /* начало/конец вдоль оси               */
    int16_t c;         /* координата поперёк                   */
    int16_t d0;        /* смещение вдоль общего пути (фаза)    */
    uint8_t rev;       /* поток в обратную сторону             */
} seg_t;

static const seg_t SEG[S_N] = {
    /* A */ { 0,   4,  X1, YM,   0, 0 },
    /* F */ { 0,  X1,  35, YM,  12, 0 },
    /* G */ { 0,  58,  X2, YM,  55, 0 },
    /* H */ { 0,  X2, 103, YM,  77, 0 },
    /* I */ { 0, 126,  X3, YM, 123, 0 },
    /* J */ { 0,  X3, 155, YM, 141, 0 },
    /* B */ { 1,  YB,  YM, X1,  12, 1 },
    /* C */ { 0,  X1,  69, YB,  32, 0 },
    /* D */ { 0,  91,  X3, YB,  85, 0 },
    /* E */ { 1,  YB,  YM, X3, 138, 0 },
    /* K */ { 1,  YM,  64, X2, 100, 0 },
};

#define M(x) (1u << (x))

typedef struct { uint16_t on, flip; } modeviz_t;

static const modeviz_t MODEVIZ[UPS_MODE_N] = {
    /* ONLINE  */ { M(S_A)|M(S_F)|M(S_G)|M(S_H)|M(S_I)|M(S_J)|M(S_K), 0 },
    /* BATTERY */ { M(S_H)|M(S_I)|M(S_J)|M(S_K), M(S_K) },
    /* BYPASS  */ { M(S_A)|M(S_B)|M(S_C)|M(S_D)|M(S_E)|M(S_J), 0 },
    /* FAULT   */ { M(S_A)|M(S_F)|M(S_G), 0 },
};

#define CHASE_PERIOD 26   /* период бегущего разрыва, px */
#define CHASE_GAP     6   /* длина разрыва, px           */
#define CHASE_PX_S   26   /* скорость, px/с              */

/* -------------------------------------------------- форматирование чисел */

static char *put_s(char *d, const char *s) { while (*s) *d++ = *s++; return d; }

/* Беззнаковое число, minw знакомест, выравнивание вправо пробелами. */
static char *put_u(char *d, uint32_t v, int minw)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = n; i < minw; i++) *d++ = ' ';
    while (n--) *d++ = t[n];
    return d;
}

/* Беззнаковое число с ведущими нулями (для значений меню). */
static char *put_u0(char *d, uint32_t v, int width)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = n; i < width; i++) *d++ = '0';
    while (n--) *d++ = t[n];
    return d;
}

/* Fixed-point: v масштабировано на 10^frac. minw - ширина целой части. */
static char *put_fx(char *d, uint32_t v, int frac, int minw)
{
    uint32_t div = 1;
    for (int i = 0; i < frac; i++) div *= 10;
    d = put_u(d, v / div, minw);
    if (frac) {
        *d++ = '.';
        uint32_t f = v % div;
        for (int i = frac - 1; i >= 0; i--) {
            uint32_t p = 1; for (int k = 0; k < i; k++) p *= 10;
            *d++ = (char)('0' + (f / p) % 10);
        }
    }
    return d;
}

/* Дата/время. Переопредели у себя, если есть RTC. */
__attribute__((weak)) void ui_get_datetime(char *out /* >= 21 байт */)
{
    memcpy(out, "--.--.----    --:--", 20);
    out[19] = 0;
}

/* ------------------------------------------------------- меню настроек */

typedef struct {
    const char        *label;
    const char *const *options;  /* NULL, если значение числовое  */
    uint8_t            opt_n;
    uint8_t            opt_idx;
    uint16_t           v_min, v_max, value;  /* числовой вариант  */
    const char        *unit;
    uint8_t            digits;
    uint8_t            is_mode;  /* пункт задаёт режим мнемосхемы */
} menu_item_t;

static const char *const OPT_MODE[] = { "ONLINE", "BATTERY", "BYPASS", "FAULT" };
static const char *const OPT_VOLT[] = { "208V", "220V", "230V", "240V" };
static const char *const OPT_FREQ[] = { "50Hz", "60Hz" };
static const char *const OPT_EOD1[] = { "1.75V", "1.84V", "1.92V" };
static const char *const OPT_EOD2[] = { "1.60V", "1.70V", "1.80V" };
static const char *const OPT_ONOFF[] = { "ON", "OFF" };

static menu_item_t MENU[] = {
    { "MODE",   OPT_MODE,  4, 0, 0,   0,   0,   NULL,  0, 1 },
    { "VOLT",   OPT_VOLT,  4, 2, 0,   0,   0,   NULL,  0, 0 },
    { "FREQ",   OPT_FREQ,  2, 0, 0,   0,   0,   NULL,  0, 0 },
    { "CAP",    NULL,      0, 0, 1,   200, 9,   "Ah",  2, 0 },
    { "EOD1",   OPT_EOD1,  3, 0, 0,   0,   0,   NULL,  0, 0 },
    { "EOD2",   OPT_EOD2,  3, 0, 0,   0,   0,   NULL,  0, 0 },
    { "TMR1",   NULL,      0, 0, 0,   999, 0,   "min", 3, 0 },
    { "TMR2",   NULL,      0, 0, 0,   999, 0,   "min", 3, 0 },
    { "BYP H",  NULL,      0, 0, 230, 264, 264, "V",   3, 0 },
    { "BYP L",  NULL,      0, 0, 170, 220, 170, "V",   3, 0 },
    { "BEEP",   OPT_ONOFF, 2, 0, 0,   0,   0,   NULL,  0, 0 },
    { "BYPASS", OPT_ONOFF, 2, 1, 0,   0,   0,   NULL,  0, 0 },
};

#define MENU_N ((int)(sizeof MENU / sizeof MENU[0]))

static int      menu_item = 0;
static bool     menu_editing = false;
static uint8_t  edit_opt_idx;    /* временное значение при правке */
static uint16_t edit_value;

/* Строка значения пункта: либо вариант из списка, либо число + единицы. */
static void menu_value_str(const menu_item_t *it, uint8_t opt_idx, uint16_t value, char *out)
{
    if (it->options) {
        strcpy(out, it->options[opt_idx]);
        return;
    }
    char *p = put_u0(out, value, it->digits);
    p = put_s(p, it->unit);
    *p = 0;
}

void ui_menu_nav(int dir)
{
    if (!menu_editing) {
        menu_item = (menu_item + dir + MENU_N) % MENU_N;
        return;
    }

    menu_item_t *it = &MENU[menu_item];
    if (it->options) {
        edit_opt_idx = (uint8_t)((edit_opt_idx + dir + it->opt_n) % it->opt_n);
    } else {
        int32_t v = (int32_t)edit_value + dir;
        if (v < it->v_min) v = it->v_min;
        if (v > it->v_max) v = it->v_max;
        edit_value = (uint16_t)v;
    }
}

void ui_menu_enter(void)
{
    menu_item_t *it = &MENU[menu_item];
    if (menu_editing) {
        it->opt_idx = edit_opt_idx;
        it->value   = edit_value;
        menu_editing = false;
    } else {
        edit_opt_idx = it->opt_idx;
        edit_value   = it->value;
        menu_editing = true;
    }
}

void ui_menu_esc(void) { menu_editing = false; }

void ui_menu_set_mode(ups_mode_t m)
{
    if (m >= UPS_MODE_N)
        return;
    for (int i = 0; i < MENU_N; i++)
        if (MENU[i].is_mode) {
            MENU[i].opt_idx = (uint8_t)m;
            if (menu_editing && menu_item == i)
                edit_opt_idx = (uint8_t)m;
            return;
        }
}

ups_mode_t ui_menu_mode(void)
{
    for (int i = 0; i < MENU_N; i++)
        if (MENU[i].is_mode) {
            /* В режиме правки мнемосхема сразу показывает выбираемый режим */
            uint8_t idx = (menu_editing && menu_item == i) ? edit_opt_idx : MENU[i].opt_idx;
            return (ups_mode_t)idx;
        }
    return UPS_ONLINE;
}

/* ------------------------------------------------------------- примитивы */

static void draw_node(int x, int y) { fb_fill(x - 1, y - 1, 3, 3); }

static void draw_tri_up(int x, int y)
{
    for (int j = 0; j < 3; j++) fb_fill(x + 2 - j, y + j, 1 + j * 2, 1);
}

static void draw_tri_dn(int x, int y)
{
    for (int j = 0; j < 3; j++) fb_fill(x + j, y + j, 5 - j * 2, 1);
}

static void draw_seg(int id, uint16_t on, uint16_t flip, int chase)
{
    const seg_t *s = &SEG[id];
    const bool live  = (on   >> id) & 1u;
    const bool rev   = (bool)s->rev != (bool)((flip >> id) & 1u);
    const int  len   = s->b - s->a;

    for (int i = 0; i <= len; i++) {
        int d = rev ? (len - i) : i;
        if (live) {
            int g = ((s->d0 + d - chase) % CHASE_PERIOD + CHASE_PERIOD) % CHASE_PERIOD;
            if (g < CHASE_GAP) continue;          /* бегущий разрыв = поток   */
        } else if (i % 3 != 0) {
            continue;                              /* обесточено = пунктир     */
        }
        if (s->vert) fb_px(s->c, s->a + i); else fb_px(s->a + i, s->c);
    }
}

static void draw_header(const ups_state_t *st, bool stale)
{
    char buf[24], *p = buf;
    if (stale) {
        p = put_s(p, "NO LINK");
    } else switch (st->mode) {
        case UPS_ONLINE:  p = put_s(p, "ONLINE");     break;
        case UPS_BATTERY: p = put_s(p, "ON BATTERY"); break;
        case UPS_BYPASS:  p = put_s(p, "ON BYPASS");  break;
        default:          p = put_s(p, "FAULT E");
                          p = put_u(p, st->fault, 2);
                          if (buf[7] == ' ') buf[7] = '0';
                          break;
    }
    *p = 0;
    fb_text(2, 2, buf);

    /* индикатор АКБ: корпус + контакт + до 4 сегментов */
    fb_rect(130, 0, 26, 11);
    fb_fill(156, 3, 2, 5);
    int bars = stale ? 0 : (st->soc_pct + 12) / 25;
    if (bars > 4) bars = 4;
    for (int i = 0; i < bars; i++) fb_fill(133 + i * 6, 3, 5, 5);
}

static void draw_data(const ups_state_t *st, bool stale)
{
    char l1[26], l2[26], *p;

    if (stale) {
        strcpy(l1, "NO DATA FROM CONTROL");
        strcpy(l2, "CHECK LINK");
    } else if (st->mode == UPS_FAULT) {
        strcpy(l1, "OUTPUT OFF");
        p = put_s(l2, "BAT ");
        p = put_fx(p, st->vbat_cV / 10, 1, 2); p = put_s(p, "V ");
        p = put_u(p, st->soc_pct, 3);          p = put_s(p, "%");
        *p = 0;
    } else {
        p = put_s(l1, "OUT ");
        p = put_u(p, st->vout_dV / 10, 3);       p = put_s(p, "V ");
        p = put_fx(p, st->fout_cHz / 10, 1, 2);  p = put_s(p, "Hz ");
        p = put_u(p, st->load_pct, 3);           p = put_s(p, "%");
        *p = 0;

        p = put_s(l2, "BAT ");
        p = put_fx(p, st->vbat_cV / 10, 1, 2);   p = put_s(p, "V ");
        p = put_u(p, st->soc_pct, 3);            p = put_s(p, "% ");
        p = put_u(p, st->rt_min, 2);             p = put_s(p, "min");
        *p = 0;
    }
    fb_text(2, 84, l1);
    fb_text(2, 94, l2);
}

/* Одна строка меню: номер, метка, значение, стрелка направления. */
static void draw_menu_line(int idx, int y, char arrow)
{
    char num[4], val[16];
    put_u0(num, (uint32_t)idx + 1, 2);
    num[2] = 0;
    menu_value_str(&MENU[idx], MENU[idx].opt_idx, MENU[idx].value, val);

    fb_text(2, y, num);
    fb_text(20, y, MENU[idx].label);
    fb_text(74, y, val);
    if (arrow == 'u') draw_tri_up(150, y + 2);
    if (arrow == 'd') draw_tri_dn(150, y + 2);
}

static void draw_menu(uint32_t anim_ms)
{
    char buf[16], val[16], *p;

    fb_hline(0, 104, FB_W);
    fb_text(2, 108, "SETUP");

    p = put_u0(buf, (uint32_t)menu_item + 1, 2);
    p = put_s(p, "/");
    p = put_u0(p, MENU_N, 2);
    *p = 0;
    fb_text(122, 108, buf);

    int prev = (menu_item - 1 + MENU_N) % MENU_N;
    int next = (menu_item + 1) % MENU_N;
    draw_menu_line(prev, 118, 'u');

    /* текущий пункт - инверсией */
    fb_fill(0, 127, FB_W, 9);
    put_u0(buf, (uint32_t)menu_item + 1, 2);
    buf[2] = 0;
    menu_value_str(&MENU[menu_item], edit_opt_idx, edit_value, val);

    fb_text_inv(2, 128, buf);
    fb_text_inv(20, 128, MENU[menu_item].label);
    if (menu_editing) {
        fb_text_inv(74, 128, val);
        if ((anim_ms / 350u) % 2 == 0) {         /* мигающий указатель правки */
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 1 + j * 2; i++) fb_px_clear(150 + 2 - j + i, 129 + j);
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 5 - j * 2; i++) fb_px_clear(150 + j + i, 132 + j);
        }
    } else {
        menu_value_str(&MENU[menu_item], MENU[menu_item].opt_idx, MENU[menu_item].value, val);
        fb_text_inv(74, 128, val);
    }

    draw_menu_line(next, 139, 'd');

    fb_text(2, 151, menu_editing ? "ENT SAVE  ESC UNDO"
                                 : "ENT EDIT  UP DN SELECT");
}

/* ----------------------------------------------------- экран мнемосхемы */

void ui_render_mimic(const ups_state_t *st, uint32_t anim_ms, bool stale)
{
    const modeviz_t v = stale ? (modeviz_t){ 0, 0 }
                              : MODEVIZ[st->mode < UPS_MODE_N ? st->mode : UPS_FAULT];
    const int chase = (int)((anim_ms * CHASE_PX_S / 1000u) % CHASE_PERIOD);

    fb_clear();

    draw_header(st, stale);
    fb_hline(0, 14, FB_W);

    /* стрелки IN/OUT */
    for (int i = 0; i < 4; i++) {
        int h = 1 + (3 - i) * 2;
        fb_fill(i,       YM - 3 + i, 1, h);
        fb_fill(159 - i, YM - 3 + i, 1, h);
    }

    for (int i = 0; i < S_N; i++) draw_seg(i, v.on, v.flip, chase);

    fb_rect(69, 20, 23, 13);  fb_text(72, 23, "BYP");
    fb_rect(35, 40, 23, 13);  fb_text(38, 43, "REC");
    fb_rect(103, 40, 23, 13); fb_text(106, 43, "INV");
    fb_rect(69, 64, 23, 13);  fb_text(72, 67, "BAT");

    draw_node(X1, YM); draw_node(X2, YM); draw_node(X3, YM);
    draw_node(X1, YB); draw_node(X3, YB);

    fb_text(2, 54, "IN");
    fb_text(141, 54, "OUT");

    fb_hline(0, 80, FB_W);
    draw_data(st, stale);

    draw_menu(anim_ms);
}

/* ------------------------------------------------------ экран измерений */

void ui_render_meters(const ups_state_t *st, uint32_t anim_ms, bool stale)
{
    (void)anim_ms;
    char b[26], *p;

    fb_clear();
    fb_text(3, 4, "MEASUREMENTS");
    fb_hline(0, 16, FB_W);

    static const char *lbl[6] = { "VIN", "VOUT", "FOUT", "LOAD", "VBAT", "SOC" };
    for (int i = 0; i < 6; i++) {
        int y = 26 + i * 16;
        fb_text(6, y, lbl[i]);
        p = b;
        if (stale) { p = put_s(p, "----"); }
        else switch (i) {
            case 0: p = put_fx(p, st->vin_dV,  1, 3); p = put_s(p, " V");   break;
            case 1: p = put_fx(p, st->vout_dV, 1, 3); p = put_s(p, " V");   break;
            case 2: p = put_fx(p, st->fout_cHz,2, 2); p = put_s(p, " HZ");  break;
            case 3: p = put_u (p, st->load_pct,   3); p = put_s(p, " %");   break;
            case 4: p = put_fx(p, st->vbat_cV, 2, 2); p = put_s(p, " V");   break;
            case 5: p = put_u (p, st->soc_pct,    3); p = put_s(p, " %");   break;
        }
        *p = 0;
        fb_text(72, y, b);
        if (i < 5) fb_hline(4, y + 11, FB_W - 8);
    }
    fb_hline(0, 143, FB_W);
    fb_text(3, 149, "ESC BACK   ENTER LOG");
}
