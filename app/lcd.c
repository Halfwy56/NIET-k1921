/*==============================================================================
 * Драйвер графического ЖКИ 240x128 на Avant SAP1024B (клон Toshiba T6963C)
 *==============================================================================
 */

#include "lcd.h"
#include <K1921VG5T.h>
#include <system_k1921vg5t.h>
#include <stdio.h>

// На этом экземпляре стекла байт 0x00 в графической памяти может физически
// светить всеми точками (инвертированная полярность матрицы). Если после
// прошивки с LCD_INVERT=1 разница видна, но "наоборот" - поставь 0.
#define LCD_INVERT 0

#if LCD_INVERT
#define LCD_BLANK_BYTE     0xFFu
#define LCD_PIXEL_ON_CMD   LCD_CMD_BIT_RESET_BASE
#define LCD_PIXEL_OFF_CMD  LCD_CMD_BIT_SET_BASE
#else
#define LCD_BLANK_BYTE     0x00u
#define LCD_PIXEL_ON_CMD   LCD_CMD_BIT_SET_BASE
#define LCD_PIXEL_OFF_CMD  LCD_CMD_BIT_RESET_BASE
#endif

//-- Тайминги ----------------------------------------------------------------
// Считаем от РЕАЛЬНОЙ частоты ядра (SystemCoreClock), но ОДИН РАЗ при
// инициализации: 64-битное деление в каждом шинном цикле стоит сотни тактов
// и превращает любой таймаут в секунды ожидания.
// +4 такта запаса на накладные расходы цикла/вызова функции.
static uint32_t lcd_cyc_strobe = 8;  // /WR, /RD - импульс >=80нс
static uint32_t lcd_cyc_setup  = 8;  // установка данных >=80нс
static uint32_t lcd_cyc_access = 12; // доступ при чтении >=150нс
static uint32_t lcd_cyc_hold   = 6;  // удержание >=40нс
static uint32_t lcd_cyc_reset  = 200; // /RST низкий >=10мкс

static void lcd_calc_delays(void)
{
	uint32_t mhz = SystemCoreClock / 1000000UL; // тактов на микросекунду
	lcd_cyc_strobe = (80UL * mhz) / 1000UL + 4UL;
	lcd_cyc_setup  = (80UL * mhz) / 1000UL + 4UL;
	lcd_cyc_access = (150UL * mhz) / 1000UL + 4UL;
	lcd_cyc_hold   = (40UL * mhz) / 1000UL + 4UL;
	lcd_cyc_reset  = 10UL * mhz + 4UL;
}

static inline void lcd_delay_cycles(uint32_t n)
{
	while (n--)
		__asm volatile("nop");
}

#define LCD_DELAY_STROBE()  lcd_delay_cycles(lcd_cyc_strobe)
#define LCD_DELAY_SETUP()   lcd_delay_cycles(lcd_cyc_setup)
#define LCD_DELAY_ACCESS()  lcd_delay_cycles(lcd_cyc_access)
#define LCD_DELAY_HOLD()    lcd_delay_cycles(lcd_cyc_hold)

// Защита от зависания на ожидании статуса при неисправной/неподключенной шине.
// Одна итерация - полный цикл чтения статуса (~пара сотен нс), так что
// 20000 итераций это единицы миллисекунд, а не секунды.
#define LCD_WAIT_TIMEOUT 20000UL

//-- Управляющие линии ---------------------------------------------------------
static inline void lcd_wr(uint8_t level) { level ? (LCD_CTRL_PORT->DATAOUTSET = LCD_WR_MSK) : (LCD_CTRL_PORT->DATAOUTCLR = LCD_WR_MSK); }
static inline void lcd_rd(uint8_t level) { level ? (LCD_CTRL_PORT->DATAOUTSET = LCD_RD_MSK) : (LCD_CTRL_PORT->DATAOUTCLR = LCD_RD_MSK); }
static inline void lcd_ce(uint8_t level) { level ? (LCD_CTRL_PORT->DATAOUTSET = LCD_CE_MSK) : (LCD_CTRL_PORT->DATAOUTCLR = LCD_CE_MSK); }
static inline void lcd_cd(uint8_t level) { level ? (LCD_CTRL_PORT->DATAOUTSET = LCD_CD_MSK) : (LCD_CTRL_PORT->DATAOUTCLR = LCD_CD_MSK); }
static inline void lcd_rst(uint8_t level) { level ? (LCD_CTRL_PORT->DATAOUTSET = LCD_RST_MSK) : (LCD_CTRL_PORT->DATAOUTCLR = LCD_RST_MSK); }

