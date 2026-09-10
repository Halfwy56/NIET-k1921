/*==============================================================================
 * Пример работы GPIO для K1921VG5T
 *------------------------------------------------------------------------------
 * НИИЭТ, Александр Дыхно <dykhno@niiet.ru>
 *==============================================================================
 * ДАННОЕ ПРОГРАММНОЕ ОБЕСПЕЧЕНИЕ ПРЕДОСТАВЛЯЕТСЯ «КАК ЕСТЬ», БЕЗ КАКИХ-ЛИБО
 * ГАРАНТИЙ, ЯВНО ВЫРАЖЕННЫХ ИЛИ ПОДРАЗУМЕВАЕМЫХ, ВКЛЮЧАЯ ГАРАНТИИ ТОВАРНОЙ
 * ПРИГОДНОСТИ, СООТВЕТСТВИЯ ПО ЕГО КОНКРЕТНОМУ НАЗНАЧЕНИЮ И ОТСУТСТВИЯ
 * НАРУШЕНИЙ, НО НЕ ОГРАНИЧИВАЯСЬ ИМИ. ДАННОЕ ПРОГРАММНОЕ ОБЕСПЕЧЕНИЕ
 * ПРЕДНАЗНАЧЕНО ДЛЯ ОЗНАКОМИТЕЛЬНЫХ ЦЕЛЕЙ И НАПРАВЛЕНО ТОЛЬКО НА
 * ПРЕДОСТАВЛЕНИЕ ДОПОЛНИТЕЛЬНОЙ ИНФОРМАЦИИ О ПРОДУКТЕ, С ЦЕЛЬЮ СОХРАНИТЬ ВРЕМЯ
 * ПОТРЕБИТЕЛЮ. НИ В КАКОМ СЛУЧАЕ АВТОРЫ ИЛИ ПРАВООБЛАДАТЕЛИ НЕ НЕСУТ
 * ОТВЕТСТВЕННОСТИ ПО КАКИМ-ЛИБО ИСКАМ, ЗА ПРЯМОЙ ИЛИ КОСВЕННЫЙ УЩЕРБ, ИЛИ
 * ПО ИНЫМ ТРЕБОВАНИЯМ, ВОЗНИКШИМ ИЗ-ЗА ИСПОЛЬЗОВАНИЯ ПРОГРАММНОГО ОБЕСПЕЧЕНИЯ
 * ИЛИ ИНЫХ ДЕЙСТВИЙ С ПРОГРАММНЫМ ОБЕСПЕЧЕНИЕМ.
 *
 *                              2025 АО "НИИЭТ"
 *==============================================================================
 */

//-- Includes ------------------------------------------------------------------
#include <K1921VG5T.h>
#include <system_k1921vg5t.h>
#include "retarget.h"
#include "bsp.h"
#include "max7219.h"
#include "lcd.h"
#include "ups_ui.h"
#include <string.h>

//-- Defines -------------------------------------------------------------------

// Частота мигания светодиода, Гц (полных циклов вкл/выкл в секунду)
#define BLINK_FREQ_HZ 10

// MAX7219 и ЖКИ SAP1024B делят пины PB4/PB5/PB7 - одновременно работать не
// могут. Пока идёт проверка/отладка ЖКИ, матрица временно отключена.
#define USE_MAX7219 0

#if USE_MAX7219
// Шрифт цифр 1-5 для матрицы 8x8 (по строкам, старший бит - левый столбец)
static const uint8_t digit_font[5][8] = {
	{0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C}, // 1
	{0x78, 0x84, 0x04, 0x08, 0x10, 0x20, 0x40, 0xFC}, // 2
	{0x78, 0x84, 0x04, 0x38, 0x04, 0x84, 0x84, 0x78}, // 3
	{0x08, 0x18, 0x28, 0x48, 0x88, 0xFC, 0x08, 0x08}, // 4
	{0xFC, 0x80, 0x80, 0xF8, 0x04, 0x84, 0x84, 0x78}, // 5
};

