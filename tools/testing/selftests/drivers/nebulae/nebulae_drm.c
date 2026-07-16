// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <drm/nebulae_drm.h>

#include "../../kselftest.h"

#define BO_SIZE	(16 * 1024)

static int create_bo(int fd, uint32_t type, uint32_t *handle, uint64_t *va)
{
	struct drm_nebulae_bo_create args = {
		.size = BO_SIZE,
		.flags = DRM_NEBULAE_BO_PLACEMENT_SHMEM | type,
	};

	if (ioctl(fd, DRM_IOCTL_NEBULAE_BO_CREATE, &args))
		return -errno;
	*handle = args.handle;
	*va = args.va;
	return 0;
}

static void *map_bo(int fd, uint32_t handle)
{
	struct drm_nebulae_bo_mmap_offset args = { .handle = handle };

	if (ioctl(fd, DRM_IOCTL_NEBULAE_BO_MMAP, &args))
		return MAP_FAILED;
	return mmap(NULL, BO_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		    args.offset);
}

static int submit_nop(int fd, uint32_t handle, int expected)
{
	struct drm_nebulae_submit_cmd_bo args = {
		.handle = handle,
		.size = 256,
		.cmd_count = 1,
		.in_fence_fd = -1,
		.out_fence_fd = -1,
		.timeout_ns = 5ULL * 1000 * 1000 * 1000,
	};
	int ret;

	ret = ioctl(fd, DRM_IOCTL_NEBULAE_SUBMIT_CMD_BO, &args);
	if (!expected)
		return ret ? -errno : args.driver_error;
	if (!ret)
		return -EBADE;
	return errno == -expected ? 0 : -errno;
}

static int submit_async_nop(int fd, uint32_t handle, int *fence_fd)
{
	struct drm_nebulae_submit_cmd_bo args = {
		.handle = handle,
		.size = 256,
		.cmd_count = 1,
		.flags = DRM_NEBULAE_SUBMIT_ASYNC |
			 DRM_NEBULAE_SUBMIT_OUT_FENCE_FD,
		.in_fence_fd = -1,
		.out_fence_fd = -1,
	};

	if (ioctl(fd, DRM_IOCTL_NEBULAE_SUBMIT_CMD_BO, &args))
		return -errno;
	if (args.driver_error)
		return args.driver_error;
	*fence_fd = args.out_fence_fd;
	return 0;
}

