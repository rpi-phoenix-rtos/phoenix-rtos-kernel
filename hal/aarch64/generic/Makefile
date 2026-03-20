#
# Makefile for Phoenix-RTOS kernel (AArch64 HAL Generic)
#
# Copyright 2026 Phoenix Systems
#

OBJS += $(addprefix $(PREFIX_O)hal/aarch64/, interrupts_gicv2.o)
OBJS += $(addprefix $(PREFIX_O)hal/aarch64/generic/, console.o generic.o)
AARCH64_TIMER_IMPL_DEFAULT_OBJS += $(addprefix $(PREFIX_O)hal/aarch64/, gtimer_timer.o)
