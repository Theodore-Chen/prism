// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: firmware bootstrap.
 *
 * Mirrors mlx5_core's mlx5_function_enable:
 *   SET_ISSI -> boot pages -> wait for fw_initializing to clear ->
 *   ACCESS_REG(HOST_ENDIANNESS) -> SET_HCA_CAP (known-good profile) ->
 *   init pages -> INIT_HCA (OPERATIONAL).
 *
 * Teardown reverses it. Once INIT_HCA succeeded this is the FULL teardown
 * (mlx5_core's mlx5_function_teardown): TEARDOWN_HCA -> reclaim pages ->
 * DISABLE_HCA. A bootstrap that failed before INIT_HCA gets the reduced
 * failed-load path instead (reclaim + disable only — mlx5_core's
 * reclaim_boot_pages: label).
 *
 * Field offsets below were verified against mlx5_ifc.h bit layouts on this
 * FW (14.32.1908) — see progress.md 历史问题归档 for the offset table.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "mlx5_prism.h"
#include "mlx5_prism_fmt.h"

/*
 * Interface version negotiation (ISSI). Old firmware without QUERY_ISSI
 * stays at ISSI 0, which mlx5_core also tolerates — only a transport-level
 * failure is fatal. query_issi_out is 112 B — a shorter outlen is rejected
 * with fw status 0x51 "bad output length". supported_issi_dw0 is a be32 at
 * out bytes[108:112], one bit per ISSI.
 */
/*
 * Quirk profile for the measured firmware. FW 14.32.1908 is the only
 * revision prism has run on; an unknown revision gets the same values plus
 * a warning — the profile is data, so a new fw only touches this table.
 */
void mlx5_prism_fw_profile_defaults(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_fw_profile *p = &dev->profile;

	p->caps_log_max_klm_cap = 18;
	p->caps_zero_cmdif_checksum = true;
	p->caps_clear_82_bits01 = true;
	p->caps_zero_162_163 = true;
	p->eq_engagement_retries = mlx5_prism_eq_engagement_retries;
	p->eq_poll_patience_ms = mlx5_prism_eq_patience_ms;

	if (mlx5_prism_fw_rev_major(dev) != 14 || mlx5_prism_fw_rev_minor(dev) != 32)
		pci_warn(dev->pdev,
			 "unprofiled firmware %u.%u.%u: using 14.32 quirks\n",
			 mlx5_prism_fw_rev_major(dev),
			 mlx5_prism_fw_rev_minor(dev),
			 mlx5_prism_fw_rev_sub(dev));
}

/*
 * Interface-state gate: the minimum fw_state at which each opcode is legal
 * (DESIGN.md v2 §4.1). Commands not listed ride unchecked — the firmware
 * remains the final judge (unknown opcodes must reach it to be rejected
 * there, as the self-test's 0xdead probe verifies).
 */
bool mlx5_prism_fw_cmd_allowed(struct mlx5_prism_dev *dev, u16 opcode)
{
	static const u8 min_state[] = {
		[MLX5_PRISM_CMD_OP_QUERY_HCA_CAP]	= MLX5_PRISM_FW_BOOT_PAGES,
		[MLX5_PRISM_CMD_OP_SET_HCA_CAP]		= MLX5_PRISM_FW_BOOT_PAGES,
		[MLX5_PRISM_CMD_OP_ACCESS_REG]		= MLX5_PRISM_FW_BOOT_PAGES,
		[MLX5_PRISM_CMD_OP_INIT_HCA]		= MLX5_PRISM_FW_CAPS_SET,
		[MLX5_PRISM_CMD_OP_TEARDOWN_HCA]	= MLX5_PRISM_FW_OPERATIONAL,
		[MLX5_PRISM_CMD_OP_ALLOC_UAR]		= MLX5_PRISM_FW_OPERATIONAL,
		[MLX5_PRISM_CMD_OP_DEALLOC_UAR]		= MLX5_PRISM_FW_OPERATIONAL,
		[MLX5_PRISM_CMD_OP_CREATE_EQ]		= MLX5_PRISM_FW_OPERATIONAL,
		[MLX5_PRISM_CMD_OP_DESTROY_EQ]		= MLX5_PRISM_FW_OPERATIONAL,
		[MLX5_PRISM_CMD_OP_QUERY_EQ]		= MLX5_PRISM_FW_OPERATIONAL,
		[MLX5_PRISM_CMD_OP_GEN_EQE]		= MLX5_PRISM_FW_OPERATIONAL,
		[MLX5_PRISM_CMD_OP_NOP]		= MLX5_PRISM_FW_OPERATIONAL,
	};

	if (opcode >= ARRAY_SIZE(min_state) || !min_state[opcode])
		return true;
	if (dev->fw.state >= min_state[opcode])
		return true;

	pci_err(dev->pdev,
		"opcode 0x%x needs fw_state >= %u (currently %u) — rejected driver-side\n",
		opcode, min_state[opcode], dev->fw.state);
	return false;
}

static int mlx5_prism_fw_set_issi(struct mlx5_prism_dev *dev)
{
	u8 qin[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 qout[112] = { };
	u8 sout[MLX5_PRISM_CMD_HDR_SIZE];
	u32 supported;
	int err;

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_QUERY_ISSI, 0,
				     qin, sizeof(qin), qout, sizeof(qout));
	if (err) {
		pci_info(dev->pdev, "QUERY_ISSI failed (%d): staying at ISSI 0\n",
			 err);
		return 0;
	}

	supported = mlx5_prism_get_be32(qout + PRISM_OFF_ISSI_SUPPORTED);
	pci_info(dev->pdev, "QUERY_ISSI ok: current=%u supported=0x%x\n",
		 mlx5_prism_get_be16(qout + PRISM_OFF_ISSI_CUR), supported);
	if (!(supported & BIT(1))) {
		pci_info(dev->pdev, "ISSI 1 not supported: staying at ISSI 0\n");
		return 0;
	}

	/* current_issi: be16 at inline bytes[10:12]. */
	{
		u8 sin[MLX5_PRISM_CMD_HDR_SIZE] = { };

		mlx5_prism_put_be16(sin + PRISM_OFF_ISSI_CUR, 1);
		err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_SET_ISSI, 0,
					     sin, sizeof(sin),
					     sout, sizeof(sout));
		if (err)
			return err;
	}

	pci_info(dev->pdev, "ISSI set to 1\n");
	return 0;
}

