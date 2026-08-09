/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mlx5_prism: ConnectX-4 Lx control-plane driver — shared definitions.
 *
 * Independent re-implementation of the mlx5 control plane; deliberately
 * does not include any mlx5_core headers (see CLAUDE.md constraint #2).
 */

#ifndef __MLX5_PRISM_H
#define __MLX5_PRISM_H

#include <linux/atomic.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define MLX5_PRISM_DRV_VERSION	"0.5"

#define MLX5_PRISM_VENDOR_ID_MELLANOX	0x15b3
#define MLX5_PRISM_DEVICE_ID_CX4_LX	0x1015

#define MLX5_PRISM_BAR0		0

/* Page and queue geometry (PRM defaults; match mlx5 MLX5_ADAPTER_PAGE_SIZE etc.) */
#define MLX5_PRISM_PAGE_SIZE		4096
#define MLX5_PRISM_MAX_COMMANDS		32
#define MLX5_PRISM_EQE_SIZE		64
#define MLX5_PRISM_EQ_ENTRIES		(MLX5_PRISM_PAGE_SIZE / MLX5_PRISM_EQE_SIZE)

/* CMDQ protocol (PRM §4 / mlx5 cmd.c). */
#define MLX5_PRISM_PCI_CMD_XPORT	7
#define MLX5_PRISM_CMD_OWNER_HW		0x1
#define MLX5_PRISM_CMD_OP_ENABLE_HCA	0x104
#define MLX5_PRISM_CMD_HDR_SIZE		16

struct mlx5_prism_iseg {
	__be32	fw_rev;			/* [minor hi16][major lo16] */
	__be32	cmdif_rev_fw_sub;	/* [cmdif_rev hi16][subminor lo16] */
	__be32	rsvd0[2];
	__be32	cmdq_addr_h;		/* 0x10 */
	__be32	cmdq_addr_l_sz;		/* 0x14: addr[31:12] | nic_iface | log_size | log_stride */
	__be32	cmd_dbell;		/* 0x18 */
	__be32	rsvd1[120];		/* 0x1c .. 0x1ff */
	__be32	initializing;		/* 0x200: bit31 = fw_initializing */
	/* further fields appended by later tasks as needed */
};

/*
 * Command descriptor — one per CMDQ slot, laid out exactly as the firmware
 * expects (PRM §4.3 / mlx5 struct mlx5_cmd_layout). 64 bytes, naturally
 * aligned, no padding.
 */
struct mlx5_prism_cmd_layout {
	u8	type;			/* transport = MLX5_PRISM_PCI_CMD_XPORT */
	u8	rsvd0[3];
	__be32	inlen;			/* total input bytes (hdr + mailbox) */
	__be64	in_ptr;			/* dma of first input mailbox, 0 if inline */
	__be32	in[4];			/* inline input: opcode/op_mod + first 8B */
	__be32	out[4];			/* inline output: status/syndrome */
	__be64	out_ptr;		/* dma of first output mailbox, 0 if inline */
	__be32	outlen;			/* total output bytes */
	u8	token;
	u8	sig;			/* xor8 over the whole descriptor */
	u8	rsvd1;
	u8	status_own;		/* bit0 = OWNER_HW; bits[7:1] = status */
};

/* Coherent mailbox block holding command data beyond the 16-byte inline hdr. */
struct mlx5_prism_cmd_mailbox {
	void		*buf;
	dma_addr_t	dma;
	size_t		size;
};

struct mlx5_prism_cmdq {
	void		*buf;		/* cpu virtual address */
	dma_addr_t	dma;		/* pci bus address for firmware */
	size_t		size;
	u8		log_sz;		/* firmware-reported queue geometry */
	u8		log_stride;
	int		max_cmds;
	spinlock_t	lock;		/* serialise producers (used from T05) */
};

struct mlx5_prism_eq {
	void		*buf;		/* cpu virtual address */
	dma_addr_t	dma;		/* pci bus address for firmware */
	size_t		size;
	u32		cons_index;	/* consumer index (used from T06) */
};

struct mlx5_prism_irq {
	int		vector;		/* Linux IRQ number; -1 if not allocated */
	atomic_t	count;		/* handler invocation counter */
	char		name[32];	/* shown in /proc/interrupts */
};

struct mlx5_prism_dev {
	struct pci_dev			*pdev;
	void __iomem			*bar0;
	struct mlx5_prism_iseg __iomem	*iseg;

	struct mlx5_prism_cmdq		cmdq;
	struct mlx5_prism_eq		eq;
	struct mlx5_prism_irq		irq;
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
int mlx5_prism_cmdq_init(struct mlx5_prism_dev *dev);
void mlx5_prism_cmdq_submit(struct mlx5_prism_dev *dev, int idx,
			    u16 opcode, u16 op_mod,
			    dma_addr_t in_ptr, u32 in_len,
			    dma_addr_t out_ptr, u32 out_len);
int mlx5_prism_cmdq_selftest(struct mlx5_prism_dev *dev);

int mlx5_prism_eq_alloc(struct mlx5_prism_dev *dev);
void mlx5_prism_eq_free(struct mlx5_prism_dev *dev);

int mlx5_prism_irq_alloc(struct mlx5_prism_dev *dev);
void mlx5_prism_irq_free(struct mlx5_prism_dev *dev);

#endif /* __MLX5_PRISM_H */
