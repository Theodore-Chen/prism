// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: MSI-X interrupt wiring.
 *
 * T04: allocate MSI-X vector(s) with IRQF_NO_AUTOEN (no EQ bound yet).
 * T09 bound the EQ to vector index 1 (v4.19 MLX5_EQ_VEC_CMD), keeping the
 * handler a counter stub while consumption stayed in eq_poll. T10 makes
 * the handler real (mlx5_eq_async_int shape): consume EQEs in hardirq,
 * wake event-mode sleepers, self-disable on an empty-IRQ storm. The
 * poll fallback (eq_mode=poll) keeps the T09 counting-stub behaviour.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/printk.h>

#include "mlx5_prism.h"

static irqreturn_t mlx5_prism_irq_handler(int irq, void *dev_id)
{
	struct mlx5_prism_dev *dev = dev_id;
	struct mlx5_prism_eq *eq;
	int eqes;

	atomic_inc(&dev->irq.count);

	/*
	 * Vector 0 carries the completion EQ (T15), vector 1 the cmd EQ.
	 * Poll fallback (T09 semantics): count only. Also covers spurious
	 * or late IRQs around the EQ lifecycles.
	 */
	eq = (irq == dev->irq.vector[0]) ? &dev->comp_eq : &dev->eq;
	if (!eq->event_mode || !READ_ONCE(eq->created) ||
	    READ_ONCE(eq->storm_latched))
		return IRQ_HANDLED;

	eqes = mlx5_prism_eq_irq_consume(dev, eq);
	if (eqes) {
		WRITE_ONCE(eq->empty_irqs, 0);
		return IRQ_HANDLED;
	}

	/*
	 * Storm guard (T06's hard lesson): one empty IRQ is normal (event
	 * aggregation races the arm doorbell), a solid stream means the
	 * line is live but the ring never looks fresh — self-disable
	 * before it starves the machine; the selftest's recreate ladder
	 * re-enables with a fresh EQ.
	 */
	if (++eq->empty_irqs > MLX5_PRISM_EQ_STORM_LIMIT) {
		WRITE_ONCE(eq->storm_latched, true);
		disable_irq_nosync(irq);
		dev_warn_ratelimited(&dev->pdev->dev,
				     "EQ irq storm: %u+ empty interrupts, line self-disabled\n",
				     MLX5_PRISM_EQ_STORM_LIMIT);
	}
	return IRQ_HANDLED;
}

int mlx5_prism_irq_alloc(struct mlx5_prism_dev *dev)
{
	struct pci_dev *pdev = dev->pdev;
	int n, irq, err, i;

	dev->irq.nvecs = 0;
	atomic_set(&dev->irq.count, 0);

	n = pci_alloc_irq_vectors(pdev, MLX5_PRISM_IRQ_VECS,
				  MLX5_PRISM_IRQ_VECS, PCI_IRQ_MSIX);
	if (n < 0) {
		pci_err(pdev, "pci_alloc_irq_vectors failed: %d\n", n);
		return n;
	}

	for (i = 0; i < n; i++) {
		irq = pci_irq_vector(pdev, i);
		if (irq < 0) {
			err = irq;
			pci_err(pdev, "pci_irq_vector(%d) failed: %d\n",
				i, irq);
			goto err_free_vec;
		}

		snprintf(dev->irq.name[i], sizeof(dev->irq.name[i]),
			 "mlx5_prism@%s+%d", pci_name(pdev), i);

		err = request_irq(irq, mlx5_prism_irq_handler,
				  IRQF_NO_AUTOEN, dev->irq.name[i], dev);
		if (err) {
			pci_err(pdev, "request_irq(%d) failed: %d\n", irq, err);
			goto err_free_vec;
		}
		dev->irq.vector[i] = irq;
	}

	dev->irq.nvecs = n;
	pci_info(pdev, "MSI-X enabled: %d vectors (masked until EQ bind)\n", n);
	return 0;

err_free_vec:
	for (i--; i >= 0; i--)
		free_irq(dev->irq.vector[i], dev);
	pci_free_irq_vectors(pdev);
	return err;
}

void mlx5_prism_irq_free(struct mlx5_prism_dev *dev)
{
	int i;

	for (i = 0; i < dev->irq.nvecs; i++)
		free_irq(dev->irq.vector[i], dev);
	if (dev->irq.nvecs) {
		pci_free_irq_vectors(dev->pdev);
		dev->irq.nvecs = 0;
	}
}
