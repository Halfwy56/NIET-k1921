/*==============================================================================
 * Драйвер CAN для K1921VG5T (модуль MultiCAN), узел 0
 *
 * Работа опросом, без прерываний: так CAN не конкурирует с обработчиком
 * таймера, который обслуживает UI и шину ЖКИ.
 *==============================================================================
 */

#include "can.h"
#include <K1921VG5T.h>
#include <system_k1921vg5t.h>
#include <stdio.h>

//-- Объекты сообщений ---------------------------------------------------------
// Объект привязывается к узлу через "список": список 1 = узел 0, 2 = узел 1.
#define CAN_LIST      (CAN_NODE + 1)
#define CAN_OBJ_TX    0
#define CAN_OBJ_RX    1

//-- Тактирование и тайминги шины ----------------------------------------------
// FDR в режиме обычного делителя: f_CAN = f_вх / (1024 - STEP).
// STEP = 0x3FE -> деление на 2. При SYSCLK 16 МГц получаем f_CAN = 8 МГц.
#define CAN_FDR_STEP_VAL   0x3FEu

// Бит = 1 + (TSEG1+1) + (TSEG2+1) = 1 + 13 + 2 = 16 квантов, BRP+1 = 1.
// 8 МГц / 16 = 500 кбит/с, точка выборки 87.5% - то же, что даёт SJA1000
// с профилем CiA (BTR0=0x00, BTR1=0x1C) в адаптере CANwise.
#define CAN_BRP    0
#define CAN_TSEG1  12
#define CAN_TSEG2  1
#define CAN_SJW    1

// Циклы ожидания ограничены, чтобы неисправная периферия не вешала прошивку.
#define CAN_WAIT_TIMEOUT  100000u

static void can_panel_wait(void)
{
    uint32_t t = CAN_WAIT_TIMEOUT;
    while ((CAN->PANCTR_bit.BUSY || CAN->PANCTR_bit.RBUSY) && --t)
        ;
}

// Статическая привязка объекта сообщения к списку (команда панели 0x02)
static void can_obj_to_list(uint32_t obj, uint32_t list)
{
    can_panel_wait();
    CAN->PANCTR = (0x2u << CAN_PANCTR_PANCMD_Pos) |
                  (obj  << CAN_PANCTR_PANAR1_Pos) |
                  (list << CAN_PANCTR_PANAR2_Pos);
    can_panel_wait();
}

//-- Инициализация -------------------------------------------------------------