/*
 * ACCESS_REG (0x805) round trip — the generic register window mlx5_core
 * exposes as mlx5_core_access_reg (port.c): a 16-byte command header
 * (register_id be16 @bytes[10:12], argument be32 @bytes[12:16], op_mod
 * 0=write / 1=read) carrying one register image after it. in_reg holds
 * the caller-built register image (its header fields select what the fw
 * operates on); out_reg receives the image back. T12 PPCNT is the first
 * L3 consumer; host_endianness rides the same path.
 */
int mlx5_prism_access_reg(struct mlx5_prism_dev *dev, u16 reg_id, bool write,
			  u32 arg, const u8 *in_reg, u8 *out_reg, u16 reg_len)
{
	u32 in_len = MLX5_PRISM_CMD_HDR_SIZE + reg_len;
	u8 *in, *out;
	int err;

	in = kzalloc(in_len, GFP_KERNEL);
	out = kzalloc(in_len, GFP_KERNEL);
	if (!in || !out) {
		err = -ENOMEM;
		goto out;
	}

	mlx5_prism_put_be16(in + PRISM_OFF_REG_ID, reg_id);
	mlx5_prism_put_be32(in + PRISM_OFF_REG_ARG, arg);
	memcpy(in + MLX5_PRISM_CMD_HDR_SIZE, in_reg, reg_len);

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_ACCESS_REG,
				     write ? 0 : 1, in, in_len, out, in_len);
	if (!err)
		memcpy(out_reg, out + MLX5_PRISM_CMD_HDR_SIZE, reg_len);

