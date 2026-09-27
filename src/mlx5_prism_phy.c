// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: PHY diagnostics — PPCNT via ACCESS_REG (T12) + the
 * production monitor, sysfs facade and alarm state machine (T13).
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
 *       pre-FEC BER numerator/denominator; PCAM-gated on CX5+ silicon
 *       but accepted and maintained by this fw (T12 on-hardware verdict)
 *
 * clr is never set: reads must not zero the fw counters the ethtool
 * cross-check and T13's monitor build on. The one-shot reads run only
 * under selftest=1 (R0.5 discipline); the T13 monitor is the production
 * path: a per-dev delayed_work sampler on its own single-thread
 * workqueue (the ACCESS_REG reads sleep — EVENT-mode invoke — so a
 * timer_list softirq callback cannot host them; mlx5_core runs its
 * periodic sleeping tasks the same way, health.c fw-log-ts update),
 * publishing a snapshot under phy->lock that the sysfs show methods
 * copy out without sleeping. Register probed at monitor start: PAOS for
 * the link oper state (state-machine freeze input) and MTMP for the
 * temperature (attribute only visible if the fw answers).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/sysfs.h>

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

/* ---- T13: production monitor — sampler, state machine, sysfs ---- */

#define PRISM_PHY_INTERVAL_DEFAULT_MS	100
#define PRISM_PHY_INTERVAL_MIN_MS	20
#define PRISM_PHY_INTERVAL_MAX_MS	60000
#define PRISM_PHY_STREAK_K		5
#define PRISM_PHY_FAIL_STREAK_WARN	10

/*
 * BER thresholds as reciprocal denominators: num/den >= 1/w. Escalation
 * uses w, de-escalation uses 2*w (half-threshold hysteresis band), so a
 * BER sitting exactly on a threshold cannot flap the state machine.
 */
#define PRISM_BER_W_DEGRADED		10000000000ULL	/* 1e-10 */
#define PRISM_BER_W_CRITICAL		10000000ULL	/* 1e-7 */

static DEFINE_IDA(prism_phy_ida);

static const char *const prism_phy_status_names[] = {
	"ok", "degraded", "critical",
};

static int mlx5_prism_phy_paos_read(struct mlx5_prism_dev *dev, u8 *reg)
{
	memset(reg, 0, PRISM_PAOS_REG_SIZE);
	prism_paos_set_port(reg);

	return mlx5_prism_access_reg(dev, MLX5_PRISM_REG_PAOS, false, 0,
				     reg, reg, PRISM_PAOS_REG_SIZE);
}

static int mlx5_prism_phy_mtmp_read(struct mlx5_prism_dev *dev, u8 len, u8 *reg)
{
	memset(reg, 0, len);

	return mlx5_prism_access_reg(dev, MLX5_PRISM_REG_MTMP, false, 0,
				     reg, reg, len);
}

/*
 * PAOS probe: the oper status feeds the state machine's link-down
 * freeze. A rejection is tolerated (link_up stays 0 and the machine
 * evolves on BER alone) but is a quirk-profile fact — log it either way.
 */
static void mlx5_prism_phy_probe_paos(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_phy *phy = &dev->phy;
	u8 r[PRISM_PAOS_REG_SIZE];
	u32 oper = 0;
	int err;

	err = mlx5_prism_phy_paos_read(dev, r);
	phy->paos_ok = !err;
	if (!err)
		oper = prism_paos_oper_readback(r);
	pci_info(dev->pdev, "PHY monitor: PAOS probe %s (oper_status=%u)\n",
		 err ? "rejected" : "accepted", oper);
}

/*
 * MTMP probe (0x16 methodology: capability bits may lie, the register
 * image decides). The CX4-era register ends right after temperature; the
 * modern one adds thresholds and sensor names — try both lengths. The
 * temperature attribute only exists when the fw answers.
 */
