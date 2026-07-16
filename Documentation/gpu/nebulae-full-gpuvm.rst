=============================
Nebulae DRM Full GPUVM record
=============================

Branch and scope
================

This implementation is maintained on ``feature/nebulae-full-gpuvm`` from
baseline ``10e89584a``.  It covers DRM-P0-01 through DRM-P2-05 in
``drivers/gpu/drm/nebulae`` and ``include/uapi/drm/nebulae_drm.h``.  Existing
LoongArch/QEMU, Accel and unrelated dirty-tree changes were preserved and are
not claimed by this record.

The Accel driver and ONNX Runtime EP are outside this implementation scope.

VM and ownership model
======================

Each ``drm_file`` owns an ASID, physical PTBR, 48-bit GPU VA allocator, VMA
list, fault FIFO and job namespace.  ASID allocation failure makes open fail;
ASID zero is never a render-client fallback.  Device pages are 16 KiB and use
the v5.7.1 three-level 12/11/11 index split independently of host PAGE_SIZE.

Global ``drm_mm`` allocates physical VRAM storage.  A per-file ``drm_mm``
allocates GPU VAs, so one BO may have different VAs in different files.  PRIME
imports require VM_BIND in the importing file.  PTE permissions encode
VALID/R/W/X/U and submit validation checks the complete range, VMA owner,
permission and explicit BO list.

Submission and lifetime
=======================

The scheduler's run_job path publishes a hardware doorbell and returns an
unsignaled hardware fence.  Completion IRQ work validates CQ cookie/sequence,
signals the fence and releases job-owned state.  Command BO, immutable shadow,
queue descriptor and completion page live until completion, abort, timeout or
reset has quiesced hardware.

The immutable shadow splits executable command/state/shader data from the
writable CQ at 16-KiB page boundaries.  The outer trusted descriptor remains a
physical address; its ring and CQ fields are VAs in the submitting file's
ASID/PTBR.  The validator accepts only the implemented CDP packet subset and
fails closed on unknown types, sizes, padding, overflow or permission errors.

Shmem is the CPU-authoritative BO backing and VRAM is a device residency copy.
Domain transitions publish CPU data and read back GPU data.  Submit operates
only on its explicit deduplicated BO set and attaches dependencies/completion
to dma-resv; it does not scan all device BOs.

Every committed scheduler job holds a reference to its ``nebulae_file`` and
pins the VMAs for its explicit BO set.  GEM handle close and file postclose do
not wait indefinitely for a device fence: they stop new pins and transfer final
VMA/page-table/ASID teardown to the last job reference.  Closing contexts stay
in the reset/restore set until that reference retires.

File close and final hardware publication form one transaction under the
device submit mutex.  After ``closing`` is published, a scheduler job either
has already become ``active_job`` and receives KILL, or observes ``closing``
before the doorbell and completes with ``-ECANCELED``.  There is no gap in
which a queued job can escape close, occupy the single hardware slot and block
the next client.

A per-context submit mutex serializes the complete entity transaction.  All
fallible allocation, dependency, reservation and user job-ID registration is
finished before ``drm_sched_job_arm()``; after arm the job is always pushed,
even if exporting a sync_file fails.

Deferred unmap is private to GEM/file teardown.  Explicit VM_UNMAP returns
``-EBUSY`` while a job owns the VMA, and MADVISE retains the usable mapping so
userspace can retry.  Neither ioctl reports success for a VA queued for hidden
asynchronous destruction.

TLB update contract
===================

PTE map, unmap, context restore/allocation and ASID release publish page-table
writes before writing ``NEB_REG_TLB_INVALIDATE`` (offset ``0x0064``).  The
current request is ``NEB_TLB_INVALIDATE_ALL``.  The QEMU device coalesces these
requests and applies them at an ordered simulator job boundary, so KMD VM
updates do not wait in the submit path and do not race an active GpuTop.

This explicit contract replaces the old v1 assumption that reconstructing the
simulator on every submit happened to clear translations.  Real hardware must
implement at least this global operation before advertising
``NEB_CAP_TLB_INVALIDATE``; future versions may add ASID/range scopes and a
completion sequence.

Executable publication independently writes ``NEB_REG_ICACHE_INVALIDATE``
after the immutable shadow bytes and before the doorbell.  The TLB and
instruction-cache operations remain separate ordered domains.

DRM-P0 through DRM-P2-05
========================

The completed areas are:

* per-file ASID/PT, VM, job ownership and fail-closed discovery;
* async drm_sched/HW fence/IRQ completion and bounded BO/shadow lifetimes;
* BO_WAIT, MADVISE, domain transitions, PRIME VM_BIND and arbitrary VA;
* R/RX/RW PTE permissions and immutable command validation;
* per-context fault FIFO, reset/quiesce/restore, coredump and wedged state;
* virtual KMS connector/vblank lifecycle;
* runtime/system PM, unplug active-operation drain, tracepoints, debugfs and
  sysfs accounting;
* ``NEB_CAP_FULL_GPUVM`` requirement, proving the device model implements the
  complete CP/GDF/CU/Texture/ROP address path.

Fault replay, real display hardware and devfreq remain capability gated.  The
current device has no fault VA/access/replay-token CSR, clock/thermal discovery
or HPD/EDID/hardware-vblank block, so the driver does not fabricate them.

Important files
===============

* ``nebulae_ctx.c``, ``nebulae_vm.c``, ``nebulae_mmu.c``: per-file ownership,
  VMA and page tables.
* ``nebulae_submit.c``, ``nebulae_irq.c``: validator, shadow, async fence/CQ.
* ``nebulae_gem.c``: shmem/VRAM domain and VM_BIND.
* ``nebulae_recovery.c``: fault, reset, PM and coredump.
* ``nebulae_device.c``, ``nebulae_drv.c``: discovery and lifecycle.
* ``nebulae_output.c``, ``nebulae_plane.c``, ``nebulae_vblank.c``: virtual
  display path.
* ``tools/testing/selftests/drivers/nebulae/nebulae_drm.c``: guest regression.

Validation
==========

The final kernel build was::

  make -j8 vmlinux

It completed successfully and produced Linux ``6.11.0+ #320``.  The selftest
was cross-built with::

  make -C tools/testing/selftests/drivers/nebulae \
    ARCH=loongarch CROSS_COMPILE=loongarch64-linux-gnu-

In the QEMU guest, ``nebulae_drm`` passed all 9 tests: render-node open, exact
discovery, command/resource BO creation, IRQ-completed NOP, BO_WAIT,
validator/fault attribution, MADVISE, bounded context close with eight queued
asynchronous jobs, and an immediate synchronous NOP from a fresh context.  The
full suite passed 20 additional consecutive rounds without a Nebulae fault,
timeout, hung/wedged state, Oops or BUG.  Three consecutive
``glxgears -> SIGINT -> vert-tex -> SIGINT`` cycles produced changing frames;
guest dmesg contained no Nebulae fault, timeout, hung/wedged state or kernel
warning.