out:
	kfree(in);
	kfree(out);
	return err;
}

/*
 * Tell the firmware the host endianness via ACCESS_REG(HOST_ENDIANNESS):
 * a 16-byte register write whose he field stays 0 for a little-endian
 * host — the whole image is zero bytes besides the register id.
 */
static int mlx5_prism_fw_set_host_endianness(struct mlx5_prism_dev *dev)
{
	u8 in[16] = { };
	u8 out[16];

	return mlx5_prism_access_reg(dev, MLX5_PRISM_REG_HOST_ENDIANNESS,
				     true, 0, in, out, sizeof(in));
}

static int mlx5_prism_fw_query_pages(struct mlx5_prism_dev *dev, u32 op_mod,
				     u32 *num_pages)
{
	struct mlx5_prism_fw *fw = &dev->fw;
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	int err;

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_QUERY_PAGES, op_mod,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		return err;

	fw->func_id = mlx5_prism_get_be32(out + PRISM_OFF_QP_FUNC_ID) & 0xffff;
	*num_pages = mlx5_prism_get_be32(out + PRISM_OFF_QP_NUM_PAGES);
	return 0;
}

/*
 * Hand `count` pages (already allocated into fw->pages[]) to the firmware
 * in a single MANAGE_PAGES — as mlx5_core does: this firmware expects the
 * full request per command (a verified single give carried 2299 init pages).
 * in layout: function_id at bytes[8:12], input_num_entries at bytes[12:16],
 * pas[i] at 16+8i.
 */