// Картинка на 6-е нажатие (заказанный пользователем узор)
static const uint8_t picture6[8] = {
	0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x7E, 0x7E,
};

// 0 - бегущий огонёк, 1..5 - цифра, 6 - картинка picture6
static volatile uint8_t digit_mode = 0;
#else
// Устанавливается в 1 после успешной lcd_init() - до этого кнопка не рисует
static volatile uint8_t lcd_ready = 0;

// Счётчик миллисекунд, инкрементируется в TMR0_IRQHandler (таймер на 1 кГц)
static volatile uint32_t ms_ticks = 0;

// Фреймбуфер UI - 160x160, экран - 240x128. Кадр выводится ПОВЁРНУТЫМ на 90°
// по часовой стрелке:
//     экран_X = (FB_H-1) - кадр_Y (160 из 240 - помещается, центрируем)
//     экран_Y = кадр_X            (из 160 строк влезают 128)
// Левый край кадра виден полностью, не помещаются 32 столбца справа - они
// отбрасываются.
// Отступ слева. Адресация графической памяти байтовая, поэтому сдвиг кратен
// 8 px: 5 байт по центру + 3 байта сдвига вправо = 8 байт = 64 px.
#define UI_X_BYTE_OFFSET (((LCD_M - FB_STRIDE) / 2) + 3)

// Буфер экрана в ОЗУ. Повёрнутая картинка собирается здесь, а в ЖКИ уходят
// целые строки одним заходом: одна установка адреса на строку вместо одной
// на каждый байт (после поворота тайл ложится в столбец, и без буфера каждый
// байт требовал бы отдельной адресации - это и тормозило отклик).
static uint8_t scr_buf[LCD_HEIGHT][LCD_M];
static uint8_t scr_lo[LCD_HEIGHT], scr_hi[LCD_HEIGHT]; // диапазон изменённых байт

static void scr_touch(uint16_t y, uint8_t x_byte, uint8_t value)
{
	if (y >= LCD_HEIGHT || x_byte >= LCD_M)
		return;
	if (scr_buf[y][x_byte] == value && scr_lo[y] <= scr_hi[y])
		return;
	scr_buf[y][x_byte] = value;
	if (scr_lo[y] > scr_hi[y]) { scr_lo[y] = scr_hi[y] = x_byte; return; }
	if (x_byte < scr_lo[y]) scr_lo[y] = x_byte;
	if (x_byte > scr_hi[y]) scr_hi[y] = x_byte;
}

// Вывести накопленные изменения: по одной адресации на изменённую строку
static void scr_flush(void)
{
	for (uint16_t y = 0; y < LCD_HEIGHT; y++) {
		if (scr_lo[y] > scr_hi[y])
			continue;
		lcd_write_row(scr_lo[y], y, &scr_buf[y][scr_lo[y]],
		              (uint16_t)(scr_hi[y] - scr_lo[y] + 1));
		scr_lo[y] = LCD_M;   // пометить строку как чистую
		scr_hi[y] = 0;
	}
}

static void scr_init(void)
{
	memset(scr_buf, 0, sizeof scr_buf);
	for (uint16_t y = 0; y < LCD_HEIGHT; y++) { scr_lo[y] = LCD_M; scr_hi[y] = 0; }
}