//-- Шина данных (двунаправленная) ----------------------------------------------
static inline void lcd_bus_dir_out(void) { LCD_DATA_PORT->OUTENSET = 0xFFu; }
static inline void lcd_bus_dir_in(void)  { LCD_DATA_PORT->OUTENCLR = 0xFFu; }

static inline void lcd_bus_write(uint8_t data)
{
	LCD_DATA_PORT->DATAOUTSET = data;
	LCD_DATA_PORT->DATAOUTCLR = (uint8_t)~data;
}

static inline uint8_t lcd_bus_read(void)
{
	return (uint8_t)(LCD_DATA_PORT->DATA & 0xFFu);
}

//-- Низкоуровневые шинные циклы ------------------------------------------------
// C/D=1 + WR=0 -> запись команды; C/D=0 + WR=0 -> запись данных. CE=0 всегда.
static void lcd_bus_write_cycle(uint8_t value, uint8_t cd)
{
	lcd_cd(cd);
	lcd_bus_dir_out();
	lcd_bus_write(value);
	LCD_DELAY_SETUP();      // данные должны установиться до начала строба

	lcd_ce(0);
	lcd_wr(0);
	LCD_DELAY_STROBE();     // ширина импульса /WR
	lcd_wr(1);
	lcd_ce(1);
	LCD_DELAY_HOLD();
}

// C/D=1 + RD=0 -> чтение статуса; C/D=0 + RD=0 -> чтение данных. CE=0 всегда.
static uint8_t lcd_bus_read_cycle(uint8_t cd)
{
	lcd_cd(cd);
	lcd_bus_dir_in();

	lcd_ce(0);
	lcd_rd(0);
	LCD_DELAY_ACCESS();     // время доступа при чтении
	uint8_t value = lcd_bus_read();
	lcd_rd(1);
	lcd_ce(1);
	LCD_DELAY_HOLD();

	return value;
}

//-- Статус ----------------------------------------------------------------------
static inline uint8_t lcd_read_status_raw(void)
{
	return lcd_bus_read_cycle(1); // C/D=1 + RD=0
}

// Обычный режим: перед КАЖДЫМ обменом ждать (status & 0x03) == 0x03.
static void lcd_wait_ready(void)
{
	uint32_t timeout = LCD_WAIT_TIMEOUT;
	uint8_t status;

	do {
		status = lcd_read_status_raw();
	} while (((status & LCD_STA_NORMAL_MSK) != LCD_STA_NORMAL_MSK) && --timeout);

	// ДИАГНОСТИКА: если готовности так и не дождались - показываем, что
	// реально читается со статусной шины (первые 10 раз, чтобы не спамить).
	if (timeout == 0)
	{
		static uint8_t reported = 0;
		if (reported < 10)
		{
			reported++;
			printf("LCD wait_ready TIMEOUT, status=0x%02X\r\n", (unsigned)status);
		}
	}
}

//-- Команды с операндами --------------------------------------------------------
// Операнды передаются ДО кода команды.
static void lcd_cmd0(uint8_t cmd)
{
	lcd_wait_ready();
	lcd_bus_write_cycle(cmd, 1);
}

static void lcd_cmd1(uint8_t cmd, uint8_t operand1)
{
	lcd_wait_ready();
	lcd_bus_write_cycle(operand1, 0);
	lcd_bus_write_cycle(cmd, 1);
}

