#
# Makefile for Phoenix-RTOS kernel (AArch64 HAL ZynqMP)
#
# Copyright 2024 Phoenix Systems
#

OBJS += $(addprefix $(PREFIX_O)hal/aarch64/, interrupts_gicv2.o)
OBJS += $(addprefix $(PREFIX_O)hal/aarch64/zynqmp/, console.o zynqmp.o)
AARCH64_TIMER_IMPL_DEFAULT_OBJS += $(addprefix $(PREFIX_O)hal/aarch64/zynqmp/, timer.o)
