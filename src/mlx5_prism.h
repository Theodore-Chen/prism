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
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#define MLX5_PRISM_DRV_VERSION	"0.26"

#define MLX5_PRISM_VENDOR_ID_MELLANOX	0x15b3
#define MLX5_PRISM_DEVICE_ID_CX4_LX	0x1015

#define MLX5_PRISM_BAR0		0

/* Page and queue geometry (PRM defaults; match mlx5 MLX5_ADAPTER_PAGE_SIZE etc.) */
#define MLX5_PRISM_PAGE_SIZE		4096
#define MLX5_PRISM_MAX_COMMANDS		32
#define MLX5_PRISM_EQE_SIZE		64
#define MLX5_PRISM_EQ_ENTRIES		(MLX5_PRISM_PAGE_SIZE / MLX5_PRISM_EQE_SIZE)

/*
 * T10 hardirq consumption: walk cap per interrupt (mlx5_core's
 * MLX5_EQ_POLLING_BUDGET) and the storm-guard threshold — consecutive
 * EQE-less handler runs before the line self-disables (T06 lesson).
 */
#define MLX5_PRISM_EQ_IRQ_BUDGET		128
#define MLX5_PRISM_EQ_STORM_LIMIT	1000

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
#define MLX5_PRISM_REG_PPCNT		0x5008
#define MLX5_PRISM_REG_PAOS		0x5006
#define MLX5_PRISM_REG_MTMP		0x900a

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

/*
 * Slot-release policy, owned by the CMDQ and switched by the EQ lifecycle
 * API (mlx5_prism_cmdq_eq_bound/eq_gone). With no EQ the slot is released
 * inline right after the ownership poll; with an EQ alive the slot stays
 * BUSY until its completion EQE (vector bit) is consumed — this fw stops
 * generating completion events for slots it considers acknowledged
 * (measured: reusing slot 0 yielded exactly one EQE per EQ lifetime;
 * mlx5_core likewise frees the slot from the EQE path in events mode).
 */
enum mlx5_prism_cmdq_mode {
	MLX5_PRISM_CMDQ_POLL_INLINE,	/* no EQ: release slot inline */
	MLX5_PRISM_CMDQ_EQ_DEFERRED,	/* EQ alive: release via EQE vector */
	MLX5_PRISM_CMDQ_EVENT,		/* EQ alive + MSI-X: invoke sleeps (T10) */
};

struct mlx5_prism_cmdq {
	void		*buf;		/* cpu virtual address */
	dma_addr_t	dma;		/* pci bus address for firmware */
	size_t		size;
	u8		log_sz;		/* firmware-reported queue geometry */
	u8		log_stride;
	int		max_cmds;
	u16		token_next;	/* rolling token; 0 is reserved */
	u8		mode;		/* enum mlx5_prism_cmdq_mode */
	DECLARE_BITMAP(slots, MLX5_PRISM_MAX_COMMANDS);
	DECLARE_BITMAP(pending, MLX5_PRISM_MAX_COMMANDS); /* event-mode claim bits */
	struct completion done[MLX5_PRISM_MAX_COMMANDS];  /* event-mode waiters */
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
	atomic_t	irq_events;	/* EQEs consumed in hardirq (T10) */
	u32		empty_irqs;	/* consecutive handler runs w/o EQEs */
	bool		storm_latched;	/* storm guard tripped (irq disabled) */
	bool		event_mode;	/* eq_mode param resolved at alloc */
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
#define MLX5_PRISM_PAGES_PER_CMD	63

/*
 * Driver-side mirror of the firmware's interface state machine. The fw
 * gates commands on its own bootstrap stage (measured: QUERY_HCA_CAP before
 * the boot-page give draws status 0x4 / syndrome 0x2b69b6); this enum lets
 * the driver reject such mistakes locally instead of paying a round trip.
 * The pre-ENABLE_HCA window is deliberately NOT mirrored — bootstrap owns
 * that sequence (mlx5_core doesn't gate boot commands driver-side either).
 */
enum mlx5_prism_fw_state {
	MLX5_PRISM_FW_RESET,		/* nothing handed over yet */
	MLX5_PRISM_FW_ENABLED,		/* ENABLE_HCA accepted */
	MLX5_PRISM_FW_BOOT_PAGES,	/* boot pages given: caps/endian legal */
	MLX5_PRISM_FW_CAPS_SET,		/* SET_HCA_CAP accepted: init pages legal */
	MLX5_PRISM_FW_OPERATIONAL,	/* INIT_HCA ok: resources legal */
};

/*
 * Quirk profile — the fw's measured (not documented) behaviour, filled per
 * firmware revision at probe (see mlx5_prism_fw_profile_defaults()). mlx5_core
 * has no such layer: it only needs documented behaviour. Consumers read the
 * profile instead of re-deriving these constants. FW 14.32.1908 provenance
 * in progress.md 历史问题归档.
 */
struct mlx5_prism_fw_profile {
	/* SET_HCA_CAP known-good patch (DESIGN.md v2 §6.2). */
	u8	caps_log_max_klm_cap;	/* clamp log_max_klm_list_size to 18 */
	bool	caps_zero_cmdif_checksum;/* clear cmdif_checksum bytes 66-67 */
	bool	caps_clear_82_bits01;	/* clear byte 82 bits 0-1 (unidentified) */
	bool	caps_zero_162_163;	/* clear bytes 162-163 (unidentified) */

