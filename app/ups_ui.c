/* ups_ui.c - ядро UI: обмен с управлением, планировщик, машина экранов. */
#include "ups_ui.h"
#include <string.h>

#ifndef __DMB
#define __DMB() __asm__ volatile ("" ::: "memory")   /* на МК заменится на CMSIS */
#endif

/* ------------------------------------------------- seqlock: ISR -> main */

static volatile uint32_t    s_seq = 0;
static volatile ups_state_t s_shared;

void ups_state_publish(const ups_state_t *s)
{
    s_seq++;  __DMB();                        /* нечётное: идёт запись */
    memcpy((void *)&s_shared, s, sizeof *s);
    __DMB();  s_seq++;                        /* чётное: снимок валиден */
}

bool ups_state_read(ups_state_t *out)
{
    for (int i = 0; i < 4; i++) {
        uint32_t a = s_seq; __DMB();
        if (a & 1u) continue;                 /* писатель в процессе */
        memcpy(out, (const void *)&s_shared, sizeof *out);
        __DMB();
        if (s_seq == a) return a != 0;
    }
    return false;
}

/* ------------------------------------- сглаживание показаний (антидребезг) */

typedef struct { uint16_t shown; uint32_t t_last; } damp_t;

/* Значение обновляется, если ушло дальше deadband или прошло hold_ms. */
static uint16_t damp(damp_t *d, uint16_t v, uint16_t deadband, uint32_t hold_ms, uint32_t now)
{
    int32_t diff = (int32_t)v - (int32_t)d->shown;
    if (diff < 0) diff = -diff;
    if (diff >= deadband || (uint32_t)(now - d->t_last) >= hold_ms) {
        d->shown  = v;
        d->t_last = now;
    }
    return d->shown;
}

static damp_t d_vin, d_vout, d_fout, d_vbat, d_load, d_soc;

/* --------------------------------------------------------- машина экранов */

/* Экран один: мнемосхема с меню настроек внизу (см. ups_ui_screens.c). */
typedef enum { SCR_MIMIC = 0, SCR_N } screen_id_t;

typedef void (*render_fn)(const ups_state_t *, uint32_t, bool);
static const render_fn SCREEN[SCR_N] = { ui_render_mimic };

#define UI_PERIOD_MS      100u    /* 10 кадров/с */
#define STALE_MS          300u    /* нет свежих данных -> "NO LINK" */
#define IDLE_RETURN_MS  30000u    /* авто-возврат на мнемосхему */

static screen_id_t   scr;
static uint32_t      t_next, t_key;
static volatile ui_key_t key_pend;
static ups_mode_t    mode_prev;

void ui_init(void)
{
    scr = SCR_MIMIC;
    t_next = 0; t_key = 0;
    key_pend = KEY_NONE;
    mode_prev = UPS_ONLINE;
    memset(&d_vin, 0, sizeof d_vin);   memset(&d_vout, 0, sizeof d_vout);
    memset(&d_fout, 0, sizeof d_fout); memset(&d_vbat, 0, sizeof d_vbat);
    memset(&d_load, 0, sizeof d_load); memset(&d_soc,  0, sizeof d_soc);
    fb_clear();
    fb_invalidate();                  /* первый кадр уйдёт целиком */
}

void ui_key(ui_key_t k) { key_pend = k; }   /* запись слова - атомарна */

/* Клавиши обслуживают меню настроек на экране мнемосхемы:
   UP/DOWN - навигация или смена значения, ENTER - правка/сохранить,
   ESC - отмена правки. */
static void handle_key(ui_key_t k, uint32_t now)
{
    t_key = now;
    switch (k) {
        case KEY_UP:    ui_menu_nav(-1);  break;
        case KEY_DOWN:  ui_menu_nav(+1);  break;
        case KEY_ENTER: ui_menu_enter();  break;
        case KEY_ESC:   ui_menu_esc();    break;
        default: break;
    }
}

void ui_tick(uint32_t now)
{
    ui_key_t k = key_pend;
    if (k != KEY_NONE) { key_pend = KEY_NONE; handle_key(k, now); }

    if ((int32_t)(now - t_next) < 0) return;
    t_next = now + UI_PERIOD_MS;

    ups_state_t st;
    memset(&st, 0, sizeof st);
    bool have  = ups_state_read(&st);
    bool stale = !have || (uint32_t)(now - st.stamp_ms) > STALE_MS;

    if (have) {
        /* Отказ всегда вытаскивает мнемосхему на передний план. */
        if (st.mode == UPS_FAULT && mode_prev != UPS_FAULT) scr = SCR_MIMIC;
        mode_prev = st.mode;

        /* Гасим дребезг младших разрядов, иначе цифры "кипят". */
        st.vin_dV   = damp(&d_vin,  st.vin_dV,   5, 1000, now);   /* 0.5 В  */
        st.vout_dV  = damp(&d_vout, st.vout_dV,  5, 1000, now);
        st.fout_cHz = damp(&d_fout, st.fout_cHz, 5, 1000, now);   /* 0.05 Гц */
        st.vbat_cV  = damp(&d_vbat, st.vbat_cV, 10, 2000, now);   /* 0.10 В */
        st.load_pct = (uint8_t)damp(&d_load, st.load_pct, 2, 1000, now);
        st.soc_pct  = (uint8_t)damp(&d_soc,  st.soc_pct,  1, 5000, now);
    }

    if (scr != SCR_MIMIC && (uint32_t)(now - t_key) > IDLE_RETURN_MS) scr = SCR_MIMIC;

    SCREEN[scr](&st, now, stale);

    /* Линк занят - кадр просто пропускаем, дельта накопится к следующему разу. */
    (void)fb_flush();
}