// Мост UI -> ЖКИ: разбираем пакет дельта-передачи и складываем изменившиеся
// тайлы в буфер экрана. Формат: A5 5A 10 lenL lenH <payload> crc8,
// payload = серии (ty, tx0, n, n*8 байт), тайл 8x8 px = 8 байт по строкам.
//
// При повороте столбец кадра становится строкой экрана, поэтому каждый тайл
// 8x8 транспонируется.
int link_send(const uint8_t *p, size_t n)
{
	if (n < 6 || p[0] != 0xA5 || p[1] != 0x5A)
		return -1;

	size_t len = (size_t)p[3] | ((size_t)p[4] << 8);
	if (len + 6 > n)
		return -1;

	const uint8_t *pl = p + 5;
	size_t i = 0;
	while (i + 3 <= len) {
		uint8_t ty = pl[i], tx0 = pl[i + 1], cnt = pl[i + 2];
		i += 3;
		if (i + (size_t)cnt * 8 > len)
			break;

		for (uint8_t k = 0; k < cnt; k++) {
			const uint8_t *tile = &pl[i + (size_t)k * 8];
			uint8_t tx = tx0 + k;

			// Транспонирование: out[j] - строка экрана, бит (7-r) - пиксель,
			// пришедший из строки r тайла и столбца j.
			uint8_t out[8] = { 0 };
			for (int j = 0; j < 8; j++)
				for (int r = 0; r < 8; r++)
					if (tile[r] & (0x80u >> j))
						out[j] |= (uint8_t)(0x80u >> (7 - r));

			for (int j = 0; j < 8; j++) {
				int y = (int)tx * 8 + j;
				if (y >= LCD_HEIGHT)
					break;      // правые столбцы кадра за пределами экрана
				scr_touch((uint16_t)y,
				          (uint8_t)(UI_X_BYTE_OFFSET + (FB_STRIDE - 1 - ty)),
				          out[j]);
			}
		}
		i += (size_t)cnt * 8;
	}
	return 0;
}

// Времени с RTC у нас нет - показываем время с момента старта.
void ui_get_datetime(char *out /* >= 21 байт */)
{
	uint32_t s = ms_ticks / 1000u;
	uint32_t hh = (s / 3600u) % 100u, mm = (s / 60u) % 60u, ss = s % 60u;
	static const char tpl[] = "UPTIME      00:00:00";
	memcpy(out, tpl, sizeof tpl);
	out[12] = (char)('0' + hh / 10); out[13] = (char)('0' + hh % 10);
	out[15] = (char)('0' + mm / 10); out[16] = (char)('0' + mm % 10);
	out[18] = (char)('0' + ss / 10); out[19] = (char)('0' + ss % 10);
}

// Демо-данные вместо реального контроллера ИБП: публикуем снимок состояния,
// иначе UI покажет "NO LINK". Режим берём из пункта MODE меню SETUP - он же
// меняется прямыми командами терминала (ONLINE/BATTERY/...).
static uint8_t demo_fault = 7;   // код, показываемый как "FAULT E07"

static void ups_demo_publish(uint32_t now)
{
	ups_mode_t demo_mode = ui_menu_mode();

	ups_state_t s;
	memset(&s, 0, sizeof s);
	s.stamp_ms = now;
	s.mode     = demo_mode;
	s.vin_dV   = 2295;
	s.vout_dV  = 2300;
	s.fout_cHz = 5000;
	s.load_pct = 45;

	if (demo_mode == UPS_ONLINE) {
		s.vbat_cV = 5460; s.soc_pct = 100; s.rt_min = 32;   // АКБ заряжена
	} else {
		s.vbat_cV = 5210; s.soc_pct = 74;  s.rt_min = 21;   // разряжается
	}
	s.fault = (demo_mode == UPS_FAULT) ? demo_fault : 0;

	ups_state_publish(&s);
}

// -------------------------------------- команды из терминала (смена экрана)

#define UART_MSG_IDLE_MS 100u   // тишина, после которой строка считается введённой

static char     uart_msg_buf[24];
static uint8_t  uart_msg_len = 0;
static uint32_t uart_last_ms = 0;

// Сравнение без учёта регистра - strcasecmp в nano-версии libc может не быть
static int str_ieq(const char *a, const char *b)
{
	for (;; a++, b++) {
		char ca = *a, cb = *b;
		if (ca >= 'a' && ca <= 'z') ca -= 32;
		if (cb >= 'a' && cb <= 'z') cb -= 32;
		if (ca != cb) return 0;
		if (!ca) return 1;
	}
}

