/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * UAPI for the Nebulae DRM graphics/display driver.
 *
 * This ABI is intentionally separate from nebulae_accel.h.  The accel UAPI is
 * for compute accelerator workloads; this header is for Mesa graphics and KMS.
 */

#ifndef __UAPI_NEBULAE_DRM_H__
#define __UAPI_NEBULAE_DRM_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_NEBULAE_DRIVER_MAJOR	1
#define DRM_NEBULAE_DRIVER_MINOR	0

enum drm_nebulae_param {
	DRM_NEBULAE_PARAM_ISA_MAJOR = 0,
	DRM_NEBULAE_PARAM_ISA_MINOR,
	DRM_NEBULAE_PARAM_WAVE_SIZE,
	DRM_NEBULAE_PARAM_NUM_COMPUTE_UNITS,
	DRM_NEBULAE_PARAM_VRAM_SIZE,
	DRM_NEBULAE_PARAM_MAX_SR_COUNT,
	DRM_NEBULAE_PARAM_MAX_USER_SR_COUNT,
	DRM_NEBULAE_PARAM_MAX_VR_COUNT,
	DRM_NEBULAE_PARAM_MAX_SCRATCH_BYTES_PER_WAVE,
	DRM_NEBULAE_PARAM_MAX_WGM_BYTES_PER_WORKGROUP,
	DRM_NEBULAE_PARAM_MAX_WAVES_PER_CU,
	DRM_NEBULAE_PARAM_MAX_WORKGROUP_INVOCATIONS,
	DRM_NEBULAE_PARAM_MAX_TEXTURES,
	DRM_NEBULAE_PARAM_MAX_SAMPLERS,
	DRM_NEBULAE_PARAM_MAX_IMAGES,
	DRM_NEBULAE_PARAM_MAX_UBOS,
	DRM_NEBULAE_PARAM_MAX_SSBOS,
	DRM_NEBULAE_PARAM_SUPPORTED_FEATURES,
	DRM_NEBULAE_PARAM_UAPI_VERSION,
	DRM_NEBULAE_PARAM_SUBMIT_CAPS,
};

#define DRM_NEBULAE_UAPI_VERSION			1

#define DRM_NEBULAE_SUBMIT_CAP_USERPTR_CMD_STREAM	(1ULL << 0)
#define DRM_NEBULAE_SUBMIT_CAP_CMD_BO			(1ULL << 1)
#define DRM_NEBULAE_SUBMIT_CAP_SYNCOBJ			(1ULL << 2)
#define DRM_NEBULAE_SUBMIT_CAP_FENCE_FD			(1ULL << 3)
#define DRM_NEBULAE_SUBMIT_CAP_ASYNC			(1ULL << 4)

#define DRM_NEBULAE_BO_WC				(1U << 0)
#define DRM_NEBULAE_BO_NO_AUTO_BIND			(1U << 1)
#define DRM_NEBULAE_BO_PLACEMENT_SHIFT			16
#define DRM_NEBULAE_BO_PLACEMENT_SHMEM			(1U << DRM_NEBULAE_BO_PLACEMENT_SHIFT)
#define DRM_NEBULAE_BO_PLACEMENT_DEVICE_LOCAL		(2U << DRM_NEBULAE_BO_PLACEMENT_SHIFT)
#define DRM_NEBULAE_BO_PLACEMENT_SHARED			(3U << DRM_NEBULAE_BO_PLACEMENT_SHIFT)
#define DRM_NEBULAE_BO_PLACEMENT_MASK			(3U << DRM_NEBULAE_BO_PLACEMENT_SHIFT)
#define DRM_NEBULAE_BO_TYPE_SHIFT			24
#define DRM_NEBULAE_BO_TYPE_COMMAND			(1U << DRM_NEBULAE_BO_TYPE_SHIFT)
#define DRM_NEBULAE_BO_TYPE_SHADER			(2U << DRM_NEBULAE_BO_TYPE_SHIFT)
#define DRM_NEBULAE_BO_TYPE_RESOURCE			(3U << DRM_NEBULAE_BO_TYPE_SHIFT)
#define DRM_NEBULAE_BO_TYPE_MASK			(3U << DRM_NEBULAE_BO_TYPE_SHIFT)
#define DRM_NEBULAE_BO_FLAGS				(DRM_NEBULAE_BO_WC | \
							 DRM_NEBULAE_BO_NO_AUTO_BIND | \
							 DRM_NEBULAE_BO_PLACEMENT_MASK | \
							 DRM_NEBULAE_BO_TYPE_MASK)