// Для двух операндов порядок записи на шину: сначала D1 (младший байт),
// потом D2 (старший байт), и только затем код команды.
static void lcd_cmd2(uint8_t cmd, uint8_t operand1, uint8_t operand2)
{
	lcd_wait_ready();
	lcd_bus_write_cycle(operand1, 0);
	lcd_bus_write_cycle(operand2, 0);
	lcd_bus_write_cycle(cmd, 1);
}

static void lcd_set_address_pointer(uint16_t addr)
{
	lcd_cmd2(LCD_CMD_ADDRESS_POINTER, (uint8_t)addr, (uint8_t)(addr >> 8));
}

//-- Заливка области памяти ------------------------------------------------------
// ВАЖНО: Auto Write (0xB0) на этом экземпляре SAP1024B неработоспособен -
// контроллер не выставляет STA3, запись встаёт после нескольких байт, а после
// такого сорванного Auto-режима команда выхода 0xB2 не восстанавливает
// нормальное состояние: адресация и содержимое памяти портятся, экран
// перестаёт показывать что-либо осмысленное.
// Поэтому льём обычной командой 0xC0 (запись данных + автоинкремент адреса)
// по штатному статусу STA0/STA1, который работает надёжно. Полный кадр
// графики (3840 байт) при этом заливается за десятки миллисекунд.
static void lcd_fill_area(uint16_t addr, uint8_t value, uint16_t count)
{
	lcd_set_address_pointer(addr);
	for (uint16_t i = 0; i < count; i++)
		lcd_cmd1(LCD_CMD_WRITE_INC, value);
}

//-- Инициализация выводов ------------------------------------------------------
static void lcd_gpio_init(void)
{
	lcd_calc_delays(); // один раз пересчитываем задержки под текущую частоту

	RCU->CGCFGAHB_bit.LCD_CTRL_PORT_EN = 1;
	RCU->RSTDISAHB_bit.LCD_CTRL_PORT_EN = 1;
	RCU->CGCFGAHB_bit.LCD_DATA_PORT_EN = 1;
	RCU->RSTDISAHB_bit.LCD_DATA_PORT_EN = 1;

	// PB4/PB5/PB7 могли быть заняты под SPI0 (MAX7219) - снимаем альтфункцию,
	// иначе шина данных ЖКИ не будет работать как обычный GPIO.
	LCD_DATA_PORT->ALTFUNCCLR = 0xFFu;

	// Сначала безопасные уровни в регистр выходных данных...
	LCD_CTRL_PORT->DATAOUTSET = LCD_WR_MSK | LCD_RD_MSK | LCD_CE_MSK;
	LCD_CTRL_PORT->DATAOUTCLR = LCD_CD_MSK | LCD_RST_MSK;
	// ...и только потом переключаем эти линии на выход.
	LCD_CTRL_PORT->OUTENSET = LCD_WR_MSK | LCD_RD_MSK | LCD_CE_MSK | LCD_CD_MSK | LCD_RST_MSK;

	// Шина данных - вход по умолчанию (в простое ничего не должна занимать).
	lcd_bus_dir_in();
}

static void lcd_hw_reset(void)
{
	lcd_rst(0);
	lcd_delay_cycles(lcd_cyc_reset); // /RST низкий минимум 10 мкс
	lcd_rst(1);
}

//-- Самотест шины (без протокола T6963C) ----------------------------------------
uint8_t lcd_selftest(void)
{
	lcd_gpio_init();

	uint8_t first = lcd_read_status_raw();
	if (first == 0x00u || first == 0xFFu)
		return 0;

	for (uint16_t i = 0; i < 1000; i++)
	{
		uint8_t s = lcd_read_status_raw();
		if (s != first || s == 0x00u || s == 0xFFu)
			return 0;
	}

	return 1;
}