// Команды терминала заменяют кнопки панели: навигация по меню SETUP и
// правка значений. Плюс быстрая смена режима мнемосхемы напрямую.
static void uart_command(const char *cmd)
{
	if (str_ieq(cmd, "UP") || str_ieq(cmd, "U")) {
		ui_key(KEY_UP);
		printf("KEY UP\r\n");
	}
	else if (str_ieq(cmd, "DOWN") || str_ieq(cmd, "D")) {
		ui_key(KEY_DOWN);
		printf("KEY DOWN\r\n");
	}
	else if (str_ieq(cmd, "ENTER") || str_ieq(cmd, "ENT") || str_ieq(cmd, "E")) {
		ui_key(KEY_ENTER);
		printf("KEY ENTER\r\n");
	}
	else if (str_ieq(cmd, "ESC") || str_ieq(cmd, "BACK")) {
		ui_key(KEY_ESC);
		printf("KEY ESC\r\n");
	}
	else if (str_ieq(cmd, "ONLINE"))       { ui_menu_set_mode(UPS_ONLINE);  printf("Mode: ONLINE\r\n"); }
	else if (str_ieq(cmd, "BATTERY") ||
	         str_ieq(cmd, "BAT"))          { ui_menu_set_mode(UPS_BATTERY); printf("Mode: BATTERY\r\n"); }
	else if (str_ieq(cmd, "BYPASS") ||
	         str_ieq(cmd, "BYPAS"))        { ui_menu_set_mode(UPS_BYPASS);  printf("Mode: BYPASS\r\n"); }
	else if (str_ieq(cmd, "FAULT"))        { ui_menu_set_mode(UPS_FAULT);   printf("Mode: FAULT\r\n"); }
	else {
		printf("Unknown command: \"%s\"\r\n", cmd);
		printf("Keys: UP | DOWN | ENTER | ESC\r\n");
		printf("Mode: ONLINE | BATTERY | BYPASS | FAULT\r\n");
	}
}

static void uart_flush_message(void)
{
	if (uart_msg_len == 0)
		return;
	uart_msg_buf[uart_msg_len] = '\0';
	uart_command(uart_msg_buf);
	uart_msg_len = 0;
}

// Неблокирующий опрос UART0: если данных нет - сразу выходим
static void uart_poll(void)
{
	if (UART0->FR_bit.RXFE)
		return;

	uint32_t dr = UART0->DR; // биты 8-11 - FE/PE/BE/OE прямо рядом с данными
	char ch = (char)(dr & 0xFF);
	if (dr & 0xF00)
	{
		// Байт с ошибкой (break-condition: линия RX висит в нуле) - не данные
		UART0->RSR = 0; // сброс флагов ошибок (запись любого значения в RSR/ECR)
		return;
	}

	uart_last_ms = ms_ticks;

	if (ch == '\r' || ch == '\n') {
		uart_flush_message();
		return;
	}
	if (ch == ' ' || ch == '\t')      // пробелы в командах не нужны
		return;

	if (uart_msg_len < sizeof uart_msg_buf - 1)
		uart_msg_buf[uart_msg_len++] = ch;
}

// Конец команды по паузе в приёме (терминал может не слать '\r'/'\n')
static void uart_idle_check(void)
{
	if (uart_msg_len > 0 && (ms_ticks - uart_last_ms) >= UART_MSG_IDLE_MS)
		uart_flush_message();
}
#endif // USE_MAX7219

void TMR0_IRQHandler();

void TMR0_init(uint32_t period)
{
  RCU->CGCFGAPB_bit.TMR0EN = 1;
  RCU->RSTDISAPB_bit.TMR0EN = 1;

  //Записываем значение периода
  TMR0->PERIOD = period-1;
  //Выбираем режим счета от 0 до значения PERIOD
  TMR0->CTRL_bit.MODE = TMR_CTRL_MODE_Up;

  //Разрешаем прерывание по совпадению значения счетчика и PERIOD
  TMR0->IM_bit.TMR = 1;

  // Настраиваем обработчик прерывания для TMR32
  PLIC_SetIrqHandler (Plic_Mach_Target, IsrVect_IRQ_TMR0, TMR0_IRQHandler);
  PLIC_SetPriority   (IsrVect_IRQ_TMR0, 0x1);
  PLIC_SetMode		 (IsrVect_IRQ_TMR0,PLIC_IRQMODE_HILEVEL);
  PLIC_IntEnable     (Plic_Mach_Target, IsrVect_IRQ_TMR0);
  SIU->CNTEN_bit.TMR0EN = 1;
}