static int mlx5_prism_fw_give_pages(struct mlx5_prism_dev *dev, u32 count)
{
	struct mlx5_prism_fw *fw = &dev->fw;
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	u32 first = fw->num_pages;
	int err, i, in_len = MLX5_PRISM_CMD_HDR_SIZE + 8 * count;
	u8 *in;

	in = kzalloc(in_len, GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	mlx5_prism_put_be32(in + PRISM_OFF_MP_FUNC_ID, fw->func_id);
	mlx5_prism_put_be32(in + PRISM_OFF_MP_NUM_ENTRIES, count);
	for (i = 0; i < count; i++)
		mlx5_prism_put_be64(in + PRISM_OFF_MP_PAS + 8 * i,
				    fw->pages_dma[first + i]);

	err = mlx5_prism_cmdq_invoke_pages(dev,
					   MLX5_PRISM_CMD_OP_MANAGE_PAGES,
					   MLX5_PRISM_PAGES_GIVE,
					   in, in_len, out, sizeof(out));
	if (err)
		goto out;

	fw->num_pages = first + count;
	pci_info(dev->pdev, "MANAGE_PAGES(give %u): out entries %u\n",
		 count, mlx5_prism_get_be32(out + PRISM_OFF_MP_OUT_ENTRIES));

out:
	kfree(in);
	return err;
}

static int mlx5_prism_fw_supply_pages(struct mlx5_prism_dev *dev, u32 op_mod,
				      const char *phase)
{
	u32 requested;
	int err, i;

	err = mlx5_prism_fw_query_pages(dev, op_mod, &requested);
	if (err)
		return err;

	if (!requested) {
		pci_info(dev->pdev, "%s pages: none requested\n", phase);
		return 0;
	}
	if (requested > MLX5_PRISM_MAX_FW_PAGES - dev->fw.num_pages) {
		pci_err(dev->pdev, "%s pages: %u requested, no room (max total %d)\n",
			phase, requested, MLX5_PRISM_MAX_FW_PAGES);
		return -E2BIG;
	}

	pci_info(dev->pdev, "%s pages: firmware wants %u\n", phase, requested);
	if (!dev->fw.pages) {
		struct mlx5_prism_fw *fw = &dev->fw;

		fw->pages = kvcalloc(MLX5_PRISM_MAX_FW_PAGES,
				     sizeof(*fw->pages), GFP_KERNEL);
		fw->pages_dma = kvcalloc(MLX5_PRISM_MAX_FW_PAGES,
					 sizeof(*fw->pages_dma), GFP_KERNEL);
		if (!fw->pages || !fw->pages_dma) {
			kvfree(fw->pages);
			kvfree(fw->pages_dma);
			fw->pages = NULL;
			fw->pages_dma = NULL;
			return -ENOMEM;
		}
	}
	for (i = 0; i < requested; i++) {
		struct mlx5_prism_fw *fw = &dev->fw;
		void *p;

		p = dma_alloc_coherent(&dev->pdev->dev, MLX5_PRISM_PAGE_SIZE,
				       &fw->pages_dma[fw->num_pages + i],
				       GFP_KERNEL);
		if (!p) {
			err = -ENOMEM;
			goto err_free_batch;
		}
		fw->pages[fw->num_pages + i] = p;
	}

	err = mlx5_prism_fw_give_pages(dev, requested);
	if (err)
		goto err_free_batch;

	err = mlx5_prism_fw_query_pages(dev, op_mod, &requested);
	if (err)
		return err;
	if (requested)
		pci_warn(dev->pdev, "%s pages: firmware still wants %u\n",
			 phase, requested);

	return 0;

err_free_batch:
	for (i--; i >= 0; i--)
		dma_free_coherent(&dev->pdev->dev, MLX5_PRISM_PAGE_SIZE,
				  dev->fw.pages[dev->fw.num_pages + i],
				  dev->fw.pages_dma[dev->fw.num_pages + i]);
	return err;
}

/*
 * Boot-stage capability probe (the T06b finding: QUERY_HCA_CAP is gated
 * behind the boot-page give — status 0x4 / syndrome 0x2b69b6 before it).
 * Beyond proving the fw advanced a stage, this exercises the 8-block
 * mailbox chain that T08's SET_HCA_CAP depends on.
 */
static int mlx5_prism_fw_probe_caps(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_fw *fw = &dev->fw;
	u8 qin[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 *caps;
	int err, i, nonzero = 0;

	caps = kzalloc(MLX5_PRISM_CMD_HDR_SIZE + MLX5_PRISM_CAP_BYTES,
		       GFP_KERNEL);
	if (!caps)
		return -ENOMEM;

	/* op_mod bit0 selects max over current; bits1+ select the cap type. */
	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_QUERY_HCA_CAP, 1,
				     qin, sizeof(qin), caps,
				     MLX5_PRISM_CMD_HDR_SIZE +
				     MLX5_PRISM_CAP_BYTES);
	if (err)
		goto out;
	if (mlx5_prism_selftest)
		pci_info(dev->pdev, "caps[max]: %64ph %64ph %64ph\n",
			 caps, caps + 64, caps + 128);
	if (!fw->caps_max) {
		fw->caps_max = kzalloc(MLX5_PRISM_CAP_BYTES, GFP_KERNEL);
		fw->caps_cur = kzalloc(MLX5_PRISM_CAP_BYTES, GFP_KERNEL);
	}
	if (fw->caps_max && fw->caps_cur)
		memcpy(fw->caps_max, caps + MLX5_PRISM_CMD_HDR_SIZE,
		       MLX5_PRISM_CAP_BYTES);

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_QUERY_HCA_CAP, 0,
				     qin, sizeof(qin), caps,
				     MLX5_PRISM_CMD_HDR_SIZE +
				     MLX5_PRISM_CAP_BYTES);
	if (err)
		goto out;
	if (mlx5_prism_selftest)
		pci_info(dev->pdev, "caps[cur]: %64ph %64ph %64ph\n",
			 caps, caps + 64, caps + 128);
	if (fw->caps_max && fw->caps_cur)
		memcpy(fw->caps_cur, caps + MLX5_PRISM_CMD_HDR_SIZE,
		       MLX5_PRISM_CAP_BYTES);

	/* Drain sanity: the payload must not be all zeros. */
	for (i = MLX5_PRISM_CMD_HDR_SIZE; i < MLX5_PRISM_CMD_HDR_SIZE + 64; i++)
		nonzero |= caps[i];
	if (!nonzero) {
		pci_err(dev->pdev, "caps probe: payload drained all-zero\n");
		err = -EIO;
		goto out;
	}
	/* log_max_srq_sz / log_max_qp_sz live at payload bytes[16:18]. */
	{
		u8 *c = caps + MLX5_PRISM_CMD_HDR_SIZE;

		pci_info(dev->pdev,
			 "caps probe: max/cur ok (8-block chains), cur payload[16:18]=%02x %02x, num_ports=%u\n",
			 prism_get_bits(c, PRISM_FLD_CAPS_LOG_MAX_SRQ_SZ, 8),
			 prism_get_bits(c, PRISM_FLD_CAPS_LOG_MAX_QP_SZ, 8),
			 prism_get_bits(c, PRISM_FLD_CAPS_NUM_PORTS, 8));
	}

out:
	kfree(caps);
	return err;
}

