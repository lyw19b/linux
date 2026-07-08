// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae IRQ handling.
 */

#include <linux/atomic.h>
#include <linux/io.h>

#include "nebulae_internal.h"

irqreturn_t nebulae_gpu_irq(int irq, void *data)
{
	struct nebulae_device *ndev = data;
	u32 status;
	u32 display_status;

	status = readl(ndev->regs + NEB_REG_IRQ_STATUS);
	display_status = readl(ndev->regs + NEB_REG_DISPLAY_IRQ_STATUS);
	if (!status && !display_status)
		return IRQ_NONE;

	if (status)
		writel(status, ndev->regs + NEB_REG_IRQ_STATUS);
	if (display_status)
		writel(display_status, ndev->regs + NEB_REG_DISPLAY_IRQ_STATUS);

	ndev->last_irq_status = status;
	ndev->last_display_irq_status = display_status;
	atomic64_inc(&ndev->irq_count);

	if (status & NEB_IRQ_COMPLETE) {
		atomic64_inc(&ndev->complete_irq_count);
	}

	if (status & NEB_IRQ_FAULT) {
		atomic64_inc(&ndev->fault_irq_count);
		WRITE_ONCE(ndev->last_error,
			   readl(ndev->regs + NEB_REG_LAST_ERROR));
	}

	if (status & (NEB_IRQ_COMPLETE | NEB_IRQ_FAULT)) {
		atomic64_set(&ndev->completed_jobs,
			     neb_readq(ndev, NEB_REG_COMPLETED_SEQ_LO));
		wake_up_all(&ndev->submit_wait);
	}

	if ((status & NEB_IRQ_DISPLAY) ||
	    (display_status & NEB_DISPLAY_IRQ_FLIP_DONE))
		atomic64_inc(&ndev->display_irq_count);

	return IRQ_HANDLED;
}