//-- Peripheral init functions -------------------------------------------------
void periph_init()
{
	SystemInit();
	SystemCoreClockUpdate();
	BSP_LED_Init();
	retarget_init();
	printf("K1921VG5T SYSCLK = %d MHz\r\n",(int)(SystemCoreClock / 1000000));
	printf("  Start RunLeds\r\n");
#if USE_MAX7219
	MAX7219_Init();
#else
	if (lcd_selftest())
	{
		printf("LCD selftest: OK\r\n");
		lcd_init(); // уже очищает и текстовую, и графическую область
		scr_init();
		ui_init();
		printf("LCD init done\r\n");
		lcd_ready = 1;
	}
	else
	{
		printf("LCD selftest: FAIL\r\n");
	}
#endif
}

//--- USER FUNCTIONS ----------------------------------------------------------------------

void delay(uint32_t a)
{
	while(a--) __asm("nop");
}

//-- Main ----------------------------------------------------------------------
int main(void)
{
  periph_init();
  // Таймер на 1 кГц: даёт счётчик миллисекунд для UI, а светодиод мигает
  // по этому же счётчику (см. TMR0_IRQHandler).
  TMR0_init(SystemCoreClock / 1000u);
  InterruptEnable();

  // Уровень кнопки USER BTN (PA13) в состоянии покоя - определяем при
  // старте, чтобы не зависеть от того, притянута она к 0 или к 1
  uint8_t btn_idle_level = (GPIOA->DATA & BTN_PIN_MSK) ? 1 : 0;
  uint8_t btn_pressed = 0;

  while(1)
  {
#if !USE_MAX7219
    if (lcd_ready)
    {
      uart_poll();
      uart_idle_check();
      ups_demo_publish(ms_ticks); // пока нет реального контроллера ИБП
      ui_tick(ms_ticks);          // рендер + дельта складываются в scr_buf
      scr_flush();                // и уходят на ЖКИ целыми строками
    }
#endif

    uint8_t btn_level = (GPIOA->DATA & BTN_PIN_MSK) ? 1 : 0;

    if (btn_level != btn_idle_level && !btn_pressed)
    {
      delay(50000); // антидребезг
      if (((GPIOA->DATA & BTN_PIN_MSK) ? 1 : 0) != btn_idle_level)
      {
        btn_pressed = 1;
#if USE_MAX7219
        digit_mode = (digit_mode % 6) + 1;
        MAX7219_SetBuffer(digit_mode == 6 ? picture6 : digit_font[digit_mode - 1]);
        printf("Button pressed, digit=%d\r\n", digit_mode);
#else
        ui_key(KEY_DOWN);   // кнопка листает экраны UI
        printf("Button pressed: next screen\r\n");
#endif
      }
    }
    else if (btn_level == btn_idle_level)
    {
      btn_pressed = 0;
    }
  }

  return 0;
}


//-- IRQ INTERRUPT HANDLERS ---------------------------------------------------------------
void TMR0_IRQHandler()
{
#if !USE_MAX7219
	ms_ticks++;                                   // таймер настроен на 1 кГц
	if (ms_ticks % (500u / BLINK_FREQ_HZ) == 0)   // полупериод мигания
		BSP_LED_Toggle();
#else
	BSP_LED_Toggle();
#endif

#if USE_MAX7219
	// "Бегущий огонёк" по одной строке матрицы MAX7219, пока не показана цифра
	if (!digit_mode)
	{
		static uint8_t col = 0;
		MAX7219_SetRow(1, 1 << col);
		col = (col + 1) % 8;
	}
#endif

    //Сбрасываем флаг прерывания таймера
    TMR0->IC = 1;
}