static int close_inflight_context(const char *node)
{
	pid_t pid;
	int status;
	int ret;
	int i;

	pid = fork();
	if (pid < 0)
		return -errno;
	if (!pid) {
		int fences[8];
		uint32_t handle;
		uint64_t va;
		uint64_t *cmd;
		int fd;

		fd = open(node, O_RDWR | O_CLOEXEC);
		if (fd < 0 || create_bo(fd, DRM_NEBULAE_BO_TYPE_COMMAND,
					     &handle, &va))
			_exit(1);
		cmd = map_bo(fd, handle);
		if (cmd == MAP_FAILED)
			_exit(1);
		memset(cmd, 0, BO_SIZE);
		for (i = 0; i < (int)(sizeof(fences) / sizeof(fences[0])); i++) {
			if (submit_async_nop(fd, handle, &fences[i]))
				_exit(1);
		}

		/* Model signal-driven client exit: BO handles and the context close
		 * while scheduler jobs and exported fences still own the submissions. */
		munmap(cmd, BO_SIZE);
		close(fd);
		for (i = 0; i < (int)(sizeof(fences) / sizeof(fences[0])); i++)
			close(fences[i]);
		_exit(0);
	}

	for (i = 0; i < 500; i++) {
		pid_t done = waitpid(pid, &status, WNOHANG);

		if (done == pid)
			break;
		if (done < 0)
			return -errno;
		usleep(10 * 1000);
	}
	if (i == 500) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		return -ETIMEDOUT;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		return -ECHILD;

	/* A bounded close is insufficient if a job crosses close-vs-run_job and
	 * keeps the single hardware slot.  A fresh context must be able to submit
	 * immediately after the closing context has gone away. */
	{
		uint32_t handle;
		uint64_t va;
		uint64_t *cmd;
		int fd;

		fd = open(node, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			return -errno;
		ret = create_bo(fd, DRM_NEBULAE_BO_TYPE_COMMAND, &handle, &va);
		if (ret) {
			close(fd);
			return ret;
		}
		cmd = map_bo(fd, handle);
		if (cmd == MAP_FAILED) {
			ret = -errno;
			close(fd);
			return ret;
		}
		memset(cmd, 0, BO_SIZE);
		ret = submit_nop(fd, handle, 0);
		munmap(cmd, BO_SIZE);
		close(fd);
	}

	return ret;
}

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	struct drm_nebulae_get_param param = {
		.param = DRM_NEBULAE_PARAM_UAPI_VERSION,
	};
	char driver_name[32] = { };
	struct drm_version version = {
		.name_len = sizeof(driver_name) - 1,
		.name = driver_name,
	};
	struct drm_nebulae_get_fault get_fault = { };
	struct drm_nebulae_bo_wait wait = { };
	struct drm_nebulae_madvise madv = {
		.madv = DRM_NEBULAE_MADV_DONTNEED,
	};
	uint32_t cmd_handle = 0, data_handle = 0;
	uint64_t cmd_va = 0, data_va = 0;
	uint64_t *cmd;
	int fd;
	int ret;

	ksft_print_header();

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		ksft_exit_skip("cannot open %s: %s\n", node, strerror(errno));
	if (ioctl(fd, DRM_IOCTL_VERSION, &version) ||
	    strcmp(driver_name, "nebulae")) {
		close(fd);
		ksft_exit_skip("%s is not a Nebulae DRM render node\n", node);
	}
	ksft_set_plan(9);
	ksft_test_result_pass("open render node\n");

	ret = ioctl(fd, DRM_IOCTL_NEBULAE_GET_PARAM, &param);
	ksft_test_result(!ret && param.value == DRM_NEBULAE_UAPI_VERSION,
			 "UAPI discovery is exact\n");

	ret = create_bo(fd, DRM_NEBULAE_BO_TYPE_COMMAND,
			&cmd_handle, &cmd_va);
	ksft_test_result(!ret && cmd_va, "create mapped command BO\n");
	if (ret) {
		ksft_test_result_skip("resource BO not run\n");
		ksft_test_result_skip("NOP submit not run\n");
		ksft_test_result_skip("BO_WAIT not run\n");
		ksft_test_result_skip("fault attribution not run\n");
		ksft_test_result_skip("MADVISE not run\n");
		goto out;
	}
	ret = create_bo(fd, DRM_NEBULAE_BO_TYPE_RESOURCE,
			&data_handle, &data_va);
	ksft_test_result(!ret && data_va, "create mapped resource BO\n");
	if (ret) {
		ksft_test_result_skip("NOP submit not run\n");
		ksft_test_result_skip("BO_WAIT not run\n");
		ksft_test_result_skip("fault attribution not run\n");
		ksft_test_result_skip("MADVISE not run\n");
		goto out;
	}

	cmd = map_bo(fd, cmd_handle);
	if (cmd == MAP_FAILED) {
		ksft_test_result_fail("map command BO: %s\n", strerror(errno));
		goto out_remaining;
	}
	memset(cmd, 0, BO_SIZE);
	ret = submit_nop(fd, cmd_handle, 0);
	ksft_test_result(!ret, "IRQ-completed synchronous NOP submit\n");

	wait.handle = cmd_handle;
	wait.timeout_ns = 1000 * 1000;
	ret = ioctl(fd, DRM_IOCTL_NEBULAE_BO_WAIT, &wait);
	ksft_test_result(!ret, "BO_WAIT observes reservation completion\n");

	/* Reserved header bits must be rejected and attributed to this file. */
	cmd[0] = 1;
	ret = submit_nop(fd, cmd_handle, -EINVAL);
	if (!ret)
		ret = ioctl(fd, DRM_IOCTL_NEBULAE_GET_FAULT, &get_fault) ?
		      -errno : 0;
	ksft_test_result(!ret &&
			 get_fault.fault.reason == DRM_NEBULAE_FAULT_ILLEGAL_PACKET,
			 "validator failure produces per-context fault record\n");

	madv.handle = data_handle;
	ret = ioctl(fd, DRM_IOCTL_NEBULAE_MADVISE, &madv);
	ksft_test_result(!ret && madv.retained <= 1,
			 "MADVISE reports real retention state\n");
	munmap(cmd, BO_SIZE);
	goto out;

out_remaining:
	ksft_test_result_skip("NOP submit not run\n");
	ksft_test_result_skip("BO_WAIT not run\n");
	ksft_test_result_skip("fault attribution not run\n");
	ksft_test_result_skip("MADVISE not run\n");
out:
	ret = close_inflight_context(node);
	ksft_test_result(!ret,
			 "close is bounded and leaves the hardware queue reusable\n");
	close(fd);
	ksft_finished();
}