	/* EQ behaviour. */
	u8	eq_engagement_retries;	/* defensive recreate on silence (encoding-era quirk; unused since) */
	u32	eq_poll_patience_ms;	/* per-event commit wait (T09-era tail measured on the 1-entry ring; T11 re-measures) */
};

struct mlx5_prism_fw {
	u32		func_id;	/* reported by QUERY_PAGES, reused in gives */
	u32		num_pages;	/* total given, reclaimed on teardown */
	bool		active;		/* bootstrap entered; teardown may talk */
	bool		initialized;	/* INIT_HCA ok; full teardown applies */
	u8		state;		/* enum mlx5_prism_fw_state */
	void		**pages;	/* given pages (lazy, MAX_FW_PAGES cap) */
	dma_addr_t	*pages_dma;
	u8		*caps_max;	/* cached QUERY_HCA_CAP payloads for L3 */
	u8		*caps_cur;
};

/*
 * PHY monitor state machine states (DESIGN.md v2.3 §8.3): transitions are
 * confirmed by K consecutive samples agreeing on the target, so a single
 * noisy sample cannot flap the alarm.
 */
enum mlx5_prism_phy_status {
	MLX5_PRISM_PHY_OK,
	MLX5_PRISM_PHY_DEGRADED,
	MLX5_PRISM_PHY_CRITICAL,
};

/*
 * PHY monitor snapshot (T13): published by the sampling work under
 * phy->lock, copied out by sysfs show. Counter fields are raw fw
 * absolutes (same since-last-clear口径 as ethtool -S); ber_num/ber_den
 * are deltas against the first successful sample — the same-load-lifecycle
 * BER window (counter clocks restart per bootstrap, DESIGN §8.1), so
 * cross-load absolutes are never compared.
 */
struct mlx5_prism_phy_snap {
	u64	rx_bits;		/* grp 0x16 phy_received_bits */
	u64	sym_err;		/* grp 0x16 phy_symbol_errors */
	u64	corr_bits;		/* grp 0x16 phy_corrected_bits */
	u64	fec_corr;		/* grp 0x12 rs_fec_corrected_blocks */
	u64	fec_unc;		/* grp 0x12 rs_fec_uncorrectable_blocks */
	u32	link_down_events;	/* grp 0x12 */
	u64	ber_num;
	u64	ber_den;
	int	temperature;		/* millidegrees, valid iff has_temp */
	u8	link_up;		/* PAOS oper_status */
	u8	status;			/* enum mlx5_prism_phy_status */
	bool	has_temp;
};

/*
 * PHY monitor (T13): per-dev periodic sampler on its own single-thread
 * workqueue + the misc-device sysfs facade. The ACCESS_REG reads run in
 * process context outside phy->lock (they sleep — EVENT-mode invoke);
 * everything under the lock is snapshot publish/copy and tiny arithmetic.
 */
struct mlx5_prism_phy {
	spinlock_t			lock;	/* snapshot publish vs sysfs copy */
	struct mlx5_prism_phy_snap	snap;
	u64				anchor_rx_bits;	/* first successful sample */
	u64				anchor_sym_err;
	bool				anchored;
	u32				interval_ms;	/* sysfs RW, applied next tick */
	u8				status;		/* enum mlx5_prism_phy_status */
	u8				streak_tgt;	/* hysteresis: target state */
	u8				streak_cnt;	/* …and its consecutive count */
	u32				fail_streak;	/* consecutive failed samples */
	bool				paos_ok;	/* probe results */
	bool				mtmp_ok;
	u8				mtmp_len;
	struct delayed_work		dw;
	struct workqueue_struct		*wq;
	struct miscdevice		misc;
	int				instance;
	char				name[16];	/* "mlx5_prismN" */
};

struct mlx5_prism_dev {
	struct pci_dev			*pdev;
	void __iomem			*bar0;
	struct mlx5_prism_iseg __iomem	*iseg;

