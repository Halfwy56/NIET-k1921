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

// Одна цифра 0..9 по центру экрана - через встроенный символогенератор
// контроллера (текстовый слой), а не отрисовкой пикселей: смена мгновенная.
#define BIGDIGIT_COL (LCD_M / 2)
#define BIGDIGIT_ROW (LCD_N / 2)

static uint8_t lcd_digit = 0;

static void lcd_draw_big_digit(uint8_t digit)
{
	if (digit > 9)
		return;
	lcd_put_char(BIGDIGIT_COL, BIGDIGIT_ROW, (char)('0' + digit));
}

// Приём строки из UART (до '\r'/'\n') и вывод её на верхнюю текстовую
// строку экрана. Строка всегда дополняется пробелами до ширины экрана,
// чтобы затереть хвост предыдущего, более длинного сообщения.
#define UART_MSG_ROW 0

static char uart_msg_buf[LCD_M + 1];
static uint8_t uart_msg_len = 0;

static void lcd_show_message(const char *msg)
{
	char line[LCD_M + 1];
	uint8_t i = 0;
	for (; i < LCD_M && msg[i]; i++)
		line[i] = msg[i];
	for (; i < LCD_M; i++)
		line[i] = ' ';
	line[LCD_M] = '\0';
	lcd_put_string(0, UART_MSG_ROW, line);
}

// Ненавязчивый (неблокирующий) опрос UART0: если данных нет - сразу выходим.
static void uart_poll(void)
{
	if (UART0->FR_bit.RXFE)
		return;

	uint32_t dr = UART0->DR; // биты 8-11 - FE/PE/BE/OE прямо рядом с данными
	char ch = (char)(dr & 0xFF);
	if (dr & 0xF00)
	{
		// Байт с ошибкой (например, break-condition - RX держится в 0) -
		// не настоящие данные, в буфер сообщения не кладём.
		printf("UART RX: 0x%02X ERR(FE=%d PE=%d BE=%d OE=%d)\r\n", (unsigned)(uint8_t)ch,
		       (int)((dr >> 8) & 1), (int)((dr >> 9) & 1), (int)((dr >> 10) & 1), (int)((dr >> 11) & 1));
		UART0->RSR = 0; // сброс флагов ошибок (запись любого значения в RSR/ECR)
		return;
	}

	printf("UART RX: 0x%02X\r\n", (unsigned)(uint8_t)ch); // ДИАГНОСТИКА

	if (ch == '\r' || ch == '\n')
	{
		if (uart_msg_len > 0)
		{
			uart_msg_buf[uart_msg_len] = '\0';
			lcd_show_message(uart_msg_buf);
			printf("LCD message: \"%s\"\r\n", uart_msg_buf);
			uart_msg_len = 0;
		}
		return;
	}

	if (uart_msg_len < LCD_M)
		uart_msg_buf[uart_msg_len++] = ch;
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
		lcd_draw_big_digit(lcd_digit);
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
  // Каждое срабатывание таймера переключает диод, т.е. на 1 цикл мигания
  // нужно 2 срабатывания -> частота таймера в 2 раза выше BLINK_FREQ_HZ
  TMR0_init(SystemCoreClock/(BLINK_FREQ_HZ*2));
  InterruptEnable();

  // Уровень кнопки USER BTN (PA13) в состоянии покоя - определяем при
  // старте, чтобы не зависеть от того, притянута она к 0 или к 1
  uint8_t btn_idle_level = (GPIOA->DATA & BTN_PIN_MSK) ? 1 : 0;
  uint8_t btn_pressed = 0;

  while(1)
  {
#if !USE_MAX7219
    if (lcd_ready)
      uart_poll();
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
        if (lcd_ready)
        {
          lcd_digit = (lcd_digit + 1) % 10;
          lcd_draw_big_digit(lcd_digit);
          printf("Button pressed: digit=%d\r\n", lcd_digit);
        }
        else
        {
          printf("Button pressed\r\n");
        }
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
	BSP_LED_Toggle();

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