void can_init(void)
{
    /* 1. Выводы: тактирование порта + альтернативная функция CAN.
          ALTFUNCNUM содержит по 2 бита на вывод. */
    RCU->CGCFGAHB_bit.CAN_PIN_PORT_EN = 1;
    RCU->RSTDISAHB_bit.CAN_PIN_PORT_EN = 1;

    uint32_t afnum = CAN_PIN_PORT->ALTFUNCNUM;
    afnum &= ~((3u << (CAN_PIN_RX * 2)) | (3u << (CAN_PIN_TX * 2)));
    afnum |=  (((uint32_t)CAN_PIN_AF) << (CAN_PIN_RX * 2)) |
              (((uint32_t)CAN_PIN_AF) << (CAN_PIN_TX * 2));
    CAN_PIN_PORT->ALTFUNCNUM = afnum;
    CAN_PIN_PORT->ALTFUNCSET = (1u << CAN_PIN_RX) | (1u << CAN_PIN_TX);

    /* 2. Тактирование модуля и снятие сброса */
    RCU->CGCFGAHB_bit.CANEN = 1;
    RCU->RSTDISAHB_bit.CANEN = 1;

    CAN->CLC_bit.DISR = 0;                     // разрешить тактирование модуля
    uint32_t t = CAN_WAIT_TIMEOUT;
    while (CAN->CLC_bit.DISS && --t)           // дождаться запуска модуля
        ;

    CAN->FDR = (1u << CAN_FDR_DM_Pos) |        // режим обычного делителя
               (CAN_FDR_STEP_VAL << CAN_FDR_STEP_Pos);

    /* 3. Узел: правка настроек разрешена только при INIT = 1 и CCE = 1 */
    CAN->Node[CAN_NODE].NCR = CAN_Node_NCR_CCE_Msk | CAN_Node_NCR_INIT_Msk;
    CAN->Node[CAN_NODE].NBTR = (CAN_TSEG2 << CAN_Node_NBTR_TSEG2_Pos) |
                               (CAN_TSEG1 << CAN_Node_NBTR_TSEG1_Pos) |
                               (CAN_SJW   << CAN_Node_NBTR_SJW_Pos)   |
                               (CAN_BRP   << CAN_Node_NBTR_BRP_Pos);
    CAN->Node[CAN_NODE].NPCR = 0;              // рабочий режим

    /* 4. Объекты сообщений */
    can_obj_to_list(CAN_OBJ_TX, CAN_LIST);
    can_obj_to_list(CAN_OBJ_RX, CAN_LIST);

    /* Передающий. RESTXRQ обязателен: неподтверждённый кадр остаётся
       взведённым и заблокировал бы объект навсегда. */
    CANMSG->Msg[CAN_OBJ_TX].MOCTR = CANMSG_Msg_MOCTR_RESTXRQ_Msk |
                                    CANMSG_Msg_MOCTR_RESMSGVAL_Msk;
    CANMSG->Msg[CAN_OBJ_TX].MOFCR = 0;
    CANMSG->Msg[CAN_OBJ_TX].MOIPR = 0;
    CANMSG->Msg[CAN_OBJ_TX].MOAMR = 0;
    CANMSG->Msg[CAN_OBJ_TX].MOCTR = CANMSG_Msg_MOCTR_SETDIR_Msk |
                                    CANMSG_Msg_MOCTR_SETTXEN0_Msk |
                                    CANMSG_Msg_MOCTR_SETTXEN1_Msk;

    // Приёмный: нулевая маска - фильтр пропускает любой идентификатор
    CANMSG->Msg[CAN_OBJ_RX].MOCTR = CANMSG_Msg_MOCTR_RESNEWDAT_Msk |
                                    CANMSG_Msg_MOCTR_RESMSGVAL_Msk;
    CANMSG->Msg[CAN_OBJ_RX].MOFCR = 0;
    CANMSG->Msg[CAN_OBJ_RX].MOIPR = 0;
    CANMSG->Msg[CAN_OBJ_RX].MOAR  = 0;
    CANMSG->Msg[CAN_OBJ_RX].MOAMR = 0;         // AM = 0, MIDE = 0 -> принимаем всё
    CANMSG->Msg[CAN_OBJ_RX].MOCTR = CANMSG_Msg_MOCTR_RESDIR_Msk |
                                    CANMSG_Msg_MOCTR_SETRXEN_Msk |
                                    CANMSG_Msg_MOCTR_SETMSGVAL_Msk;

    /* 5. Узел на шину: снимаем INIT и CCE */
    CAN->Node[CAN_NODE].NCR = 0;
}

//-- Передача ------------------------------------------------------------------

bool can_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    if (len > 8)
        len = 8;

    if (CANMSG->Msg[CAN_OBJ_TX].MOSTAT_bit.TXRQ)
        return false;                          // предыдущий кадр ещё в очереди

    // На время правки объект выводим из работы
    CANMSG->Msg[CAN_OBJ_TX].MOCTR = CANMSG_Msg_MOCTR_RESMSGVAL_Msk;

    uint32_t lo = 0, hi = 0;
    for (uint8_t i = 0; i < len; i++) {
        if (i < 4) lo |= ((uint32_t)data[i]) << (8 * i);
        else       hi |= ((uint32_t)data[i]) << (8 * (i - 4));
    }
    CANMSG->Msg[CAN_OBJ_TX].MODATAL = lo;
    CANMSG->Msg[CAN_OBJ_TX].MODATAH = hi;

    // Стандартный кадр: 11-битный идентификатор лежит в разрядах 28..18 поля ID
    CANMSG->Msg[CAN_OBJ_TX].MOAR = (2u << CANMSG_Msg_MOAR_PRI_Pos) |
                                   ((id & 0x7FFu) << 18);
    CANMSG->Msg[CAN_OBJ_TX].MOFCR = ((uint32_t)len) << CANMSG_Msg_MOFCR_DLC_Pos;

    CANMSG->Msg[CAN_OBJ_TX].MOCTR = CANMSG_Msg_MOCTR_SETTXRQ_Msk |
                                    CANMSG_Msg_MOCTR_SETMSGVAL_Msk;
    return true;
}

