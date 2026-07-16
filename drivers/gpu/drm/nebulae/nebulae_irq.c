// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae IRQ handling.  The hard IRQ only acknowledges registers and queues
 * work; completion tears down GPUVM mappings and therefore runs in process
 * context.
 */

#include <linux/atomic.h>
#include <linux/io.h>

#include "nebulae_internal.h"

static void nebulae_irq_work(struct work_struct *work)
{
	struct nebulae_device *ndev =
		container_of(work, struct nebulae_device, irq_work);
	u32 status = atomic_xchg(&ndev->pending_irq, 0);

	if (status & (NEB_IRQ_COMPLETE | NEB_IRQ_FAULT))
		nebulae_submit_irq_process(ndev, status);
}

void nebulae_irq_init(struct nebulae_device *ndev)
{
	atomic_set(&ndev->pending_irq, 0);
	INIT_WORK(&ndev->irq_work, nebulae_irq_work);
}

void nebulae_irq_fini(struct nebulae_device *ndev)
{
	cancel_work_sync(&ndev->irq_work);
}

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

	if (status & NEB_IRQ_COMPLETE)
		atomic64_inc(&ndev->complete_irq_count);
	if (status & NEB_IRQ_FAULT) {
		atomic64_inc(&ndev->fault_irq_count);
		WRITE_ONCE(ndev->last_error,
			   readl(ndev->regs + NEB_REG_LAST_ERROR));
	}
	if ((status & NEB_IRQ_DISPLAY) ||
	    (display_status & NEB_DISPLAY_IRQ_FLIP_DONE))
		atomic64_inc(&ndev->display_irq_count);

	if (status & (NEB_IRQ_COMPLETE | NEB_IRQ_FAULT)) {
		atomic_or(status, &ndev->pending_irq);
		schedule_work(&ndev->irq_work);
	}

	return IRQ_HANDLED;
}
