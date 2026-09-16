# SPDX-License-Identifier: GPL-2.0
#
# Invoked via the repo-root Makefile (make -C $(KDIR) M=<repo>/src).

obj-m := mlx5_prism.o

# trace/define_trace.h re-includes the trace header via a string-literal
# include; the module source dir must be on the -I path for it to resolve.
ccflags-y += -I$(src)

mlx5_prism-y := mlx5_prism_main.o mlx5_prism_pci.o \
		mlx5_prism_cmdq.o mlx5_prism_fw.o \
		mlx5_prism_eq.o mlx5_prism_irq.o \
		mlx5_prism_trace.o
