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

#define MLX5_PRISM_DRV_VERSION	"0.26"

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
#define MLX5_PRISM_CMD_OP_QUERY_HCA_CAP	0x100
#define MLX5_PRISM_CMD_OP_INIT_HCA	0x102
#define MLX5_PRISM_CMD_OP_TEARDOWN_HCA	0x103
#define MLX5_PRISM_CMD_OP_ENABLE_HCA	0x104
#define MLX5_PRISM_CMD_OP_DISABLE_HCA	0x105
#define MLX5_PRISM_CMD_OP_QUERY_PAGES	0x107
#define MLX5_PRISM_CMD_OP_MANAGE_PAGES	0x108
#define MLX5_PRISM_CMD_OP_SET_HCA_CAP	0x109
#define MLX5_PRISM_CMD_OP_QUERY_ISSI	0x10a
#define MLX5_PRISM_CMD_OP_SET_ISSI	0x10b
#define MLX5_PRISM_CMD_OP_ACCESS_REG	0x805
#define MLX5_PRISM_CMD_HDR_SIZE		16
#define MLX5_PRISM_CMD_DATA_BLOCK_SIZE	512
#define MLX5_PRISM_CMD_BLOCK_STRIDE	1024	/* mlx5 dma_pool align: pow2(576) */
#define MLX5_PRISM_CMD_MAX_CHAIN		128	/* blocks per command (64 KiB payload) */
#define MLX5_PRISM_CAP_BYTES		0x1000	/* cmd_hca_cap struct size */

/* QUERY_PAGES / MANAGE_PAGES op_mod (mlx5_ifc.h MLX5_*_IN_OP_MOD_*). */
#define MLX5_PRISM_QUERY_PAGES_BOOT	1
#define MLX5_PRISM_QUERY_PAGES_INIT	2
#define MLX5_PRISM_PAGES_GIVE		1
#define MLX5_PRISM_PAGES_TAKE		2

/* ACCESS_REG register ids (PRM §21, mlx5 register enum). */
#define MLX5_PRISM_REG_HOST_ENDIANNESS	0x7004

/* EQ / UAR (PRM §8 / mlx5 eq.c). */
#define MLX5_PRISM_CMD_OP_CREATE_EQ	0x301
#define MLX5_PRISM_CMD_OP_DESTROY_EQ	0x302
#define MLX5_PRISM_CMD_OP_QUERY_EQ	0x303
#define MLX5_PRISM_CMD_OP_GEN_EQE	0x304
#define MLX5_PRISM_CMD_OP_ALLOC_UAR	0x802
#define MLX5_PRISM_CMD_OP_DEALLOC_UAR	0x803
#define MLX5_PRISM_CMD_OP_NOP		0x80d
#define MLX5_PRISM_EVENT_TYPE_CMD	0x0a
#define MLX5_PRISM_EQ_DOORBELL_OFFSET	0x40
#define MLX5_PRISM_EQ_BUF_PAGES		64	/* ladder rung D needs 64 pas */
#define MLX5_PRISM_EQE_OWNER_INIT	0x1	/* MLX5_EQE_OWNER_INIT_VAL */
#define MLX5_PRISM_IRQ_VECS		2	/* v4.19: cmd EQ lives on vec 1 */

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

/*
 * Mailbox block (PRM §4.2.2 / mlx5 struct mlx5_cmd_prot_block). Payload
 * beyond the 16-byte inline header travels in 576-byte blocks linked by
 * DMA next pointers and numbered from 0; each block's DMA address must be
 * 1 KiB aligned (mlx5_core serves them from a dma_pool with alignment
 * roundup_pow_of_two(576) — misalignment draws delivery status 0x4/0x5).
 */
struct mlx5_prism_cmd_block {
	u8	data[MLX5_PRISM_CMD_DATA_BLOCK_SIZE];
	u8	rsvd0[48];
	__be64	next;			/* dma of the following block, 0 if last */
	__be32	block_num;		/* 0-based position in the chain */
	u8	rsvd1;
	u8	token;			/* must match the descriptor token */
	u8	ctrl_sig;
	u8	sig;
};

static_assert(sizeof(struct mlx5_prism_cmd_block) == 576);

struct mlx5_prism_cmdq {
	void		*buf;		/* cpu virtual address */
	dma_addr_t	dma;		/* pci bus address for firmware */
	size_t		size;
	u8		log_sz;		/* firmware-reported queue geometry */
	u8		log_stride;
	int		max_cmds;
	u16		token_next;	/* rolling token; 0 is reserved */
	DECLARE_BITMAP(slots, MLX5_PRISM_MAX_COMMANDS);
	spinlock_t	lock;		/* serialise producers (used from T05) */
};

/*
 * Event Queue Entry (PRM §8 / mlx5 struct mlx5_eqe, verified against
 * v5.15 device.h): type at byte 1, the command-completion vector (CMDQ slot
 * bitmask) at bytes[32:36], owner at byte 63. An entry is fresh when
 * (owner & 1) == ((cons_index >> log_sz) & 1); the ring is pre-filled with
 * owner = 1 so the first lap's fresh entries carry owner = 0.
 */
struct mlx5_prism_eqe {
	u8	rsvd0;
	u8	type;			/* e.g. MLX5_PRISM_EVENT_TYPE_CMD */
	u8	rsvd1;
	u8	sub_type;
	u8	rsvd2[28];
	union {
		__be32	cmd_vector;	/* type 0x0a: bit i = CMDQ slot i done */
		u8	raw[28];
	} data;
	u8	rsvd3[2];
	u8	signature;
	u8	owner;
};

