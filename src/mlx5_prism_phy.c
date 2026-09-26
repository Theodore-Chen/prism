// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: PHY diagnostics — PPCNT via ACCESS_REG (T12).
 *
 * PHY counters live behind the firmware's register window, not in BAR0,
 * so every read is an ACCESS_REG(0x805) round trip carrying one 256-byte
 * ppcnt register image (mlx5_core: en_stats.c's PPCNT groups through
 * mlx5_core_access_reg). This card is dual-PF with one port per
 * function, so local_port is always 1 (mlx5_core hardcodes it too).
 *
 * Counter groups read here:
 *   0x12 physical layer — FEC corrected/uncorrectable blocks (per FC
 *       lane + RS totals), symbol/sync-header errors, link-down events
 *   0x00 IEEE 802.3 — the ethtool -S anchors (tx/rx_packets_phy,
 *       rx_symbol_err_phy) used by the T12 cross-check
 *   0x16 statistical — phy_received_bits/phy_symbol_errors, the true
 *       pre-FEC BER numerator/denominator; PCAM-gated on CX5+ silicon,
 *       probed once here with failure expected and tolerated on CX4 Lx
 *
 * clr is never set: reads must not zero the fw counters the ethtool
 * cross-check and T13's monitor build on. The one-shot reads run only
 * under selftest=1 (R0.5 discipline); T13 adds the production monitor.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "mlx5_prism.h"
#include "mlx5_prism_fmt.h"

static int mlx5_prism_phy_ppcnt_read(struct mlx5_prism_dev *dev, u8 grp,
				     u8 *reg)
{
	memset(reg, 0, PRISM_PPCNT_REG_SIZE);
	prism_ppcnt_set_grp(reg, grp);

	/*
	 * The same buffer serves as request and response image: the wrapper
	 * copies the request into the command mailbox before the doorbell,
	 * so overwriting it with the response afterwards is safe.
	 */
	return mlx5_prism_access_reg(dev, MLX5_PRISM_REG_PPCNT, false, 0,
				     reg, reg, PRISM_PPCNT_REG_SIZE);
}

/*
 * Drain sanity (J1): an all-0xff image means the read did not really
 * happen. An all-zero image is only fatal for grp 0x12 — its
 * time_since_last_clear must be ticking — while grp 0x00 has no clock
 * field and reads legitimately all-zero on an idle, error-free port
 * (J6 covers the interpretation).
 */
static bool prism_ppcnt_all_ff(const u8 *reg)
{
	u8 hi = 0xff;
	int i;

	for (i = 0; i < PRISM_PPCNT_REG_SIZE; i++)
		hi &= reg[i];
	return hi == 0xff;
}

static void mlx5_prism_phy_dump_12(struct mlx5_prism_dev *dev, const u8 *r)
{
	u64 corr[4], unc[4], edpl[4];
	int i;

	for (i = 0; i < 4; i++) {
		edpl[i] = prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT12_EDPL_L0 + 8 * i);
		corr[i] = prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT12_FC_CORR_L0 + 8 * i);
		unc[i] = prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT12_FC_UNC_L0 + 8 * i);
	}

	pci_info(dev->pdev,
		 "PHY PPCNT(0x12): tslc=%llu sym_err=%llu sync_hdr=%llu edpl=[%llu %llu %llu %llu] fec_corr=[%llu %llu %llu %llu rs %llu] fec_unc=[%llu %llu %llu %llu rs %llu] link_down=%u\n",
		 prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT12_TSLC),
		 prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT12_SYM_ERR),
		 prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT12_SYNC_HDR),
		 edpl[0], edpl[1], edpl[2], edpl[3],
		 corr[0], corr[1], corr[2], corr[3],
		 prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT12_RS_CORR),
		 unc[0], unc[1], unc[2], unc[3],
		 prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT12_RS_UNC),
		 mlx5_prism_get_be32(r + PRISM_OFF_PPCNT12_LINK_DOWN));
}

static void mlx5_prism_phy_dump_0(struct mlx5_prism_dev *dev, const u8 *r)
{
	pci_info(dev->pdev, "PHY PPCNT(0x00): tx_ok=%llu rx_ok=%llu sym_err_carrier=%llu\n",
		 prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT0_TX_OK),
		 prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT0_RX_OK),
		 prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT0_SYM_ERR));
}

static void mlx5_prism_phy_probe_16(struct mlx5_prism_dev *dev)
{
	u8 r[PRISM_PPCNT_REG_SIZE];
	u64 bits, sym;
	int err;

	err = mlx5_prism_phy_ppcnt_read(dev, PRISM_PPCNT_GRP_PHY_STATISTICAL, r);
	if (err) {
		pci_info(dev->pdev,
			 "PHY PPCNT(0x16) probe: rejected (%d) — pre-PCAM silicon, expected\n",
			 err);
		return;
	}

	bits = prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT16_RX_BITS);
	sym = prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT16_SYM_ERR);
	if (!bits && !sym && !prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT16_CORR_BITS)) {
		pci_info(dev->pdev,
			 "PHY PPCNT(0x16) probe: accepted but all-zero — group unsupported, expected\n");
		return;
	}

	pci_info(dev->pdev,
		 "PHY PPCNT(0x16) probe: rx_bits=%llu sym_err=%llu corr_bits=%llu (pre-FEC BER derivable)\n",
		 bits, sym, prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT16_CORR_BITS));
}

int mlx5_prism_phy_selftest(struct mlx5_prism_dev *dev)
{
	u8 r[PRISM_PPCNT_REG_SIZE];
	int err;

	err = mlx5_prism_phy_ppcnt_read(dev, PRISM_PPCNT_GRP_PHY_LAYER, r);
	if (err)
		return err;
	if (prism_ppcnt_all_ff(r) ||
	    !prism_ppcnt_ctr64(r, PRISM_OFF_PPCNT12_TSLC)) {
		pci_err(dev->pdev, "PPCNT(0x12): image not alive (drain failed)\n");
		return -EIO;
	}
	mlx5_prism_phy_dump_12(dev, r);

	err = mlx5_prism_phy_ppcnt_read(dev, PRISM_PPCNT_GRP_IEEE_802_3, r);
	if (err)
		return err;
	if (prism_ppcnt_all_ff(r)) {
		pci_err(dev->pdev, "PPCNT(0x00): image not alive (drain failed)\n");
		return -EIO;
	}
	mlx5_prism_phy_dump_0(dev, r);

	mlx5_prism_phy_probe_16(dev);
	return 0;
}