	struct mlx5_prism_cmdq		cmdq;
	struct mlx5_prism_eq		eq;
	struct mlx5_prism_irq		irq;
	struct mlx5_prism_fw		fw;
	struct mlx5_prism_fw_profile	profile;
	struct mlx5_prism_phy		phy;
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

/* Set at probe from the selftest= module param (default off): gates the
 * built-in self-tests and the verbose logs (caps hexdumps).
 */
extern bool mlx5_prism_selftest;

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
 * (single block up to 512 B of payload, up to 128 blocks chained). Slot
 * release follows cmdq->mode: inline without an EQ, deferred to the
 * completion EQE otherwise (see enum mlx5_prism_cmdq_mode).
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
/*
 * EQ→CMDQ completion interface — the only way the EQ side touches slots
 * (DESIGN.md v2 §2.1 rule 2). complete_slots() releases the slots named
 * by a command-completion EQE's vector bitmap and returns how many it
 * freed (EQ_DEFERRED mode) or woke (EVENT mode); eq_bound()/eq_gone()
 * bracket the EQ lifetime and switch the slot-release mode. eq_gone()
 * also forgets every held regular slot — pending EQEs will never arrive —
 * and force-wakes any event-mode sleepers.
 */
int mlx5_prism_cmdq_complete_slots(struct mlx5_prism_dev *dev, u32 vector);
void mlx5_prism_cmdq_eq_bound(struct mlx5_prism_dev *dev, bool event);
void mlx5_prism_cmdq_eq_gone(struct mlx5_prism_dev *dev);
int mlx5_prism_cmdq_selftest(struct mlx5_prism_dev *dev);
int mlx5_prism_cmdq_mailbox_selftest(struct mlx5_prism_dev *dev);

int mlx5_prism_fw_bootstrap(struct mlx5_prism_dev *dev);
void mlx5_prism_fw_teardown(struct mlx5_prism_dev *dev);
void mlx5_prism_fw_profile_defaults(struct mlx5_prism_dev *dev);
bool mlx5_prism_fw_cmd_allowed(struct mlx5_prism_dev *dev, u16 opcode);
/*
 * Generic ACCESS_REG window (mlx5_core's mlx5_core_access_reg): one
 * register image in, one image back; op_mod is derived from `write`.
 * The register image header (e.g. PPCNT local_port/grp) is built by the
 * caller from the fmt.h offset table.
 */
int mlx5_prism_access_reg(struct mlx5_prism_dev *dev, u16 reg_id, bool write,
			  u32 arg, const u8 *in_reg, u8 *out_reg, u16 reg_len);

int mlx5_prism_eq_alloc(struct mlx5_prism_dev *dev);
void mlx5_prism_eq_free(struct mlx5_prism_dev *dev);
int mlx5_prism_eq_setup(struct mlx5_prism_dev *dev);
int mlx5_prism_eq_selftest(struct mlx5_prism_dev *dev);
void mlx5_prism_eq_teardown(struct mlx5_prism_dev *dev);
int mlx5_prism_eq_irq_consume(struct mlx5_prism_dev *dev);

/* EQ quirk knobs (module params, defaults = measured values, see profile). */
extern u32 mlx5_prism_eq_patience_ms;
extern u32 mlx5_prism_eq_engagement_retries;

int mlx5_prism_irq_alloc(struct mlx5_prism_dev *dev);
void mlx5_prism_irq_free(struct mlx5_prism_dev *dev);

/* T12: PPCNT one-shot reads (selftest only, R0.5 discipline). */
int mlx5_prism_phy_selftest(struct mlx5_prism_dev *dev);
/*
 * T13: production monitor + sysfs — start registers the misc device and
 * launches the sampler (PAOS/MTMP probed first, their verdict recorded);
 * stop cancels the work, destroys the queue and deregisters, strictly
 * before the EQ teardown so no sample is in flight while completions die.
 */
int mlx5_prism_phy_monitor_start(struct mlx5_prism_dev *dev);
void mlx5_prism_phy_monitor_stop(struct mlx5_prism_dev *dev);

#endif /* __MLX5_PRISM_H */