/*
 * Capability negotiation: the fw cannot size its init working set until
 * the driver has written the capability structure once. The patch below
 * applies the known-good profile from the quirk table — writing other
 * (v5.15-ifc-named) fields leaves the fw rejecting every init-pages give.
 * Archaeology: progress.md T08; profile semantics: DESIGN.md v2 §6.2.
 */
static int mlx5_prism_fw_set_hca_cap(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_fw *fw = &dev->fw;
	u8 qin[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 sout[MLX5_PRISM_CMD_HDR_SIZE];
	u8 *caps, *c;
	int err;

	caps = kzalloc(MLX5_PRISM_CMD_HDR_SIZE + MLX5_PRISM_CAP_BYTES,
		       GFP_KERNEL);
	if (!caps)
		return -ENOMEM;
	c = caps + MLX5_PRISM_CMD_HDR_SIZE;	/* capability payload */

	/* The profile applies to the CURRENT caps (fw defaults), not max. */
	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_QUERY_HCA_CAP, 0,
				     qin, sizeof(qin), caps,
				     MLX5_PRISM_CMD_HDR_SIZE +
				     MLX5_PRISM_CAP_BYTES);
	if (err)
		goto out;

	/* Known-good profile from the quirk table — same bytes as v0.26. */
	prism_set_bits(c, PRISM_FLD_CAPS_LOG_MAX_KLM, 5,
		       dev->profile.caps_log_max_klm_cap);
	if (dev->profile.caps_zero_cmdif_checksum)
		memset(c + PRISM_OFF_CAPS_CMDIF_CHKSUM, 0, 2);
	if (dev->profile.caps_clear_82_bits01)
		c[PRISM_OFF_CAPS_UNIDENT_82] &= (u8)~0x3;
	if (dev->profile.caps_zero_162_163)
		memset(c + PRISM_OFF_CAPS_UNIDENT_162, 0, 2);

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_SET_HCA_CAP, 0,
				     caps, MLX5_PRISM_CMD_HDR_SIZE +
				     MLX5_PRISM_CAP_BYTES,
				     sout, sizeof(sout));
	if (err)
		goto out;
	dev->fw.state = MLX5_PRISM_FW_CAPS_SET;
	pci_info(dev->pdev,
		 "SET_HCA_CAP ok: profile known-good, num_ports=%u log_max_qp=%u\n",
		 prism_get_bits(c, PRISM_FLD_CAPS_NUM_PORTS, 8),
		 prism_get_bits(c, PRISM_FLD_CAPS_LOG_MAX_QP, 5));

	/* Read back: verifies the fw accepted and kept the profile. */
	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_QUERY_HCA_CAP, 0,
				     qin, sizeof(qin), caps,
				     MLX5_PRISM_CMD_HDR_SIZE +
				     MLX5_PRISM_CAP_BYTES);
	if (err)
		goto out;
	if (mlx5_prism_selftest)
		pci_info(dev->pdev, "caps[post]: %64ph %64ph %64ph\n",
			 caps, caps + 64, caps + 128);
	if (fw->caps_cur)
		memcpy(fw->caps_cur, caps + MLX5_PRISM_CMD_HDR_SIZE,
		       MLX5_PRISM_CAP_BYTES);

