/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mlx5_prism: ConnectX-4 Lx control-plane driver — shared definitions.
 *
 * Independent re-implementation of the mlx5 control plane; deliberately
 * does not include any mlx5_core headers (see CLAUDE.md constraint #2).
 */

#ifndef __MLX5_PRISM_H
#define __MLX5_PRISM_H

#include <linux/io.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define MLX5_PRISM_DRV_VERSION	"0.3"

#define MLX5_PRISM_VENDOR_ID_MELLANOX	0x15b3
#define MLX5_PRISM_DEVICE_ID_CX4_LX	0x1015

#define MLX5_PRISM_BAR0		0

/* Page and queue geometry (PRM defaults; match mlx5 MLX5_ADAPTER_PAGE_SIZE etc.) */
#define MLX5_PRISM_PAGE_SIZE		4096
#define MLX5_PRISM_MAX_COMMANDS		32
#define MLX5_PRISM_EQE_SIZE		64
#define MLX5_PRISM_EQ_ENTRIES		(MLX5_PRISM_PAGE_SIZE / MLX5_PRISM_EQE_SIZE)

struct mlx5_prism_iseg {
	__be32	fw_rev;			/* [minor hi16][major lo16] */
	__be32	cmdif_rev_fw_sub;	/* [cmdif_rev hi16][subminor lo16] */
	__be32	rsvd0[2];
	__be32	cmdq_addr_h;		/* 0x10 */
	__be32	cmdq_addr_l_sz;		/* 0x14: addr[31:12] | nic_iface | log_size | log_stride */
	__be32	cmd_dbell;		/* 0x18 */
	__be32	rsvd1[120];		/* 0x1c .. 0x1ff */
	__be32	initializing;		/* 0x200: bit0 = fw_initializing */
	/* further fields appended by later tasks as needed */
};

struct mlx5_prism_cmdq {
	void		*buf;		/* cpu virtual address */
	dma_addr_t	dma;		/* pci bus address for firmware */
	size_t		size;
	spinlock_t	lock;		/* serialise producers (used from T05) */
};

struct mlx5_prism_eq {
	void		*buf;		/* cpu virtual address */
	dma_addr_t	dma;		/* pci bus address for firmware */
	size_t		size;
	u32		cons_index;	/* consumer index (used from T06) */
};

struct mlx5_prism_dev {
	struct pci_dev			*pdev;
	void __iomem			*bar0;
	const struct mlx5_prism_iseg __iomem *iseg;

	struct mlx5_prism_cmdq		cmdq;
	struct mlx5_prism_eq		eq;
};

static inline u16 mlx5_prism_fw_rev_major(const struct mlx5_prism_dev *dev)
{
	return ioread32be(&dev->iseg->fw_rev) & 0xffff;
}

static inline u16 mlx5_prism_fw_rev_minor(const struct mlx5_prism_dev *dev)
{
	return ioread32be(&dev->iseg->fw_rev) >> 16;
}

static inline u16 mlx5_prism_fw_rev_sub(const struct mlx5_prism_dev *dev)
{
	return ioread32be(&dev->iseg->cmdif_rev_fw_sub) & 0xffff;
}

static inline u16 mlx5_prism_cmdif_rev(const struct mlx5_prism_dev *dev)
{
	return ioread32be(&dev->iseg->cmdif_rev_fw_sub) >> 16;
}

extern const struct pci_device_id mlx5_prism_pci_ids[];

int mlx5_prism_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id);
void mlx5_prism_pci_remove(struct pci_dev *pdev);

int mlx5_prism_cmdq_alloc(struct mlx5_prism_dev *dev);
void mlx5_prism_cmdq_free(struct mlx5_prism_dev *dev);

int mlx5_prism_eq_alloc(struct mlx5_prism_dev *dev);
void mlx5_prism_eq_free(struct mlx5_prism_dev *dev);

#endif /* __MLX5_PRISM_H */
