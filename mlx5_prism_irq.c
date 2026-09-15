// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: MSI-X interrupt wiring.
 *
 * T04: allocate MSI-X vector(s) and register minimal counting-stub handlers
 * (kept masked with IRQF_NO_AUTOEN because no EQ is bound yet). T09 binds
 * the EQ to vector index 1, mirroring v4.19 mlx5_core (MLX5_EQ_VEC_CMD=1 —
 * vector 0 belongs to the pages EQ), so two vectors are allocated.
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

	atomic_inc(&dev->irq.count);
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
