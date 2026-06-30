// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae IRQ handling.
 */

#include <linux/atomic.h>
#include <linux/io.h>

#include "nebulae_internal.h"

irqreturn_t nebulae_irq(int irq, void *data)
{
	struct nebulae_device *ndev = data;
	u32 status;

	status = readl(ndev->regs + NEB_REG_IRQ_STATUS);
	if (!status)
		return IRQ_NONE;

	writel(status, ndev->regs + NEB_REG_IRQ_STATUS);
	WRITE_ONCE(ndev->last_error, readl(ndev->regs + NEB_REG_LAST_ERROR));
	atomic64_set(&ndev->completed_jobs,
		     neb_readq(ndev, NEB_REG_COMPLETED_SEQ_LO));

	return IRQ_HANDLED;
}