static void mlx5_prism_phy_probe_mtmp(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_phy *phy = &dev->phy;
	u8 r[PRISM_MTMP_LEN_FULL];
	int err;

	phy->mtmp_len = PRISM_MTMP_LEN_MIN;
	err = mlx5_prism_phy_mtmp_read(dev, phy->mtmp_len, r);
	if (err) {
		phy->mtmp_len = PRISM_MTMP_LEN_FULL;
		err = mlx5_prism_phy_mtmp_read(dev, phy->mtmp_len, r);
	}
	phy->mtmp_ok = !err;
	if (!err) {
		phy->snap.temperature =
			prism_mtmp_to_mdeg(mlx5_prism_get_be16(r + PRISM_OFF_MTMP_TEMPERATURE));
		phy->snap.has_temp = true;
		pci_info(dev->pdev, "PHY monitor: MTMP probe accepted (len=%u, %d mdegC)\n",
			 phy->mtmp_len, phy->snap.temperature);
	} else {
		pci_info(dev->pdev, "PHY monitor: MTMP probe rejected — temperature unavailable\n");
	}
}

/* num/den >= 1/w in exact integer arithmetic (no float in the kernel). */
static bool prism_ber_ge(u64 num, u64 den, u64 w)
{
	if (!den)
		return false;
	if (num > U64_MAX / w)
		return true;
	return num * w >= den;
}

/*
 * Desired state for one sample's interval BER: escalate at the enter
 * thresholds (1e-7 / 1e-10), de-escalate below the halves (5e-8 / 5e-11),
 * hold in the gap bands. Escalation can jump ok -> critical; the caller
 * still demands K consecutive agreeing samples before committing.
 */
static u8 prism_phy_target_state(u8 cur, u64 num, u64 den)
{
	u8 tgt = cur;

	if (prism_ber_ge(num, den, PRISM_BER_W_CRITICAL)) {
		tgt = MLX5_PRISM_PHY_CRITICAL;
	} else if (prism_ber_ge(num, den, PRISM_BER_W_DEGRADED) &&
		   tgt < MLX5_PRISM_PHY_DEGRADED) {
		tgt = MLX5_PRISM_PHY_DEGRADED;
	}
	if (!prism_ber_ge(num, den, 2 * PRISM_BER_W_CRITICAL) &&
	    tgt > MLX5_PRISM_PHY_DEGRADED)
		tgt = MLX5_PRISM_PHY_DEGRADED;
	if (!prism_ber_ge(num, den, 2 * PRISM_BER_W_DEGRADED))
		tgt = MLX5_PRISM_PHY_OK;
	return tgt;
}