out:
	kfree(caps);
	return err;
}

int mlx5_prism_fw_bootstrap(struct mlx5_prism_dev *dev)
{
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	u64 t_boot, t0;
	int err;

	t_boot = ktime_get_ns();
	dev->fw.active = true;
	mlx5_prism_fw_profile_defaults(dev);

	/*
	 * ENABLE_HCA is the first command the fw accepts; the self-test
	 * already sent it (and advanced fw_state) when running, otherwise
	 * this is the production path's turn.
	 */
	if (dev->fw.state == MLX5_PRISM_FW_RESET) {
		u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
		u8 out[MLX5_PRISM_CMD_HDR_SIZE];

		err = mlx5_prism_cmdq_invoke(dev,
					     MLX5_PRISM_CMD_OP_ENABLE_HCA, 0,
					     in, sizeof(in),
					     out, sizeof(out));
		if (err)
			return err;
		pci_info(dev->pdev, "ENABLE_HCA ok\n");
		dev->fw.state = MLX5_PRISM_FW_ENABLED;
	}

	err = mlx5_prism_fw_set_issi(dev);
	if (err)
		return err;

	err = mlx5_prism_fw_supply_pages(dev, MLX5_PRISM_QUERY_PAGES_BOOT,
					 "boot");
	if (err)
		return err;
	dev->fw.state = MLX5_PRISM_FW_BOOT_PAGES;

	/*
	 * The firmware boots its boot image from the pages just handed over.
	 * Watch the init segment while that happens so we can tell "busy
	 * booting" (initializing set) from "silently unhappy".
	 */
	{
		u32 init1 = ioread32be(&dev->iseg->initializing);
		int calm;

		for (calm = 0; calm < 200; calm++) {
			u32 init2 = ioread32be(&dev->iseg->initializing);

			if (init2 != init1)
				pci_info(dev->pdev,
					 "post-boot-pages: initializing 0x%x -> 0x%x\n",
					 init1, init2);
			init1 = init2;
			if (!(init1 >> 31))
				break;
			usleep_range(10000, 20000);
		}
		if (init1 >> 31)
			pci_warn(dev->pdev,
				 "firmware still initializing after 2s: 0x%x\n",
				 init1);
		else
			pci_info(dev->pdev,
				 "post-boot-pages: initializing clear after <= %d ms\n",
				 calm * 10);
	}

	err = mlx5_prism_fw_set_host_endianness(dev);
	if (err)
		return err;
	pci_info(dev->pdev, "host endianness declared (reg 0x7004)\n");

	err = mlx5_prism_fw_probe_caps(dev);
	if (err)
		return err;

	err = mlx5_prism_fw_set_hca_cap(dev);
	if (err)
		return err;

	t0 = ktime_get_ns();
	err = mlx5_prism_fw_supply_pages(dev, MLX5_PRISM_QUERY_PAGES_INIT,
					 "init");
	if (err)
		return err;
	pci_info(dev->pdev, "init pages supplied in %llu ms\n",
		 (ktime_get_ns() - t0) / NSEC_PER_MSEC);

	t0 = ktime_get_ns();
	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_INIT_HCA, 0,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		return err;
	dev->fw.initialized = true;
	dev->fw.state = MLX5_PRISM_FW_OPERATIONAL;
	pci_info(dev->pdev, "INIT_HCA ok: HCA operational (%llu ms)\n",
		 (ktime_get_ns() - t0) / NSEC_PER_MSEC);
	pci_info(dev->pdev, "Bootstrap time: %llu ms\n",
		 (ktime_get_ns() - t_boot) / NSEC_PER_MSEC);

	return 0;
}

