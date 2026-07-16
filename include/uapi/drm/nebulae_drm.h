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
#define DRM_NEBULAE_DRIVER_MINOR	1

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

#define DRM_NEBULAE_UAPI_VERSION			5

#define DRM_NEBULAE_SUBMIT_CAP_USERPTR_CMD_STREAM	(1ULL << 0)
#define DRM_NEBULAE_SUBMIT_CAP_CMD_BO			(1ULL << 1)
#define DRM_NEBULAE_SUBMIT_CAP_SYNCOBJ			(1ULL << 2)
#define DRM_NEBULAE_SUBMIT_CAP_FENCE_FD			(1ULL << 3)
#define DRM_NEBULAE_SUBMIT_CAP_ASYNC			(1ULL << 4)
#define DRM_NEBULAE_SUBMIT_CAP_JOB_CONTROL		(1ULL << 5)
#define DRM_NEBULAE_SUBMIT_CAP_BO_LIST			(1ULL << 6)

#define DRM_NEBULAE_BO_WC				(1U << 0)
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
							 DRM_NEBULAE_BO_PLACEMENT_MASK | \
							 DRM_NEBULAE_BO_TYPE_MASK)

#define DRM_NEBULAE_BO_DOMAIN_CPU			(1U << 0)
#define DRM_NEBULAE_BO_DOMAIN_GPU			(1U << 1)
#define DRM_NEBULAE_BO_DOMAIN_SCANOUT			(1U << 2)
#define DRM_NEBULAE_BO_DOMAIN_MASK			(DRM_NEBULAE_BO_DOMAIN_CPU | \
							 DRM_NEBULAE_BO_DOMAIN_GPU | \
							 DRM_NEBULAE_BO_DOMAIN_SCANOUT)

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
	/* Relative timeout.  Zero waits indefinitely. */
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
	__u64 bo_handles;
	__u32 bo_handle_count;
	__u32 pad;
};

struct drm_nebulae_madvise {
	__u32 handle;
	__u32 madv;
	__u32 retained;
	__u32 pad;
};

#define DRM_NEBULAE_MADV_WILLNEED	0
#define DRM_NEBULAE_MADV_DONTNEED	1

#define DRM_NEBULAE_GET_PARAM		0x00
#define DRM_NEBULAE_GET_INFO		0x01
#define DRM_NEBULAE_BO_CREATE		0x02
#define DRM_NEBULAE_BO_MMAP		0x03
#define DRM_NEBULAE_BO_WAIT		0x04
#define DRM_NEBULAE_SUBMIT		0x05
#define DRM_NEBULAE_MADVISE		0x06
#define DRM_NEBULAE_SUBMIT_CMD_BO	0x07
#define DRM_NEBULAE_BO_INFO		0x08
#define DRM_NEBULAE_BO_SET_DOMAIN	0x09
#define DRM_NEBULAE_VM_BIND		0x0a
#define DRM_NEBULAE_JOB_CONTROL		0x0b
#define DRM_NEBULAE_GET_FAULT		0x0c
#define DRM_NEBULAE_NUM_IOCTLS		0x0d

/* Query the BO metadata needed by user mode after import.  PRIME import gives
 * a per-file GEM handle; Mesa still needs the GPU VA, size, placement and
 * current coherency domain to build descriptors and make scanout decisions. */
struct drm_nebulae_bo_info {
	__u32 handle;
	__u32 flags;
	__u64 size;
	__u64 va;
	__u32 placement;
	__u32 domain;
};

/* Request a coherency domain transition for a BO.  read_domains asks the kernel
 * to make that domain current before returning; write_domain marks the domain
 * that subsequent writes will authoritatively update. */
struct drm_nebulae_bo_set_domain {
	__u32 handle;
	__u32 read_domains;
	__u32 write_domain;
	__u32 pad;
};

#define DRM_NEBULAE_VM_BIND_OP_MAP	0
#define DRM_NEBULAE_VM_BIND_OP_UNMAP	1

/* Map (or unmap) a BO into the calling client's GPU address space at the BO's
 * VA.  With sparse per-client page tables a client's own BOs are mapped on
 * create, but a PRIME-imported BO has no owning file at import time; the
 * importer must VM_BIND it before its waves can reach it. */
struct drm_nebulae_vm_bind {
	__u32 handle;
	__u32 op;	/* DRM_NEBULAE_VM_BIND_OP_* */
	/* MAP: returned per-file GPU VA. UNMAP: VA that was removed. */
	__u64 va;
	__u64 reserved[2];
};

#define DRM_NEBULAE_JOB_CONTROL_CANCEL	0
#define DRM_NEBULAE_JOB_CONTROL_KILL	1

struct drm_nebulae_job_control {
	__u64 seq;
	__u32 op;	/* DRM_NEBULAE_JOB_CONTROL_* */
	__u32 flags;
	__u32 status;
	__s32 driver_error;
};

/* Fault records are scoped to the DRM file/context that submitted the job.
 * Flags explicitly state which optional hardware evidence is valid; the v1
 * simulator has no fault-address/token CSR and therefore never sets VALID_VA
 * or REPLAYABLE. */
#define DRM_NEBULAE_FAULT_NOT_PRESENT	1
#define DRM_NEBULAE_FAULT_PERMISSION	2
#define DRM_NEBULAE_FAULT_BUS		3
#define DRM_NEBULAE_FAULT_ILLEGAL_PACKET	4
#define DRM_NEBULAE_FAULT_TIMEOUT	5
#define DRM_NEBULAE_FAULT_RESET		6
#define DRM_NEBULAE_FAULT_UNKNOWN	0xff

#define DRM_NEBULAE_FAULT_ACCESS_READ	(1U << 0)
#define DRM_NEBULAE_FAULT_ACCESS_WRITE	(1U << 1)
#define DRM_NEBULAE_FAULT_ACCESS_EXEC	(1U << 2)

#define DRM_NEBULAE_FAULT_FLAG_VALID_VA		(1U << 0)
#define DRM_NEBULAE_FAULT_FLAG_REPLAYABLE	(1U << 1)
#define DRM_NEBULAE_FAULT_FLAG_RESET_REQUIRED	(1U << 2)

struct drm_nebulae_fault {
	__u64 sequence;
	__u64 timestamp_ns;
	__u64 ctx_id;
	__u64 job_seq;
	__u64 va;
	__u64 replay_token;
	__u32 asid;
	__u32 reason;
	__u32 access;
	__u32 flags;
	__u32 hw_status;
	__s32 driver_error;
};

#define DRM_NEBULAE_GET_FAULT_PEEK	(1U << 0)

struct drm_nebulae_get_fault {
	__u32 flags;
	__u32 pad;
	struct drm_nebulae_fault fault;
};

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
#define DRM_IOCTL_NEBULAE_BO_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_BO_INFO, \
		 struct drm_nebulae_bo_info)
#define DRM_IOCTL_NEBULAE_BO_SET_DOMAIN \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_BO_SET_DOMAIN, \
		 struct drm_nebulae_bo_set_domain)
#define DRM_IOCTL_NEBULAE_VM_BIND \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_VM_BIND, \
		 struct drm_nebulae_vm_bind)
#define DRM_IOCTL_NEBULAE_JOB_CONTROL \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_JOB_CONTROL, \
		 struct drm_nebulae_job_control)
#define DRM_IOCTL_NEBULAE_GET_FAULT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_GET_FAULT, \
		 struct drm_nebulae_get_fault)

#if defined(__cplusplus)
}
#endif

#endif /* __UAPI_NEBULAE_DRM_H__ */
