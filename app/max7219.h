/*==============================================================================
 * Драйвер MAX7219 для матрицы 8x8 (K1921VG5T), аппаратный SPI0
 *==============================================================================
 */

#ifndef MAX7219_H
#define MAX7219_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

//-- Пины подключения -------------------------------------------------------------
// Разъём XP6, группа "SPI0" (см. распиновку платы NIIET-MINI-K1921VG5T):
//   MAX7219 VCC -> +5V (XP6)      MAX7219 GND -> GND (XP6, рядом с SPI0)
//   MAX7219 DIN -> TX  = PB7 (пин 44) - альтфункция AF2 = SPI0_TX
//   MAX7219 CS  -> FSS = PB5 (пин 40) - альтфункция AF2 = SPI0_FSS, CS формируется аппаратно
//   MAX7219 CLK -> CLK = PB4 (пин 39) - альтфункция AF2 = SPI0_CLK
// Аппаратный блок SPI0 сам формирует CLK и держит FSS(CS) в 0 на время всего
// 16-битного кадра, что в точности соответствует протоколу MAX7219.
#define MAX7219_PORT     GPIOB
#define MAX7219_PORT_EN  GPIOBEN
#define MAX7219_SPI      SPI0
#define MAX7219_SPI_EN   SPI0EN
#define MAX7219_DIN_POS  7
#define MAX7219_CLK_POS  4
#define MAX7219_CS_POS   5

void MAX7219_Init(void);
void MAX7219_Clear(void);
// Отправить один столбец (байт) на строку 1-8 (row: 1..8, снизу разряд MAX7219 - "цифра")
void MAX7219_SetRow(uint8_t row, uint8_t value);
// Отправить всю матрицу сразу (buf[0..7] - строки 1..8)
void MAX7219_SetBuffer(const uint8_t buf[8]);
// Яркость 0x0..0xF
void MAX7219_SetBrightness(uint8_t value);

#ifdef __cplusplus
}
#endif

#endif // MAX7219_H
