/*==============================================================================
 * Драйвер MAX7219 для матрицы 8x8 (K1921VG5T), аппаратный SPI0
 *==============================================================================
 */

#include "max7219.h"
#include <K1921VG5T.h>

//-- MAX7219 регистры ------------------------------------------------------------
#define MAX7219_REG_NOOP        0x00
#define MAX7219_REG_DIGIT0      0x01
#define MAX7219_REG_DECODEMODE  0x09
#define MAX7219_REG_INTENSITY   0x0A
#define MAX7219_REG_SCANLIMIT   0x0B
#define MAX7219_REG_SHUTDOWN    0x0C
#define MAX7219_REG_DISPLAYTEST 0x0F

#define MAX7219_DIN_MSK (1 << MAX7219_DIN_POS)
#define MAX7219_CLK_MSK (1 << MAX7219_CLK_POS)
#define MAX7219_CS_MSK  (1 << MAX7219_CS_POS)

// Делитель частоты SCK: SCK = PCLK / (SPI_CPSDVSR * (1 + SPI_SCR)) = PCLK/32.
// Даже при полной тактовой частоте ядра остаётся с большим запасом
// ниже максимума MAX7219 (10 МГц).
#define SPI_CPSDVSR 2
#define SPI_SCR     15

// Ограничение на ожидание готовности SPI0, чтобы при аппаратной проблеме
// (например, неверно выбранная альтфункция пина) не зависнуть навсегда
// внутри обработчика прерывания.
#define SPI_BSY_TIMEOUT 100000

static void max7219_send(uint8_t reg, uint8_t data)
{
	uint32_t timeout = SPI_BSY_TIMEOUT;
	while (MAX7219_SPI->SR_bit.BSY && --timeout);

	MAX7219_SPI->DR = ((uint16_t)reg << 8) | data;

	timeout = SPI_BSY_TIMEOUT;
	while (MAX7219_SPI->SR_bit.BSY && --timeout);
}

void MAX7219_Init(void)
{
	RCU->CGCFGAHB_bit.MAX7219_PORT_EN = 1;
	RCU->RSTDISAHB_bit.MAX7219_PORT_EN = 1;
	// Тактирование регистров SPI0 на шине APB (доступ CPU к CR/DR/SR и т.д.)
	RCU->CGCFGAPB_bit.MAX7219_SPI_EN = 1;
	RCU->RSTDISAPB_bit.MAX7219_SPI_EN = 1;
	// Тактирование самого ядра SPI0 (генератор SCK и сдвиговый регистр) -
	// без этого BSY никогда не снимается и передача физически не идёт.
	RCU->SPICFG[SPI0_Num].SPICFG_bit.CLKSEL = RCU_SPICFG_CLKSEL_HSECLK;
	RCU->SPICFG[SPI0_Num].SPICFG_bit.CLKEN = 1;
	RCU->SPICFG[SPI0_Num].SPICFG_bit.RSTDIS = 1;

	// Переводим пины CLK/FSS/TX в альтернативную функцию SPI0 (AF2)
	MAX7219_PORT->ALTFUNCNUM_bit.PIN4 = 2;
	MAX7219_PORT->ALTFUNCNUM_bit.PIN5 = 2;
	MAX7219_PORT->ALTFUNCNUM_bit.PIN7 = 2;
	MAX7219_PORT->ALTFUNCSET = MAX7219_CLK_MSK | MAX7219_CS_MSK | MAX7219_DIN_MSK;

	MAX7219_SPI->CPSR = SPI_CPSDVSR;
	// Настраиваем всё одним словом, чтобы не было промежуточных состояний:
	// 16 бит слово, формат SPI (Motorola), CPOL=0/CPHA=0 (SPI mode 0),
	// master, делитель SCR, приёмопередатчик сразу включен (SSE=1).
	MAX7219_SPI->CR = (SPI_CR_DSS_16bit << SPI_CR_DSS_Pos) |
	                  (SPI_SCR          << SPI_CR_SCR_Pos) |
	                  (SPI_CR_FRF_SPI   << SPI_CR_FRF_Pos) |
	                  (0                << SPI_CR_SPO_Pos) |
	                  (0                << SPI_CR_SPH_Pos) |
	                  (0                << SPI_CR_MS_Pos)  |
	                  (1                << SPI_CR_SSE_Pos);

	max7219_send(MAX7219_REG_SHUTDOWN, 0x00);    // выключить (shutdown mode)
	max7219_send(MAX7219_REG_DISPLAYTEST, 0x00); // тестовый режим выключен
	max7219_send(MAX7219_REG_DECODEMODE, 0x00);  // без BCD-декодера - режим матрицы
	max7219_send(MAX7219_REG_SCANLIMIT, 0x07);   // сканировать все 8 строк
	max7219_send(MAX7219_REG_INTENSITY, 0x08);   // яркость по умолчанию
	MAX7219_Clear();
	max7219_send(MAX7219_REG_SHUTDOWN, 0x01);    // включить (normal mode)
}

void MAX7219_Clear(void)
{
	for (uint8_t row = 1; row <= 8; row++)
		max7219_send(MAX7219_REG_DIGIT0 + row - 1, 0x00);
}

void MAX7219_SetRow(uint8_t row, uint8_t value)
{
	max7219_send(MAX7219_REG_DIGIT0 + row - 1, value);
}

void MAX7219_SetBuffer(const uint8_t buf[8])
{
	for (uint8_t row = 1; row <= 8; row++)
		MAX7219_SetRow(row, buf[row - 1]);
}

void MAX7219_SetBrightness(uint8_t value)
{
	max7219_send(MAX7219_REG_INTENSITY, value & 0x0F);
}
