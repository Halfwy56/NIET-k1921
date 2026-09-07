# Makefile для проектов К1921ВГ5Т (плата NIIET-MINI / КФДЛ.441461.044)
#
# Флаги взяты из projects/NIIET-MINI-K1921VG5T/run_leds/aspect/niiet_aspect.json
# Класть рядом с папкой app/ внутри проекта, например:
#   k1921vg5t_sdk/projects/NIIET-MINI-K1921VG5T/run_leds/Makefile
#
# Сборка:    make            (или make all)
# Прошивка:  make flash
# Стирание:  make erase      (нужна перемычка XP8 SERVEN)
# Отладка:   make gdbserver  (в отдельном окне), затем make gdb
# Разбор:    make disasm     -> build/run_leds.lst
# Проверка:  make info       -> печатает пути и версии, ничего не собирает
#
# Вер. 2: команды оболочки переписаны на POSIX — make из kit НИИЭТ
#         использует собственный sh.exe, а не cmd.
# Вер. 3: OpenOCD переведён на сборку НИИЭТ (в kit-овской нет flash-драйвера
#         k1921vg5t); пути к OpenOCD сведены в один корень OCD_ROOT;
#         adapter_khz заменён на adapter speed через -c, чтобы не править
#         конфиг в SDK; добавлены disasm, info, program, reset.

# ============================================================================
# НАСТРОЙКИ — правь под свои пути
# ============================================================================

TARGET      := run_leds

# Префикс компилятора. Узнать точный:
#   dir -Recurse -Filter "*gcc.exe" C:\niiet\gcc
# Варианты: riscv64-unknown-elf-  (sc-dt, toolkit НИИЭТ)
#           riscv-none-elf-       (xPack)
CROSS       := riscv64-unknown-elf-
GCC_PATH    := C:/niiet/gcc/bin

# --- OpenOCD ---------------------------------------------------------------
# ВНИМАНИЕ: бинарник из C:/niiet/kit/bin НЕ содержит flash-драйвера
# 'k1921vg5t' и падает с "flash driver 'k1921vg5t' not found".
# Рабочая сборка — в отдельном репозитории openocd от НИИЭТ.
OCD_ROOT    := C:/niiet/openocd/tools
OPENOCD     := $(OCD_ROOT)/bin/openocd.exe
OCD_SCRIPTS := $(OCD_ROOT)/share/openocd/scripts

# Конфиг подключения. Лежит в SDK, не в репозитории openocd.
# Содержит: jlink + jtag + reset_config trst_only + adapter_khz 200.
OCD_SNIP    := C:/niiet/k1921vg5t_sdk/tools/openocd/openocd-snippets
OCD_CFG     := $(OCD_SNIP)/k1921vg5t/connect_jlink_jtag.cfg

# Скорость JTAG, кГц. Выше 250 этот МК не держит.
# Задаётся через -c ПОСЛЕ -f, поэтому переопределяет adapter_khz из конфига
# и убирает предупреждение DEPRECATED.
OCD_SPEED   := 200

OCD_BASE     = $(OPENOCD) -s $(OCD_SCRIPTS) -f $(OCD_CFG) -c "adapter speed $(OCD_SPEED)"

# Корень SDK относительно этого Makefile
SDK         := ../../..
BSP_NAME    := NIIET-MINI-K1921VG5T

# ============================================================================
# ИНСТРУМЕНТЫ
# ============================================================================

CC      := $(GCC_PATH)/$(CROSS)gcc
OBJCOPY := $(GCC_PATH)/$(CROSS)objcopy
OBJDUMP := $(GCC_PATH)/$(CROSS)objdump
SIZE    := $(GCC_PATH)/$(CROSS)size
GDB     := $(GCC_PATH)/$(CROSS)gdb

# ============================================================================
# ПУТИ
# ============================================================================

DEV     := $(SDK)/platform/Device/K1921VG5T
BSP     := $(SDK)/hardware/bsp/$(BSP_NAME)
RETARG  := $(SDK)/platform/retarget/Template/K1921VG5T

BUILD   := build

INCLUDES := \
  -I./app \
  -I$(DEV)/include \
  -I$(BSP) \
  -I$(RETARG)

# ============================================================================
# ИСХОДНИКИ
# ============================================================================

ASM_SRC := \
  $(DEV)/source/startup_k1921vg5t.S

C_SRC := \
  ./app/main.c \
  ./app/max7219.c \
  $(BSP)/bsp.c \
  $(DEV)/source/system_k1921vg5t.c \
  $(DEV)/source/sys_init.c \
  $(DEV)/source/mtimer.c \
  $(DEV)/source/plic.c \
  $(DEV)/source/riscv-irq.c