#define DRM_NEBULAE_SUBMIT_ASYNC			(1U << 0)
#define DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ		(1U << 1)
#define DRM_NEBULAE_SUBMIT_IN_FENCE_FD			(1U << 2)
#define DRM_NEBULAE_SUBMIT_OUT_FENCE_FD			(1U << 3)

struct drm_nebulae_get_param {
	__u32 param;
	__u32 pad;
	__u64 value;
};

struct drm_nebulae_device_info {
	__u32 isa_major;
	__u32 isa_minor;
	__u32 wave_size;
	__u32 num_compute_units;
	__u64 vram_size;
	__u32 max_sr_count;
	__u32 max_user_sr_count;
	__u32 max_vr_count;
	__u32 max_scratch_bytes_per_wave;
	__u32 max_wgm_bytes_per_workgroup;
	__u32 max_waves_per_cu;
	__u32 max_workgroup_invocations;
	__u32 max_textures;
	__u32 max_samplers;
	__u32 max_images;
	__u32 max_ubos;
	__u32 max_ssbos;
	__u32 pad;
	__u64 supported_features;
};

struct drm_nebulae_bo_create {
	__u64 size;
	__u32 flags;
	__u32 handle;
	__u64 va;
};

struct drm_nebulae_bo_mmap_offset {
	__u32 handle;
	__u32 pad;
	__u64 offset;
};

struct drm_nebulae_bo_wait {
	__u32 handle;
	__u32 pad;
	__u64 timeout_ns;
};

struct drm_nebulae_submit {
	__u64 cmds;
	__u64 size;
	__u32 flags;
	__s32 fence_fd;
	__u64 seq;
};

struct drm_nebulae_submit_cmd_bo {
	__u32 queue_id;
	__u32 flags;
	__u32 handle;
	__u32 offset;
	__u32 size;
	__u32 cmd_count;
	__u32 in_syncobj;
	__u32 out_syncobj;
	__s32 in_fence_fd;
	__s32 out_fence_fd;
	__u64 seq;
	__u64 timeout_ns;
	__u32 status;
	__s32 driver_error;
};

struct drm_nebulae_madvise {
	__u32 handle;
	__u32 madv;
	__u32 retained;
	__u32 pad;
};

#define DRM_NEBULAE_GET_PARAM		0x00
#define DRM_NEBULAE_GET_INFO		0x01
#define DRM_NEBULAE_BO_CREATE		0x02
#define DRM_NEBULAE_BO_MMAP		0x03
#define DRM_NEBULAE_BO_WAIT		0x04
#define DRM_NEBULAE_SUBMIT		0x05
#define DRM_NEBULAE_MADVISE		0x06
#define DRM_NEBULAE_SUBMIT_CMD_BO	0x07
#define DRM_NEBULAE_NUM_IOCTLS		0x08

#define DRM_IOCTL_NEBULAE_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_GET_PARAM, \
		 struct drm_nebulae_get_param)
#define DRM_IOCTL_NEBULAE_GET_INFO \
	DRM_IOR(DRM_COMMAND_BASE + DRM_NEBULAE_GET_INFO, \
		struct drm_nebulae_device_info)
#define DRM_IOCTL_NEBULAE_BO_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_BO_CREATE, \
		 struct drm_nebulae_bo_create)
#define DRM_IOCTL_NEBULAE_BO_MMAP \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_BO_MMAP, \
		 struct drm_nebulae_bo_mmap_offset)
#define DRM_IOCTL_NEBULAE_BO_WAIT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NEBULAE_BO_WAIT, \
		struct drm_nebulae_bo_wait)
#define DRM_IOCTL_NEBULAE_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_SUBMIT, \
		 struct drm_nebulae_submit)
#define DRM_IOCTL_NEBULAE_MADVISE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_MADVISE, \
		 struct drm_nebulae_madvise)
#define DRM_IOCTL_NEBULAE_SUBMIT_CMD_BO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_SUBMIT_CMD_BO, \
		 struct drm_nebulae_submit_cmd_bo)

#if defined(__cplusplus)
}
#endif

#endif /* __UAPI_NEBULAE_DRM_H__ */