static void mlx5_prism_phy_sample(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_phy *phy = &dev->phy;
	struct mlx5_prism_phy_snap next, prev;
	u8 r16[PRISM_PPCNT_REG_SIZE];
	u8 r12[PRISM_PPCNT_REG_SIZE];
	u8 rpaos[PRISM_PAOS_REG_SIZE];
	u8 rmtmp[PRISM_MTMP_LEN_FULL];
	bool have_link = false, link_up = false;
	bool have_temp = false;
	bool had_prev, event;
	u64 inum = 0, iden = 0;
	u8 old;
	int temp = 0;
	int err;

	/*
	 * All sleeping ACCESS_REG round trips happen before phy->lock is
	 * taken — under the lock there is only snapshot arithmetic.
	 */
	err = mlx5_prism_phy_ppcnt_read(dev, PRISM_PPCNT_GRP_PHY_STATISTICAL, r16);
	if (!err)
		err = mlx5_prism_phy_ppcnt_read(dev, PRISM_PPCNT_GRP_PHY_LAYER, r12);
	if (err) {
		phy->fail_streak++;
		if (phy->fail_streak == PRISM_PHY_FAIL_STREAK_WARN)
			pci_warn(dev->pdev,
				 "PHY monitor: %u consecutive failed samples\n",
				 phy->fail_streak);
		return;
	}
	phy->fail_streak = 0;

	if (phy->paos_ok && !mlx5_prism_phy_paos_read(dev, rpaos)) {
		have_link = true;
		link_up = prism_paos_oper_readback(rpaos) == PRISM_PAOS_OPER_UP;
	}
	if (phy->mtmp_ok &&
	    !mlx5_prism_phy_mtmp_read(dev, phy->mtmp_len, rmtmp)) {
		u16 raw = mlx5_prism_get_be16(rmtmp + PRISM_OFF_MTMP_TEMPERATURE);

		have_temp = true;
		temp = prism_mtmp_to_mdeg(raw);
	}

	spin_lock(&phy->lock);

	prev = phy->snap;
	old = phy->status;

	next = prev;
	next.rx_bits = prism_ppcnt_ctr64(r16, PRISM_OFF_PPCNT16_RX_BITS);
	next.sym_err = prism_ppcnt_ctr64(r16, PRISM_OFF_PPCNT16_SYM_ERR);
	next.corr_bits = prism_ppcnt_ctr64(r16, PRISM_OFF_PPCNT16_CORR_BITS);
	next.fec_corr = prism_ppcnt_ctr64(r12, PRISM_OFF_PPCNT12_RS_CORR);
	next.fec_unc = prism_ppcnt_ctr64(r12, PRISM_OFF_PPCNT12_RS_UNC);
	next.link_down_events = mlx5_prism_get_be32(r12 + PRISM_OFF_PPCNT12_LINK_DOWN);
	if (have_link)
		next.link_up = link_up;
	if (have_temp) {
		next.temperature = temp;
		next.has_temp = true;
	}

	had_prev = phy->anchored &&
		   next.rx_bits >= prev.rx_bits && next.sym_err >= prev.sym_err;
	if (!had_prev) {
		/* first sample, or a counter-clock restart mid-load: re-anchor
		 * — BER windows never span a restart (DESIGN §8.1 tslc note).
		 */
		phy->anchor_rx_bits = next.rx_bits;
		phy->anchor_sym_err = next.sym_err;
		phy->anchored = true;
	}
	next.ber_num = next.sym_err - phy->anchor_sym_err;
	next.ber_den = next.rx_bits - phy->anchor_rx_bits;
	if (had_prev) {
		inum = next.sym_err - prev.sym_err;
		iden = next.rx_bits - prev.rx_bits;
	}

	next.status = old;
	if (have_link && !link_up) {
		/* Link down: BER is meaningless — sit at ok (DESIGN §8.3). */
		next.status = MLX5_PRISM_PHY_OK;
		phy->streak_cnt = 0;
	} else if (had_prev) {
		u8 tgt = prism_phy_target_state(old, inum, iden);

		if (tgt == old) {
			phy->streak_cnt = 0;
		} else if (tgt == phy->streak_tgt) {
			phy->streak_cnt++;
		} else {
			phy->streak_tgt = tgt;
			phy->streak_cnt = 1;
		}
		if (tgt != old && phy->streak_cnt >= PRISM_PHY_STREAK_K) {
			next.status = tgt;
			phy->streak_cnt = 0;
		}
	}

	event = next.status != old;
	phy->snap = next;
	phy->status = next.status;

	spin_unlock(&phy->lock);

	if (event) {
		if (next.status == MLX5_PRISM_PHY_CRITICAL) {
			pci_err(dev->pdev,
				"PHY alarm: %s -> critical (window ber %llu/%llu)\n",
				prism_phy_status_names[old],
				next.ber_num, next.ber_den);
		} else if (next.status == MLX5_PRISM_PHY_DEGRADED) {
			pci_warn(dev->pdev,
				 "PHY alarm: %s -> degraded (window ber %llu/%llu)\n",
				 prism_phy_status_names[old],
				 next.ber_num, next.ber_den);
		} else {
			pci_info(dev->pdev, "PHY status: %s -> %s\n",
				 prism_phy_status_names[old],
				 prism_phy_status_names[next.status]);
		}
	}
}

static void mlx5_prism_phy_work(struct work_struct *work)
{
	struct mlx5_prism_phy *phy =
		container_of(to_delayed_work(work), struct mlx5_prism_phy, dw);
	struct mlx5_prism_dev *dev =
		container_of(phy, struct mlx5_prism_dev, phy);
	u32 interval;

	mlx5_prism_phy_sample(dev);

	spin_lock(&phy->lock);
	interval = phy->interval_ms;
	spin_unlock(&phy->lock);

	queue_delayed_work(phy->wq, &phy->dw, msecs_to_jiffies(interval));
}

/* ---- sysfs facade: snapshot copies, no sleeping, sysfs_emit ---- */

static struct mlx5_prism_phy *mlx5_prism_phy_from_sysfs(struct device *d)
{
	struct miscdevice *m = dev_get_drvdata(d);

	return container_of(m, struct mlx5_prism_phy, misc);
}

static struct mlx5_prism_phy_snap mlx5_prism_phy_snap_of(struct device *d)
{
	struct mlx5_prism_phy *phy = mlx5_prism_phy_from_sysfs(d);
	struct mlx5_prism_phy_snap s;

	spin_lock(&phy->lock);
	s = phy->snap;
	spin_unlock(&phy->lock);
	return s;
}

