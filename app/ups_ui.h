/* ups_ui.h - UI слой ИБП: модель -> рендер -> транспорт.
 *
 * Правила:
 *   1. Ни одна функция отсюда не вызывается из ISR управления (20 кГц).
 *      Из ISR допустим только ups_state_publish().
 *   2. Никакого float в тракте отображения - только целочисленный fixed-point.
 *   3. Рендер - чистая функция: состояние -> фреймбуфер. Без побочных эффектов.
 */
#ifndef UPS_UI_H
#define UPS_UI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FB_W        160
#define FB_H        160
#define FB_STRIDE   (FB_W / 8)          /* 20 байт на строку, MSB = левый пиксель */
#define FB_SIZE     (FB_STRIDE * FB_H)  /* 3200 байт */
#define TILE_COLS   (FB_W / 8)          /* тайл 8x8 px = 8 байт */
#define TILE_ROWS   (FB_H / 8)

/* ------------------------------------------------------------------ модель */

typedef enum {
    UPS_ONLINE = 0,
    UPS_BATTERY,
    UPS_BYPASS,
    UPS_FAULT,
    UPS_MODE_N
} ups_mode_t;

/* Снимок состояния. Все величины целые, единицы указаны в имени поля. */
typedef struct {
    uint32_t   stamp_ms;   /* время формирования снимка */
    ups_mode_t mode;
    uint16_t   vin_dV;     /* 2300 = 230.0 В  */
    uint16_t   vout_dV;    /* 2300 = 230.0 В  */
    uint16_t   fout_cHz;   /* 5000 = 50.00 Гц */
    uint16_t   vbat_cV;    /* 5460 = 54.60 В  */
    uint8_t    load_pct;
    uint8_t    soc_pct;
    uint16_t   rt_min;     /* оставшееся время автономии, мин */
    uint8_t    fault;      /* 0 = нет; иначе код Exx */
} ups_state_t;

/* Публикация из управляющего контекста (seqlock, без критических секций). */
void ups_state_publish(const ups_state_t *s);
/* Чтение согласованного снимка. false - данных ещё не было. */
bool ups_state_read(ups_state_t *out);

/* ----------------------------------------------------------- фреймбуфер */

void fb_clear(void);
void fb_px(int x, int y);
void fb_hline(int x, int y, int w);
void fb_vline(int x, int y, int h);
void fb_rect(int x, int y, int w, int h);   /* контур */
void fb_fill(int x, int y, int w, int h);   /* заливка */
void fb_text(int x, int y, const char *s);  /* шрифт 5x7, шаг 6 px */
void fb_px_clear(int x, int y);             /* погасить пиксель */
void fb_text_inv(int x, int y, const char *s); /* текст по залитому фону */
void fb_invalidate(void);                   /* следующий flush уйдёт целиком */

/* Отправляет только изменившиеся тайлы. Возврат: байт отправлено, <0 - линк занят. */
int  fb_flush(void);

const uint8_t *fb_raw(void);                /* для хост-превью/тестов */

/* ------------------------------------------------------------------- UI */

typedef enum { KEY_NONE = 0, KEY_UP, KEY_DOWN, KEY_ENTER, KEY_ESC } ui_key_t;

void ui_init(void);
void ui_key(ui_key_t k);          /* из обработчика кнопок, неблокирующее */
void ui_tick(uint32_t now_ms);    /* из main loop, чем чаще тем лучше */

/* Прямой рендер конкретного экрана - для хост-превью и юнит-тестов. */
void ui_render_mimic(const ups_state_t *s, uint32_t anim_ms, bool stale);
void ui_render_meters(const ups_state_t *s, uint32_t anim_ms, bool stale);

/* ------------------------------------------------ меню настроек (SETUP) */

void       ui_menu_nav(int dir);    /* -1 вверх / +1 вниз, в режиме правки - смена значения */
void       ui_menu_enter(void);     /* войти в правку / сохранить */
void       ui_menu_esc(void);       /* отменить правку */
ups_mode_t ui_menu_mode(void);              /* режим, выбранный пунктом MODE */
void       ui_menu_set_mode(ups_mode_t m);  /* задать пункт MODE извне */

/* --------------------------------------------------- порт под платформу */

/* Поставить блок в очередь DMA. 0 - принято, <0 - занято (кадр будет пропущен). */
int link_send(const uint8_t *p, size_t n);

#endif /* UPS_UI_H */
