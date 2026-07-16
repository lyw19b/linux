// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae vblank hooks.
 *
 * The simulator has no independent scanout clock yet, but DRI3/Present still
 * needs a monotonically advancing vblank counter.  Use a software vblank timer
 * until the display block grows a native vblank interrupt.
 */

#include "nebulae_internal.h"

#include <drm/drm_vblank.h>

#define NEBULAE_DEFAULT_VBLANK_NS	(NSEC_PER_SEC / 60)

static enum hrtimer_restart nebulae_vblank_timer(struct hrtimer *timer)
{
	struct nebulae_device *ndev =
		container_of(timer, struct nebulae_device, vblank_timer);

	hrtimer_forward_now(timer, ndev->vblank_period);
	drm_crtc_handle_vblank(&ndev->pipe.crtc);

	return HRTIMER_RESTART;
}

int nebulae_vblank_init(struct nebulae_device *ndev)
{
	atomic64_set(&ndev->display_flips, 0);
	ndev->vblank_period = ns_to_ktime(NEBULAE_DEFAULT_VBLANK_NS);
	hrtimer_init(&ndev->vblank_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ndev->vblank_timer.function = nebulae_vblank_timer;

	return drm_vblank_init(&ndev->drm, 1);
}

void nebulae_vblank_fini(struct nebulae_device *ndev)
{
	hrtimer_cancel(&ndev->vblank_timer);
}

int nebulae_vblank_enable(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct nebulae_device *ndev = to_nebulae(crtc->dev);
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);
	u64 frame_ns;

	drm_calc_timestamping_constants(crtc, &crtc->mode);

	frame_ns = vblank->framedur_ns;
	if (!frame_ns)
		frame_ns = NEBULAE_DEFAULT_VBLANK_NS;
	ndev->vblank_period = ns_to_ktime(frame_ns);

	hrtimer_cancel(&ndev->vblank_timer);
	hrtimer_start(&ndev->vblank_timer, ndev->vblank_period,
		      HRTIMER_MODE_REL);

	return 0;
}

void nebulae_vblank_disable(struct drm_simple_display_pipe *pipe)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);

	hrtimer_cancel(&ndev->vblank_timer);
}

void nebulae_vblank_record_flip(struct nebulae_device *ndev)
{
	atomic64_inc(&ndev->display_flips);
}