/* num/den as "d.dde-X" (e.g. "2.50e-12"); 0 when the numerator is 0. */
static ssize_t prism_ber_format(char *buf, u64 num, u64 den)
{
	u64 n = num, m;
	int e = 0;

	if (!num || !den)
		return sysfs_emit(buf, "0\n");
	while (n * 10 <= den) {
		n *= 10;
		e--;
	}
	m = n > U64_MAX / 1000 ? n / (den / 1000) : n * 1000 / den;
	while (m >= 1000) {
		m /= 10;
		e++;
	}
	return sysfs_emit(buf, "%llu.%02llue%d\n", m / 100, m % 100, e - 1);
}

static ssize_t phy_symbol_errors_show(struct device *d,
				      struct device_attribute *attr, char *buf)
{
	struct mlx5_prism_phy_snap s = mlx5_prism_phy_snap_of(d);

	return sysfs_emit(buf, "%llu\n", s.sym_err);
}
static DEVICE_ATTR_RO(phy_symbol_errors);

static ssize_t phy_received_bits_show(struct device *d,
				      struct device_attribute *attr, char *buf)
{
	struct mlx5_prism_phy_snap s = mlx5_prism_phy_snap_of(d);

	return sysfs_emit(buf, "%llu\n", s.rx_bits);
}
static DEVICE_ATTR_RO(phy_received_bits);

static ssize_t phy_corrected_bits_show(struct device *d,
				       struct device_attribute *attr, char *buf)
{
	struct mlx5_prism_phy_snap s = mlx5_prism_phy_snap_of(d);

	return sysfs_emit(buf, "%llu\n", s.corr_bits);
}
static DEVICE_ATTR_RO(phy_corrected_bits);

static ssize_t fec_corrected_show(struct device *d,
				  struct device_attribute *attr, char *buf)
{
	struct mlx5_prism_phy_snap s = mlx5_prism_phy_snap_of(d);

	return sysfs_emit(buf, "%llu\n", s.fec_corr);
}
static DEVICE_ATTR_RO(fec_corrected);

static ssize_t fec_uncorrectable_show(struct device *d,
				      struct device_attribute *attr, char *buf)
{
	struct mlx5_prism_phy_snap s = mlx5_prism_phy_snap_of(d);

	return sysfs_emit(buf, "%llu\n", s.fec_unc);
}
static DEVICE_ATTR_RO(fec_uncorrectable);

static ssize_t link_down_events_show(struct device *d,
				     struct device_attribute *attr, char *buf)
{
	struct mlx5_prism_phy_snap s = mlx5_prism_phy_snap_of(d);

	return sysfs_emit(buf, "%u\n", s.link_down_events);
}
static DEVICE_ATTR_RO(link_down_events);

static ssize_t pre_fec_ber_show(struct device *d,
				struct device_attribute *attr, char *buf)
{
	struct mlx5_prism_phy_snap s = mlx5_prism_phy_snap_of(d);

	return prism_ber_format(buf, s.ber_num, s.ber_den);
}
static DEVICE_ATTR_RO(pre_fec_ber);

static ssize_t status_show(struct device *d,
			   struct device_attribute *attr, char *buf)
{
	struct mlx5_prism_phy_snap s = mlx5_prism_phy_snap_of(d);

	return sysfs_emit(buf, "%s\n", prism_phy_status_names[s.status]);
}
static DEVICE_ATTR_RO(status);

static ssize_t link_up_show(struct device *d,
			    struct device_attribute *attr, char *buf)
{
	struct mlx5_prism_phy_snap s = mlx5_prism_phy_snap_of(d);

	return sysfs_emit(buf, "%u\n", s.link_up);
}
static DEVICE_ATTR_RO(link_up);

static ssize_t temperature_show(struct device *d,
				struct device_attribute *attr, char *buf)
{
	struct mlx5_prism_phy_snap s = mlx5_prism_phy_snap_of(d);

	return sysfs_emit(buf, "%d\n", s.temperature);
}
static DEVICE_ATTR_RO(temperature);

static ssize_t monitor_interval_ms_show(struct device *d,
					struct device_attribute *attr,
					char *buf)
{
	struct mlx5_prism_phy *phy = mlx5_prism_phy_from_sysfs(d);
	u32 v;

	spin_lock(&phy->lock);
	v = phy->interval_ms;
	spin_unlock(&phy->lock);
	return sysfs_emit(buf, "%u\n", v);
}

