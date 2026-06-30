// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae vblank hooks.
 *
 * The bring-up KMS path uses a simple display pipe with no_vblank set during
 * atomic checks.  This file is kept as the vblank ownership point for the
 * future native display interrupt path.
 */

#include "nebulae_internal.h"

int nebulae_vblank_init(struct nebulae_device *ndev)
{
	return 0;
}