//-- Инициализация контроллера ---------------------------------------------------
void lcd_init(void)
{
	lcd_gpio_init();
	lcd_hw_reset();

	lcd_cmd2(LCD_CMD_TEXT_HOME, (uint8_t)LCD_TEXT_HOME, (uint8_t)(LCD_TEXT_HOME >> 8));
	lcd_cmd2(LCD_CMD_TEXT_AREA, (uint8_t)LCD_TEXT_AREA, 0x00);
	lcd_cmd2(LCD_CMD_GRAPHIC_HOME, (uint8_t)LCD_GRAPHIC_HOME, (uint8_t)(LCD_GRAPHIC_HOME >> 8));
	lcd_cmd2(LCD_CMD_GRAPHIC_AREA, (uint8_t)LCD_GRAPHIC_AREA, 0x00);

	lcd_cmd0(LCD_CMD_MODE_SET_OR_INT); // OR, внутренний CG
	lcd_cmd0(LCD_CMD_DISPMODE_BOTH);   // одновременно графика и текст

	lcd_fill_area(LCD_TEXT_HOME, 0x00, LCD_TEXT_FRAME_SIZE);
	lcd_clear();
}

void lcd_clear(void)
{
	lcd_fill(LCD_BLANK_BYTE);
}

// Заливка всей графической области "сырым" байтом памяти напрямую,
// без учёта инверсии полярности (см. LCD_INVERT)
void lcd_fill(uint8_t value)
{
	lcd_fill_area(LCD_GRAPHIC_HOME, value, LCD_GRAPHIC_FRAME_SIZE);
}

//-- Графика -----------------------------------------------------------------
void lcd_set_pixel(uint16_t x, uint16_t y, uint8_t on)
{
	if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
		return;

	uint16_t addr = LCD_GRAPHIC_HOME + (uint16_t)y * LCD_M + (x >> 3);
	uint8_t bit = x & 7u; // T6963C: b2b1b0=0 -> D0 = левый пиксель байта

	lcd_set_address_pointer(addr);
	lcd_cmd0((on ? LCD_PIXEL_ON_CMD : LCD_PIXEL_OFF_CMD) | bit);
}

void lcd_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
	int16_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
	int16_t sx = (x0 < x1) ? 1 : -1;
	int16_t dy = (y1 > y0) ? (y0 - y1) : (y1 - y0); // отрицательное значение
	int16_t sy = (y0 < y1) ? 1 : -1;
	int16_t err = dx + dy;

	for (;;)
	{
		lcd_set_pixel((uint16_t)x0, (uint16_t)y0, 1);
		if (x0 == x1 && y0 == y1)
			break;
		int16_t e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

void lcd_draw_rect(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
	lcd_draw_line(x0, y0, x1, y0);
	lcd_draw_line(x1, y0, x1, y1);
	lcd_draw_line(x1, y1, x0, y1);
	lcd_draw_line(x0, y1, x0, y0);
}

//-- Текст (внутренний CG контроллера) -------------------------------------------
// Внутренний CG ROM T6963C/SAP1024B адресуется кодом (ASCII - 0x20):
// пробел там = 0x00, а не 0x20, как в обычной таблице ASCII. Наружу API
// принимает обычные символы, сдвиг делаем здесь.
#define LCD_CHAR_TO_CGCODE(c) ((uint8_t)((c) - 0x20))

void lcd_put_char(uint8_t col, uint8_t row, char c)
{
	if (col >= LCD_M || row >= LCD_N)
		return;

	uint16_t addr = LCD_TEXT_HOME + (uint16_t)row * LCD_M + col;
	lcd_set_address_pointer(addr);
	lcd_cmd1(LCD_CMD_WRITE_INC, LCD_CHAR_TO_CGCODE(c));
}

void lcd_put_string(uint8_t col, uint8_t row, const char *str)
{
	if (row >= LCD_N)
		return;

	uint16_t addr = LCD_TEXT_HOME + (uint16_t)row * LCD_M + col;
	lcd_set_address_pointer(addr);

	for (; *str && col < LCD_M; str++, col++)
		lcd_cmd1(LCD_CMD_WRITE_INC, LCD_CHAR_TO_CGCODE(*str)); // адрес сам сдвигается (+inc)
}
