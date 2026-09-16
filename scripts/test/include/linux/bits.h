/* Host-test shim: kernel linux/bits.h subset used by mlx5_prism_fmt.h. */
#ifndef __SHIM_LINUX_BITS_H
#define __SHIM_LINUX_BITS_H
#define BIT(nr) (1UL << (nr))
#define U32_MAX ((u32)~0U)
#endif