static_assert(sizeof(struct mlx5_prism_eqe) == MLX5_PRISM_EQE_SIZE);

struct mlx5_prism_eq {
	void		*buf;		/* cpu virtual address */
	dma_addr_t	dma;		/* pci bus address for firmware */
	size_t		size;
	u32		cons_index;	/* total entries consumed */
	u8		log_sz;		/* accepted ladder rung (entries = 1<<log) */
	u32		eqn;		/* assigned by CREATE_EQ */
	u32		uar;		/* assigned by ALLOC_UAR */
	void __iomem	*doorbell;	/* bar0 + uar * 4 KiB + 0x40 */
	u32		events;		/* EQEs consumed (T09: process ctx) */
	bool		created;
};

struct mlx5_prism_irq {
	int		vector[MLX5_PRISM_IRQ_VECS];	/* Linux IRQ numbers */
	int		nvecs;
	atomic_t	count;		/* handler invocation counter */
	char		name[MLX5_PRISM_IRQ_VECS][32]; /* /proc/interrupts */
};

/*
 * Working set handed to the firmware before it accepts resource commands:
 * boot pages bring the boot image up, init pages (T08) plus INIT_HCA take
 * the HCA to operational. One MANAGE_PAGES gives at most 63 pages.
 */
#define MLX5_PRISM_MAX_FW_PAGES		8192
#define MLX5_PRISM_PAGES_PER_CMD		63

struct mlx5_prism_fw {
	u32		func_id;	/* reported by QUERY_PAGES, reused in gives */
	u32		num_pages;	/* total given, reclaimed on teardown */
	bool		active;		/* bootstrap entered; teardown may talk */
	bool		initialized;	/* INIT_HCA ok; full teardown applies */
	void		*pages[MLX5_PRISM_MAX_FW_PAGES];
	dma_addr_t	pages_dma[MLX5_PRISM_MAX_FW_PAGES];
};

struct mlx5_prism_dev {
	struct pci_dev			*pdev;
	void __iomem			*bar0;
	struct mlx5_prism_iseg __iomem	*iseg;

	struct mlx5_prism_cmdq		cmdq;
	struct mlx5_prism_eq		eq;
	struct mlx5_prism_irq		irq;
	struct mlx5_prism_fw		fw;
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

/* Big-endian field accessors (the Ubuntu header pack lacks asm/unaligned.h). */
static inline u16 mlx5_prism_get_be16(const u8 *p)
{
	return (u16)((p[0] << 8) | p[1]);
}

static inline void mlx5_prism_put_be16(u8 *p, u16 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

static inline u32 mlx5_prism_get_be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static inline void mlx5_prism_put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

static inline void mlx5_prism_put_be64(u8 *p, u64 v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (u8)(v >> (56 - 8 * i));
}

extern const struct pci_device_id mlx5_prism_pci_ids[];

int mlx5_prism_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id);
void mlx5_prism_pci_remove(struct pci_dev *pdev);

int mlx5_prism_cmdq_alloc(struct mlx5_prism_dev *dev);
void mlx5_prism_cmdq_free(struct mlx5_prism_dev *dev);
int mlx5_prism_cmdq_init(struct mlx5_prism_dev *dev);

/*
 * Blocking CMDQ round trip in poll mode: register a slot under cmdq->lock,
 * ring the doorbell, poll the ownership bit, parse status/syndrome, copy the
 * inline out back. `in`/`out` are FULL command buffers — bytes[0:8] of `in`
 * are the opcode/op_mod passed separately, bytes[8:16] are caller-set inline
 * args, and bytes[16:len) travel through 1 KiB-aligned mailbox blocks
 * (single block up to 512 B of payload, up to 128 blocks chained). The
 * EQ/interrupt completion path arrives in T09/T10.
 */
int mlx5_prism_cmdq_invoke(struct mlx5_prism_dev *dev,
			   u16 opcode, u16 op_mod,
			   u8 *in, u32 in_len, u8 *out, u32 out_len);
/*
 * Same round trip on the dedicated top CMDQ slot reserved for MANAGE_PAGES
 * (mlx5_core's page_queue): page commands must never starve behind regular
 * commands, since completing those may itself require pages.
 */
int mlx5_prism_cmdq_invoke_pages(struct mlx5_prism_dev *dev,
				 u16 opcode, u16 op_mod,
				 u8 *in, u32 in_len, u8 *out, u32 out_len);
int mlx5_prism_cmdq_release_slots(struct mlx5_prism_dev *dev, u32 vector);
int mlx5_prism_cmdq_selftest(struct mlx5_prism_dev *dev);
int mlx5_prism_cmdq_mailbox_selftest(struct mlx5_prism_dev *dev);

int mlx5_prism_fw_bootstrap(struct mlx5_prism_dev *dev);
void mlx5_prism_fw_teardown(struct mlx5_prism_dev *dev);

int mlx5_prism_eq_alloc(struct mlx5_prism_dev *dev);
void mlx5_prism_eq_free(struct mlx5_prism_dev *dev);
int mlx5_prism_eq_setup(struct mlx5_prism_dev *dev);
int mlx5_prism_eq_selftest(struct mlx5_prism_dev *dev);
void mlx5_prism_eq_teardown(struct mlx5_prism_dev *dev);

int mlx5_prism_irq_alloc(struct mlx5_prism_dev *dev);
void mlx5_prism_irq_free(struct mlx5_prism_dev *dev);

#endif /* __MLX5_PRISM_H */
