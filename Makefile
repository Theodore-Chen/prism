# SPDX-License-Identifier: GPL-2.0

# Default goal — must be the first target in the file, otherwise bare
# "make" falls through to compile_commands below and never links the .ko.
all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR)

.PHONY: all

obj-m := mlx5_prism.o

mlx5_prism-y := mlx5_prism_main.o mlx5_prism_pci.o \
		mlx5_prism_cmdq.o mlx5_prism_eq.o mlx5_prism_irq.o

# Generate compile_commands.json for clangd / IDE code navigation.
# Uses the kernel's native GEN compile_commands.json target which invokes
# scripts/clang-tools/gen_compile_commands.py in the kernel build context,
# so -I./arch/... paths resolve correctly against the kernel build dir.
# Usage: make compile_commands (no extra deps needed)
compile_commands:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) \
		compile_commands.json
.PHONY: compile_commands
