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
// Устанавливается в 1 после успешной lcd_init() - до этого анимация не рисует
static volatile uint8_t lcd_ready = 0;

// "Бегущая полоса" из 1 пикселя: движется по диагонали от левого верхнего
// угла к правому нижнему (алгоритм Брезенхема), затем цикл начинается заново
static void lcd_anim_step(void)
{
	static int16_t x, y, err;
	static int16_t prev_x = -1, prev_y = -1;
	static uint8_t inited = 0;

	const int16_t dx = LCD_WIDTH - 1;
	const int16_t dy = -(LCD_HEIGHT - 1);

	if (!inited)
	{
		x = 0;
		y = 0;
		err = dx + dy;
		inited = 1;
	}

	if (prev_x >= 0)
		lcd_set_pixel((uint16_t)prev_x, (uint16_t)prev_y, 0);
	lcd_set_pixel((uint16_t)x, (uint16_t)y, 1);
	prev_x = x;
	prev_y = y;

	if (x == LCD_WIDTH - 1 && y == LCD_HEIGHT - 1)
	{
		inited = 0; // дошли до правого нижнего угла - начинаем заново
		prev_x = -1;
		return;
	}

	int16_t e2 = 2 * err;
	if (e2 >= dy) { err += dy; x++; }
	if (e2 <= dx) { err += dx; y++; }
}

// По нажатию кнопки: все пиксели загораются, по следующему нажатию - гаснут.
static uint8_t lcd_all_on = 0;
#endif // USE_MAX7219

void TMR0_IRQHandler();
extern void InterruptDisable(void); // есть в system_k1921vg5t.c, но не объявлена в заголовке

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
		lcd_draw_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
		lcd_put_string(1, 1, "SAP1024B OK");
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
          lcd_all_on = !lcd_all_on;
          // На время заливки запрещаем прерывания: TMR0_IRQHandler дёргает
          // lcd_anim_step(), и если он влезет посреди Auto Write своими
          // командами на ту же шину ЖКИ, протокол собьётся и заливка
          // получится неполной/повреждённой.
          InterruptDisable();
          lcd_fill(lcd_all_on ? 0xFF : 0x00);
          InterruptEnable();
          printf("Button pressed: LCD %s\r\n", lcd_all_on ? "all ON" : "all OFF");
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
#else
	if (lcd_ready)
		lcd_anim_step();
#endif

    //Сбрасываем флаг прерывания таймера
    TMR0->IC = 1;
}
