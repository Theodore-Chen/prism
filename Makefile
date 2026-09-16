# SPDX-License-Identifier: GPL-2.0
#
# Thin forwarder — the kernel-build incantation lives HERE and only here.
# Everything else (docs, scripts) just says "make" at the repo root; the
# module itself is built from src/ (see src/Makefile for the obj-m list).

KDIR ?= /lib/modules/$(shell uname -r)/build

# Default to the kernel's own compiler: the interactive toolchain env
# (~/.local/env-toolchain.sh) prepends a newer local gcc over /usr/bin,
# and a mixed-compiler tree is the real hazard (the .cmd incremental
# cache does not notice a compiler switch behind an unchanged "gcc").
# `make CC=...` on the command line still overrides for experiments.
ifeq ($(origin CC),command line)
KCC := $(CC)
else
KCC := /usr/bin/gcc-11
endif

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/src CC="$(KCC)"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/src CC="$(KCC)" clean

# Generate compile_commands.json for clangd / IDE code navigation.
# Uses the kernel's native GEN compile_commands.json target which invokes
# scripts/clang-tools/gen_compile_commands.py in the kernel build context,
# so -I./arch/... paths resolve correctly against the kernel build dir.
# Usage: make compile_commands (no extra deps needed)
compile_commands:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/src CC="$(KCC)" compile_commands.json

.PHONY: all clean compile_commands
