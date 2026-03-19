#
# Makefile for Phoenix-RTOS kernel (AArch64 HAL)
#
# Copyright 2018, 2020, 2021, 2024 Phoenix Systems
#

# Note: _init.o must be first in the list
OBJS += $(addprefix $(PREFIX_O)hal/aarch64/, _init.o cache.o cpu.o dtb.o hal.o exceptions.o pmap.o spinlock.o string.o _memcpy.o _memset.o)

PLATFORM_MAKEFILE := hal/aarch64/$(TARGET_SUBFAMILY)/Makefile

CFLAGS += -mcpu=$(cpu)+nofp -Ihal/aarch64
ifneq ($(wildcard $(PLATFORM_MAKEFILE)),)
  include $(PLATFORM_MAKEFILE)
  CFLAGS += -I$(dir $(PLATFORM_MAKEFILE))
endif

$(PREFIX_O)hal/aarch64/_init.o: hal/aarch64/_exceptions.S hal/aarch64/_init.S
