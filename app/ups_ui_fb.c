/* ups_ui_fb.c - монохромный фреймбуфер 160x160 (1 bpp) + дельта-передача.
 *
 * Формат пакета в линию:
 *   A5 5A 10 lenL lenH  <payload>  crc8
 *   payload = последовательность серий: ty, tx0, n, n*8 байт
 *   тайл 8x8 px = 8 байт (по одному на строку тайла)
 *
 * Дельта считается сравнением с теневым буфером. Тень обновляется только
 * после успешной постановки пакета в DMA - иначе изменения не теряются.
 */
#include "ups_ui.h"
#include <string.h>

static uint8_t fb[FB_SIZE];
static uint8_t shadow[FB_SIZE];
static bool    shadow_valid = false;

/* ------------------------------------------------------------ шрифт 5x7 */
/* Колоночный формат: 5 байт на символ, бит 0 = верхняя строка. 0x20..0x5F. */
static const uint8_t font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */ {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */ {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */ {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */ {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */ {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */ {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */ {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */ {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */ {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */ {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */ {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */ {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */ {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */ {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */ {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */ {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */ {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */ {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */ {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */ {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */ {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */ {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */ {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */ {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */ {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */ {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */ {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */ {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */ {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */ {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */ {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */ {0x40,0x40,0x40,0x40,0x40}, /* _ */
    /* 0x60..0x7F не хранятся: строчные приводятся к заглавным в fb_text */
    {0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},
    {0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},{0},
};

/* ------------------------------------------------------------ примитивы */

void fb_clear(void) { memset(fb, 0, sizeof fb); }

void fb_px(int x, int y)
{
    if ((unsigned)x >= FB_W || (unsigned)y >= FB_H) return;
    fb[y * FB_STRIDE + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
}

void fb_hline(int x, int y, int w) { while (w-- > 0) fb_px(x++, y); }
void fb_vline(int x, int y, int h) { while (h-- > 0) fb_px(x, y++); }

void fb_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    fb_hline(x, y, w);
    fb_hline(x, y + h - 1, w);
    fb_vline(x, y, h);
    fb_vline(x + w - 1, y, h);
}

void fb_fill(int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++) fb_hline(x, y + j, w);
}

void fb_px_clear(int x, int y)
{
    if ((unsigned)x >= FB_W || (unsigned)y >= FB_H) return;
    fb[y * FB_STRIDE + (x >> 3)] &= (uint8_t)~(0x80u >> (x & 7));
}

static void fb_text_draw(int x, int y, const char *s, bool inverse)
{
    for (; *s; s++, x += 6) {
        unsigned c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z') c -= 32;      /* строчные -> заглавные */
        if (c < 0x20 || c > 0x7F) c = '?';
        const uint8_t *g = font5x7[c - 0x20];
        for (int gx = 0; gx < 5; gx++) {
            uint8_t col = g[gx];
            for (int gy = 0; gy < 7; gy++)
                if (col & (1u << gy)) {
                    if (inverse) fb_px_clear(x + gx, y + gy);
                    else         fb_px(x + gx, y + gy);
                }
        }
    }
}

void fb_text(int x, int y, const char *s) { fb_text_draw(x, y, s, false); }

/* Текст "дырками" по уже залитому прямоугольнику - выделенная строка меню */
void fb_text_inv(int x, int y, const char *s) { fb_text_draw(x, y, s, true); }

const uint8_t *fb_raw(void) { return fb; }

void fb_invalidate(void) { shadow_valid = false; }

/* -------------------------------------------------------------- дельта */

#define PKT_CAP 512

static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0xFF;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}

static bool tile_equal(int tx, int ty)
{
    const int base = ty * 8 * FB_STRIDE + tx;
    for (int r = 0; r < 8; r++)
        if (fb[base + r * FB_STRIDE] != shadow[base + r * FB_STRIDE]) return false;
    return true;
}

static void tile_commit(int tx, int ty)
{
    const int base = ty * 8 * FB_STRIDE + tx;
    for (int r = 0; r < 8; r++) shadow[base + r * FB_STRIDE] = fb[base + r * FB_STRIDE];
}

typedef struct { uint8_t ty, tx, n; } run_t;

static uint8_t pkt[PKT_CAP];
static size_t  pkt_len;          /* длина payload */
static run_t   pend[24];
static int     pend_n;

static void pkt_begin(void) { pkt_len = 0; pend_n = 0; }

/* Возврат: байт отправлено, <0 - линк занят (тень не трогаем). */
static int pkt_send(void)
{
    if (pkt_len == 0) return 0;
    uint8_t hdr[5] = { 0xA5, 0x5A, 0x10, (uint8_t)(pkt_len & 0xFF), (uint8_t)(pkt_len >> 8) };
    static uint8_t out[PKT_CAP + 6];
    memcpy(out, hdr, 5);
    memcpy(out + 5, pkt, pkt_len);
    out[5 + pkt_len] = crc8(pkt, pkt_len);
    size_t total = pkt_len + 6;
    if (link_send(out, total) < 0) return -1;
    for (int i = 0; i < pend_n; i++)
        for (int k = 0; k < pend[i].n; k++) tile_commit(pend[i].tx + k, pend[i].ty);
    pkt_begin();
    return (int)total;
}

static bool pkt_add_run(int ty, int tx0, int n)
{
    size_t need = 3 + (size_t)n * 8;
    if (pkt_len + need > PKT_CAP || pend_n == (int)(sizeof pend / sizeof pend[0])) return false;
    pkt[pkt_len++] = (uint8_t)ty;
    pkt[pkt_len++] = (uint8_t)tx0;
    pkt[pkt_len++] = (uint8_t)n;
    for (int k = 0; k < n; k++) {
        const int base = ty * 8 * FB_STRIDE + tx0 + k;
        for (int r = 0; r < 8; r++) pkt[pkt_len++] = fb[base + r * FB_STRIDE];
    }
    pend[pend_n].ty = (uint8_t)ty;
    pend[pend_n].tx = (uint8_t)tx0;
    pend[pend_n].n  = (uint8_t)n;
    pend_n++;
    return true;
}

int fb_flush(void)
{
    if (!shadow_valid) {
        memset(shadow, 0xAA, sizeof shadow);   /* заведомо отличается -> полный кадр */
        shadow_valid = true;
    }
    pkt_begin();
    int sent = 0;
    for (int ty = 0; ty < TILE_ROWS; ty++) {
        int tx = 0;
        while (tx < TILE_COLS) {
            if (tile_equal(tx, ty)) { tx++; continue; }
            int n = 0;
            while (tx + n < TILE_COLS && !tile_equal(tx + n, ty) && n < 20) n++;
            if (!pkt_add_run(ty, tx, n)) {
                int r = pkt_send();
                if (r < 0) return -1;          /* линк занят: тень не двигаем */
                sent += r;
                if (!pkt_add_run(ty, tx, n)) return sent;  /* серия не влезла в пустой пакет */
            }
            tx += n;
        }
    }
    int r = pkt_send();
    if (r < 0) return -1;
    return sent + r;
}
