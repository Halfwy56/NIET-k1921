/*==============================================================================
 * Драйвер CAN для K1921VG5T (модуль MultiCAN), узел 0 = "CAN0"
 *==============================================================================
 */

#ifndef CAN_H
#define CAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

//-- Подключение ---------------------------------------------------------------
// Узел 0 модуля MultiCAN выведен на PA14 (Rx) и PA15 (Tx).
//
// Между этими выводами и разъёмом CAN_H/CAN_L обязателен приёмопередатчик
// (SN65HVD230 и т.п.) и терминаторы 120 Ом по концам линии. Это цифровые
// выводы МК, напрямую на CAN_H/CAN_L их заводить нельзя.
//
// Номер альтфункции: AF2, подобран на плате перебором. Таблицы AF в SDK нет
// ни в заголовках, ни в plib5t. Признак верного варианта - модуль CAN держит
// Tx в рецессиве: при AF2 на PA15 единица, при AF1 и AF3 - ноль.
#define CAN_PIN_PORT     GPIOA
#define CAN_PIN_PORT_EN  GPIOAEN
#define CAN_PIN_RX       14
#define CAN_PIN_TX       15
#define CAN_PIN_AF       2

#define CAN_NODE         0          // узел MultiCAN: 0 = CAN0, 1 = CAN1
#define CAN_BAUD_HZ      500000u    // 500 кбит/с, точка выборки 87.5% (CiA)

//-- Кадр ----------------------------------------------------------------------
typedef struct {
    uint32_t id;        // идентификатор (11 бит для стандартного кадра)
    uint8_t  len;       // 0..8
    uint8_t  data[8];
} can_frame_t;

//-- API -----------------------------------------------------------------------
void can_init(void);

// Поставить кадр на передачу. false - предыдущий ещё не ушёл.
bool can_send(uint32_t id, const uint8_t *data, uint8_t len);
bool can_tx_busy(void);     // кадр ещё в очереди на передачу
void can_abort_tx(void);    // отменить зависшую передачу

// Забрать принятый кадр. false - ничего не пришло.
bool can_recv(can_frame_t *f);

uint8_t     can_last_error(void);              // код ошибки NSR.LEC
const char *can_error_str(uint8_t lec);
bool        can_bus_off(void);                 // узел отключён от шины по ошибкам
void        can_print_status(const char *tag);
void        can_reset(void);                   // поднять узел заново

#ifdef __cplusplus
}
#endif

#endif // CAN_H
