/*==============================================================================
 * Драйвер графического ЖКИ 240x128 на Avant SAP1024B (клон Toshiba T6963C)
 * Параллельная 8-битная шина, K1921VG5T
 *==============================================================================
 */

#ifndef LCD_H
#define LCD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

//-- Подключение -----------------------------------------------------------------
// DB0..DB7 -> GPIOB[0..7] (единой операцией с регистром порта)
// /WR -> PA8   /RD -> PA9   /CE -> PA10   C/D -> PA11   /RST -> PA1
// Все линии через транслятор уровней 3.3В<->5В. Питание и контраст - внешние.
//
// ВНИМАНИЕ: PA2..PA6 (JTAG), PA7 (SERVEN), PA12 (LED), PA13 (кнопка),
// PB8/PB9 (UART0) драйвер не трогает.
//
// КОНФЛИКТ: PB4/PB5/PB7 заняты под MAX7219 (SPI0_CLK/FSS/TX, см. max7219.h).
// lcd_gpio_init() принудительно снимает с них альтфункцию SPI0, чтобы шина
// данных ЖКИ работала как обычный GPIO - MAX7219 в этот момент перестаёт
// быть доступен. Одновременная работа обоих устройств на этой распайке
// невозможна без переноса одного из них на другие ножки.
#define LCD_DATA_PORT    GPIOB
#define LCD_DATA_PORT_EN GPIOBEN

#define LCD_CTRL_PORT    GPIOA
#define LCD_CTRL_PORT_EN GPIOAEN

#define LCD_WR_POS   8
#define LCD_RD_POS   9
#define LCD_CE_POS   10
#define LCD_CD_POS   11
#define LCD_RST_POS  1

#define LCD_WR_MSK   (1u << LCD_WR_POS)
#define LCD_RD_MSK   (1u << LCD_RD_POS)
#define LCD_CE_MSK   (1u << LCD_CE_POS)
#define LCD_CD_MSK   (1u << LCD_CD_POS)
#define LCD_RST_MSK  (1u << LCD_RST_POS)

//-- Геометрия экрана -------------------------------------------------------------
#define LCD_WIDTH          240
#define LCD_HEIGHT         128
#define LCD_M              30      // байт/строка графики = символов/строка текста
#define LCD_N              16      // строк текста (128 / 8 пикселей на символ)

#define LCD_GRAPHIC_HOME   0x0000u
#define LCD_GRAPHIC_AREA   LCD_M
#define LCD_TEXT_HOME      0x1000u
#define LCD_TEXT_AREA      LCD_M

#define LCD_GRAPHIC_FRAME_SIZE (LCD_M * LCD_HEIGHT) // 3840 байт
#define LCD_TEXT_FRAME_SIZE    (LCD_M * LCD_N)      // 480 байт

//-- Команды SAP1024B / T6963C -----------------------------------------------------
#define LCD_CMD_CURSOR_POINTER    0x21
#define LCD_CMD_OFFSET_REGISTER   0x22
#define LCD_CMD_ADDRESS_POINTER   0x24
#define LCD_CMD_TEXT_HOME         0x40
#define LCD_CMD_TEXT_AREA         0x41
#define LCD_CMD_GRAPHIC_HOME      0x42
#define LCD_CMD_GRAPHIC_AREA      0x43
#define LCD_CMD_MODE_SET_OR_INT   0x80 // OR, внутренний CG
#define LCD_CMD_DISPMODE_GRAPHIC  0x98
#define LCD_CMD_DISPMODE_TEXT     0x9C
#define LCD_CMD_DISPMODE_BOTH     0x9E
#define LCD_CMD_AUTO_WRITE        0xB0
#define LCD_CMD_AUTO_READ         0xB1
#define LCD_CMD_AUTO_RESET        0xB2
#define LCD_CMD_WRITE_INC         0xC0
#define LCD_CMD_READ_INC          0xC1
#define LCD_CMD_WRITE_NOINC       0xC4
#define LCD_CMD_BIT_RESET_BASE    0xF0 // 0xF0..0xF7
#define LCD_CMD_BIT_SET_BASE      0xF8 // 0xF8..0xFF

//-- Статус --------------------------------------------------------------------
#define LCD_STA_NORMAL_MSK   0x03u // STA0|STA1 - готовность вне Auto-режима
#define LCD_STA_AUTORD_MSK   0x04u // STA2 - готовность чтения в Auto Read
#define LCD_STA_AUTOWR_MSK   0x08u // STA3 - готовность записи в Auto Write

//-- API -------------------------------------------------------------------------
// Первым делом: проверка шины БЕЗ протокола T6963C - 1000 чтений статуса подряд.
// Возвращает 1, если статус стабилен и не равен 0x00/0xFF (монтаж в порядке).
uint8_t lcd_selftest(void);

void lcd_init(void);
void lcd_clear(void);
void lcd_fill(uint8_t value); // 0xFF - залить весь экран (все пиксели горят)

// Запись готовых байт графической памяти: count байт подряд начиная с
// байтовой колонки x_byte (0..LCD_M-1) в пиксельной строке y. Быстрый путь
// для вывода внешнего фреймбуфера - без пересчёта по одному пикселю.
void lcd_write_row(uint16_t x_byte, uint16_t y, const uint8_t *data, uint16_t count);

void lcd_set_pixel(uint16_t x, uint16_t y, uint8_t on);
void lcd_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1);
void lcd_draw_rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1);

void lcd_put_char(uint8_t col, uint8_t row, char c);
void lcd_put_string(uint8_t col, uint8_t row, const char *str);

#ifdef __cplusplus
}
#endif

#endif // LCD_H