static ssize_t monitor_interval_ms_store(struct device *d,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct mlx5_prism_phy *phy = mlx5_prism_phy_from_sysfs(d);
	unsigned long v;
	int err;

	err = kstrtoul(buf, 0, &v);
	if (err)
		return err;
	if (v < PRISM_PHY_INTERVAL_MIN_MS || v > PRISM_PHY_INTERVAL_MAX_MS)
		return -EINVAL;

	spin_lock(&phy->lock);
	phy->interval_ms = v;
	spin_unlock(&phy->lock);
	return count;
}
static DEVICE_ATTR_RW(monitor_interval_ms);

static umode_t mlx5_prism_phy_attr_visible(struct kobject *kobj,
					   struct attribute *attr, int idx)
{
	struct mlx5_prism_phy *phy =
		mlx5_prism_phy_from_sysfs(kobj_to_dev(kobj));

	if (attr == &dev_attr_temperature.attr && !phy->mtmp_ok)
		return 0;
	return attr->mode;
}

static struct attribute *mlx5_prism_phy_attrs[] = {
	&dev_attr_phy_symbol_errors.attr,
	&dev_attr_phy_received_bits.attr,
	&dev_attr_phy_corrected_bits.attr,
	&dev_attr_fec_corrected.attr,
	&dev_attr_fec_uncorrectable.attr,
	&dev_attr_link_down_events.attr,
	&dev_attr_pre_fec_ber.attr,
	&dev_attr_status.attr,
	&dev_attr_link_up.attr,
	&dev_attr_temperature.attr,
	&dev_attr_monitor_interval_ms.attr,
	NULL,
};

static const struct attribute_group mlx5_prism_phy_attr_group = {
	.attrs = mlx5_prism_phy_attrs,
	.is_visible = mlx5_prism_phy_attr_visible,
};

static const struct attribute_group *mlx5_prism_phy_attr_groups[] = {
	&mlx5_prism_phy_attr_group,
	NULL,
};

/*
 * Monitor bring-up: ida instance number -> "mlx5_prismN" misc device
 * (one per PF, recycled across load cycles), PAOS/MTMP probes, then the
 * sampler. Tear-down runs the film backwards strictly before the EQ
 * dies: cancel waits for any in-flight (sleeping) sample, so no ACCESS_REG
 * can race the teardown's command path.
 */
int mlx5_prism_phy_monitor_start(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_phy *phy = &dev->phy;
	int err;

	phy->instance = ida_alloc(&prism_phy_ida, GFP_KERNEL);
	if (phy->instance < 0)
		return phy->instance;
	snprintf(phy->name, sizeof(phy->name), KBUILD_MODNAME "%d",
		 phy->instance);
	phy->interval_ms = PRISM_PHY_INTERVAL_DEFAULT_MS;
	spin_lock_init(&phy->lock);

	mlx5_prism_phy_probe_paos(dev);
	mlx5_prism_phy_probe_mtmp(dev);

	phy->misc.minor = MISC_DYNAMIC_MINOR;
	phy->misc.name = phy->name;
	phy->misc.parent = &dev->pdev->dev;
	phy->misc.groups = mlx5_prism_phy_attr_groups;

	err = misc_register(&phy->misc);
	if (err)
		goto err_ida;

	phy->wq = create_singlethread_workqueue(phy->name);
	if (!phy->wq) {
		err = -ENOMEM;
		goto err_misc;
	}

	INIT_DELAYED_WORK(&phy->dw, mlx5_prism_phy_work);
	queue_delayed_work(phy->wq, &phy->dw, 0);

	pci_info(dev->pdev,
		 "PHY monitor up: %s, interval %ums, paos=%s, mtmp=%s\n",
		 phy->name, phy->interval_ms,
		 phy->paos_ok ? "yes" : "no",
		 phy->mtmp_ok ? "yes" : "no");
	return 0;

err_misc:
	misc_deregister(&phy->misc);
err_ida:
	ida_free(&prism_phy_ida, phy->instance);
	return err;
}

void mlx5_prism_phy_monitor_stop(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_phy *phy = &dev->phy;

	cancel_delayed_work_sync(&phy->dw);
	destroy_workqueue(phy->wq);
	misc_deregister(&phy->misc);
	ida_free(&prism_phy_ida, phy->instance);
}