# Файлы retarget (printf в UART). Если сборка ругается на их отсутствие
# или на дубли символов — закомментируй строку ниже.
C_SRC += $(wildcard $(RETARG)/*.c)

OBJ := $(addprefix $(BUILD)/,$(notdir $(C_SRC:.c=.o) $(ASM_SRC:.S=.o)))
VPATH := $(sort $(dir $(C_SRC) $(ASM_SRC)))

# Файлы зависимостей: при правке .h пересоберутся только затронутые .c
DEPS := $(OBJ:.o=.d)

# ============================================================================
# ФЛАГИ
# ============================================================================

# ВАЖНО: rv32imfc_zicsr_zifencei / ilp32f
# Расширения "d" у К1921ВГ5Т НЕТ (FPU только одинарной точности).
ARCH := -march=rv32imfc_zicsr_zifencei -mabi=ilp32f

# Системные определения — из niiet_aspect.json примера run_leds
DEFS := \
  -DUSE_LIBC \
  -DSYSCLK_HSE \
  -DCKO_NONE \
  -DHSECLK_VAL=16000000 \
  -DRETARGET

OPT := -Og -g3

CFLAGS := $(ARCH) $(OPT) $(DEFS) $(INCLUDES) \
  -Wall \
  -ffunction-sections -fdata-sections \
  -fno-common \
  -MMD -MP

ASFLAGS := $(ARCH) $(OPT) $(DEFS) $(INCLUDES) -x assembler-with-cpp -MMD -MP

LDSCRIPT := $(DEV)/ldscripts/k1921vg5t_flash.ld

LDFLAGS := $(ARCH) \
  -T$(LDSCRIPT) \
  -L$(DEV)/ldscripts \
  -nostartfiles \
  -Wl,--gc-sections \
  -Wl,-Map=$(BUILD)/$(TARGET).map \
  --specs=nano.specs \
  --specs=nosys.specs

# ============================================================================
# ЦЕЛИ
# ============================================================================

.PHONY: all clean flash program erase reset gdbserver gdb size disasm info

all: $(BUILD)/$(TARGET).elf size

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	@echo CC $<
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S | $(BUILD)
	@echo AS $<
	@$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJ)
	@echo LD $@
	@$(CC) $(OBJ) $(LDFLAGS) -o $@
	@$(OBJCOPY) -O ihex $@ $(BUILD)/$(TARGET).hex
	@$(OBJCOPY) -O binary $@ $(BUILD)/$(TARGET).bin

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) $<

# Дизассемблер с исходником — смотреть, что нагенерил компилятор.
disasm: $(BUILD)/$(TARGET).elf
	@$(OBJDUMP) -d -S $< > $(BUILD)/$(TARGET).lst
	@echo $(BUILD)/$(TARGET).lst

# ----------------------------------------------------------------------------
# Работа с платой
# ----------------------------------------------------------------------------

# Собрать и прошить.
flash: $(BUILD)/$(TARGET).elf program

# Прошить то, что уже собрано, без пересборки.
program:
	$(OCD_BASE) -c "program $(BUILD)/$(TARGET).elf verify reset exit"

# Сервисное стирание. Перед запуском ПОСТАВИТЬ перемычку XP8 SERVEN,
# после — снять.
erase:
	$(OCD_BASE) -c "k1921vg5t srv_erase" -c "shutdown"

# Просто сбросить МК, ничего не трогая во flash.
reset:
	$(OCD_BASE) -c "reset run" -c "shutdown"

# Только поднять связь, без прошивки — контрольная точка отладки железа
# и сервер для F5 в VSCode (задача gdbserver в tasks.json).
# Ожидаемый вывод: TAP с id 0xDEB14001, "Listening on port 3333".
gdbserver:
	$(OCD_BASE)

gdb: $(BUILD)/$(TARGET).elf
	$(GDB) $< -ex "target extended-remote localhost:3333" \
	          -ex "set remote hardware-breakpoint-limit 2" \
	          -ex "break main"

# ----------------------------------------------------------------------------
# Диагностика: показать, чем собираем и прошиваем
# ----------------------------------------------------------------------------

info:
	@echo "TARGET   : $(TARGET)"
	@echo "CC       : $(CC)"
	@$(CC) -dumpversion
	@echo "OPENOCD  : $(OPENOCD)"
	@echo "SCRIPTS  : $(OCD_SCRIPTS)"
	@echo "CFG      : $(OCD_CFG)"
	@echo "SPEED    : $(OCD_SPEED) kHz"
	@echo "LDSCRIPT : $(LDSCRIPT)"

clean:
	@rm -rf $(BUILD)

-include $(DEPS)