bool can_tx_busy(void)
{
    return CANMSG->Msg[CAN_OBJ_TX].MOSTAT_bit.TXRQ ? true : false;
}

/* Снять запрос передачи, не трогая настройки узла. Пока кадр не подтверждён,
   узел повторяет его бесконечно и засыпает шину кадрами ошибок. */
void can_abort_tx(void)
{
    CANMSG->Msg[CAN_OBJ_TX].MOCTR = CANMSG_Msg_MOCTR_RESTXRQ_Msk |
                                    CANMSG_Msg_MOCTR_RESMSGVAL_Msk;
}

//-- Приём ---------------------------------------------------------------------

bool can_recv(can_frame_t *f)
{
    if (!CANMSG->Msg[CAN_OBJ_RX].MOSTAT_bit.NEWDAT)
        return false;

    CANMSG->Msg[CAN_OBJ_RX].MOCTR = CANMSG_Msg_MOCTR_RESNEWDAT_Msk;

    uint32_t moar = CANMSG->Msg[CAN_OBJ_RX].MOAR;
    uint32_t lo   = CANMSG->Msg[CAN_OBJ_RX].MODATAL;
    uint32_t hi   = CANMSG->Msg[CAN_OBJ_RX].MODATAH;
    uint8_t  len  = (uint8_t)CANMSG->Msg[CAN_OBJ_RX].MOFCR_bit.DLC;

    f->id = (moar & CANMSG_Msg_MOAR_IDE_Msk) ? (moar & 0x1FFFFFFFu)
                                             : ((moar >> 18) & 0x7FFu);
    if (len > 8)
        len = 8;
    f->len = len;
    for (uint8_t i = 0; i < len; i++)
        f->data[i] = (uint8_t)(((i < 4) ? (lo >> (8 * i)) : (hi >> (8 * (i - 4)))) & 0xFFu);

    return true;
}

//-- Диагностика ---------------------------------------------------------------

uint8_t can_last_error(void)
{
    return (uint8_t)CAN->Node[CAN_NODE].NSR_bit.LEC;
}

const char *can_error_str(uint8_t lec)
{
    static const char *const s[8] = {
        "нет ошибок", "ошибка стаффинга", "ошибка формата", "нет подтверждения",
        "Bit1: линия в нуле", "Bit0: линия не отзывается", "ошибка CRC", "не обновлялось"
    };
    return s[lec & 7u];
}

bool can_bus_off(void)
{
    return CAN->Node[CAN_NODE].NSR_bit.BOFF ? true : false;
}

void can_print_status(const char *tag)
{
    uint8_t  lec = (uint8_t)CAN->Node[CAN_NODE].NSR_bit.LEC;
    uint32_t nec = CAN->Node[CAN_NODE].NECNT;

    printf("CAN%d %s: %u бит/с, PA%d(Rx)/PA%d(Tx)\r\n",
           CAN_NODE, tag, (unsigned)CAN_BAUD_HZ, CAN_PIN_RX, CAN_PIN_TX);
    printf("  TXOK=%d RXOK=%d EWRN=%d BOFF=%d  TEC=%u REC=%u  LEC=%d (%s)\r\n",
           (int)CAN->Node[CAN_NODE].NSR_bit.TXOK,
           (int)CAN->Node[CAN_NODE].NSR_bit.RXOK,
           (int)CAN->Node[CAN_NODE].NSR_bit.EWRN,
           (int)CAN->Node[CAN_NODE].NSR_bit.BOFF,
           (unsigned)((nec >> 8) & 0xFFu), (unsigned)(nec & 0xFFu),
           (int)lec, can_error_str(lec));
}

/* Аварийный сброс: снять зависший запрос передачи и заново поднять узел. */
void can_reset(void)
{
    can_abort_tx();
    can_init();
}