void mlx5_prism_fw_teardown(struct mlx5_prism_dev *dev)
{
	struct mlx5_prism_fw *fw = &dev->fw;
	u8 tin[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 tout[MLX5_PRISM_CMD_HDR_SIZE + 8 * MLX5_PRISM_PAGES_PER_CMD];
	u8 in[MLX5_PRISM_CMD_HDR_SIZE] = { };
	u8 out[MLX5_PRISM_CMD_HDR_SIZE];
	u32 remaining, batch, returned, total = 0;
	int err, i, rounds = 0;

	if (!fw->active)
		return;
	fw->active = false;

	/*
	 * With the HCA operational, stop it first: the firmware keeps DMA-ing
	 * into the init pages until TEARDOWN_HCA quiesces it. Without INIT_HCA
	 * (failed bootstrap) there is no consumer to stop — reclaim directly.
	 */
	if (fw->initialized) {
		err = mlx5_prism_cmdq_invoke(dev,
					     MLX5_PRISM_CMD_OP_TEARDOWN_HCA, 0,
					     in, sizeof(in),
					     out, sizeof(out));
		if (err)
			pci_warn(dev->pdev, "TEARDOWN_HCA failed: %d\n", err);
		fw->initialized = false;
		fw->state = MLX5_PRISM_FW_CAPS_SET;
	}

	/*
	 * Take mode: input_num_entries (bytes[12:16]) says how many the host
	 * wants back; the firmware fills the returned addresses into the out
	 * pas[] array.
	 */
	remaining = fw->num_pages;
	while (remaining && rounds++ < MLX5_PRISM_MAX_FW_PAGES) {
		batch = min_t(u32, remaining, MLX5_PRISM_PAGES_PER_CMD);
		memset(tin, 0, sizeof(tin));
		memset(tout, 0, sizeof(tout));
		mlx5_prism_put_be32(tin + PRISM_OFF_MP_FUNC_ID, fw->func_id);
		mlx5_prism_put_be32(tin + PRISM_OFF_MP_NUM_ENTRIES, batch);

		err = mlx5_prism_cmdq_invoke_pages(dev,
						   MLX5_PRISM_CMD_OP_MANAGE_PAGES,
						   MLX5_PRISM_PAGES_TAKE,
						   tin, sizeof(tin),
						   tout, sizeof(tout));
		if (err) {
			pci_warn(dev->pdev, "page reclaim failed: %d\n", err);
			break;
		}
		returned = mlx5_prism_get_be32(tout + PRISM_OFF_MP_OUT_ENTRIES);
		if (!returned)
			break;
		remaining -= min(returned, batch);
		total += min(returned, batch);
	}

	if (total != fw->num_pages)
		pci_warn(dev->pdev, "firmware returned %u/%u pages\n",
			 total, fw->num_pages);

	for (i = 0; i < fw->num_pages; i++)
		dma_free_coherent(&dev->pdev->dev, MLX5_PRISM_PAGE_SIZE,
				  fw->pages[i], fw->pages_dma[i]);
	kvfree(fw->pages);
	kvfree(fw->pages_dma);
	fw->pages = NULL;
	fw->pages_dma = NULL;

	kfree(fw->caps_max);
	kfree(fw->caps_cur);
	fw->caps_max = NULL;
	fw->caps_cur = NULL;

	err = mlx5_prism_cmdq_invoke(dev, MLX5_PRISM_CMD_OP_DISABLE_HCA, 0,
				     in, sizeof(in), out, sizeof(out));
	if (err)
		pci_warn(dev->pdev, "DISABLE_HCA failed: %d\n", err);
	else
		pci_info(dev->pdev, "teardown: %u/%u pages reclaimed, HCA disabled\n",
			 total, fw->num_pages);
	fw->num_pages = 0;
}
