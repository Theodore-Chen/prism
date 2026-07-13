# SPDX-License-Identifier: GPL-2.0

obj-m := mlx5_prism.o

mlx5_prism-y := mlx5_prism_main.o mlx5_prism_pci.o

# Generate compile_commands.json for clangd / IDE code navigation.
# Usage: make compile_commands
# Requires: sudo apt install bear
compile_commands:
	@command -v bear >/dev/null 2>&1 || { \
		echo "ERROR: bear not found. Install: sudo apt install bear"; \
		exit 1; \
	}
	bear -- $(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) modules
.PHONY: compile_commands
