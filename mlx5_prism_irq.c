// SPDX-License-Identifier: GPL-2.0
/*
 * mlx5_prism: MSI-X interrupt wiring.
 *
 * T04: allocate one MSI-X vector and register a minimal handler that
 * only counts invocations. The vector is left masked (IRQF_NO_AUTOEN)
 * because the firmware has no EQ bound to it yet — T07 enables it after
 * CREATE_EQ. T06 extends the handler to actually consume EQEs.
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
	int n, irq, err;

	dev->irq.vector = -1;
	atomic_set(&dev->irq.count, 0);

	n = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
	if (n < 0) {
		pci_err(pdev, "pci_alloc_irq_vectors failed: %d\n", n);
		return n;
	}

	irq = pci_irq_vector(pdev, 0);
	if (irq < 0) {
		pci_err(pdev, "pci_irq_vector(0) failed: %d\n", irq);
		err = irq;
		goto err_free_vec;
	}

	snprintf(dev->irq.name, sizeof(dev->irq.name),
		 "mlx5_prism@%s", pci_name(pdev));

	err = request_irq(irq, mlx5_prism_irq_handler, IRQF_NO_AUTOEN,
			  dev->irq.name, dev);
	if (err) {
		pci_err(pdev, "request_irq(%d) failed: %d\n", irq, err);
		goto err_free_vec;
	}

	dev->irq.vector = irq;
	pci_info(pdev, "MSI-X enabled: 1 vector, irq=%d (masked until EQ bind)\n",
		 irq);
	return 0;

err_free_vec:
	pci_free_irq_vectors(pdev);
	return err;
}

void mlx5_prism_irq_free(struct mlx5_prism_dev *dev)
{
	if (dev->irq.vector < 0)
		return;

	free_irq(dev->irq.vector, dev);
	pci_free_irq_vectors(dev->pdev);
	dev->irq.vector = -1;
}
