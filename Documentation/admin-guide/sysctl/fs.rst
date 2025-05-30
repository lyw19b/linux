===============================
Documentation for /proc/sys/fs/
===============================

Copyright (c) 1998, 1999,  Rik van Riel <riel@nl.linux.org>

Copyright (c) 2009,        Shen Feng<shen@cn.fujitsu.com>

For general info and legal blurb, please look in intro.rst.

------------------------------------------------------------------------------

This file contains documentation for the sysctl files and directories
in ``/proc/sys/fs/``.

The files in this directory can be used to tune and monitor
miscellaneous and general things in the operation of the Linux
kernel. Since some of the files *can* be used to screw up your
system, it is advisable to read both documentation and source
before actually making adjustments.

1. /proc/sys/fs
===============

Currently, these files might (depending on your configuration)
show up in ``/proc/sys/fs``:

.. contents:: :local:


aio-nr & aio-max-nr
-------------------

``aio-nr`` shows the current system-wide number of asynchronous io
requests.  ``aio-max-nr`` allows you to change the maximum value
``aio-nr`` can grow to.  If ``aio-nr`` reaches ``aio-nr-max`` then
``io_setup`` will fail with ``EAGAIN``.  Note that raising
``aio-max-nr`` does not result in the
pre-allocation or re-sizing of any kernel data structures.

dentry-negative
----------------------------

Policy for negative dentries. Set to 1 to always delete the dentry when a
file is removed, and 0 to disable it. By default, this behavior is disabled.

dentry-state
------------

This file shows the values in ``struct dentry_stat_t``, as defined in
``fs/dcache.c``::

  struct dentry_stat_t dentry_stat {
        long nr_dentry;
        long nr_unused;
        long age_limit;         /* age in seconds */
        long want_pages;        /* pages requested by system */
        long nr_negative;       /* # of unused negative dentries */
        long dummy;             /* Reserved for future use */
  };

Dentries are dynamically allocated and deallocated.

``nr_dentry`` shows the total number of dentries allocated (active
+ unused). ``nr_unused shows`` the number of dentries that are not
actively used, but are saved in the LRU list for future reuse.

``age_limit`` is the age in seconds after which dcache entries
can be reclaimed when memory is short and ``want_pages`` is
nonzero when ``shrink_dcache_pages()`` has been called and the
dcache isn't pruned yet.

``nr_negative`` shows the number of unused dentries that are also
negative dentries which do not map to any files. Instead,
they help speeding up rejection of non-existing files provided
by the users.


file-max & file-nr
------------------

The value in ``file-max`` denotes the maximum number of file-
handles that the Linux kernel will allocate. When you get lots
of error messages about running out of file handles, you might
want to increase this limit.

Historically,the kernel was able to allocate file handles
dynamically, but not to free them again. The three values in
``file-nr`` denote the number of allocated file handles, the number
of allocated but unused file handles, and the maximum number of
file handles. Linux 2.6 and later always reports 0 as the number of free
file handles -- this is not an error, it just means that the
number of allocated file handles exactly matches the number of
used file handles.

Attempts to allocate more file descriptors than ``file-max`` are
reported with ``printk``, look for::

  VFS: file-max limit <number> reached

in the kernel logs.


inode-nr & inode-state
----------------------

As with file handles, the kernel allocates the inode structures
dynamically, but can't free them yet.

The file ``inode-nr`` contains the first two items from
``inode-state``, so we'll skip to that file...

``inode-state`` contains three actual numbers and four dummies.
The actual numbers are, in order of appearance, ``nr_inodes``,
``nr_free_inodes`` and ``preshrink``.

``nr_inodes`` stands for the number of inodes the system has
allocated.

``nr_free_inodes`` represents the number of free inodes (?) and
preshrink is nonzero when the
system needs to prune the inode list instead of allocating
more.


mount-max
---------

This denotes the maximum number of mounts that may exist
in a mount namespace.


nr_open
-------

This denotes the maximum number of file-handles a process can
allocate. Default value is 1024*1024 (1048576) which should be
enough for most machines. Actual limit depends on ``RLIMIT_NOFILE``
resource limit.


overflowgid & overflowuid
-------------------------

Some filesystems only support 16-bit UIDs and GIDs, although in Linux
UIDs and GIDs are 32 bits. When one of these filesystems is mounted
with writes enabled, any UID or GID that would exceed 65535 is translated
to a fixed value before being written to disk.

These sysctls allow you to change the value of the fixed UID and GID.
The default is 65534.


pipe-user-pages-hard
--------------------

Maximum total number of pages a non-privileged user may allocate for pipes.
Once this limit is reached, no new pipes may be allocated until usage goes
below the limit again. When set to 0, no limit is applied, which is the default
setting.


pipe-user-pages-soft
--------------------

Maximum total number of pages a non-privileged user may allocate for pipes
before the pipe size gets limited to a single page. Once this limit is reached,
new pipes will be limited to a single page in size for this user in order to
limit total memory usage, and trying to increase them using ``fcntl()`` will be
denied until usage goes below the limit again. The default value allows to
allocate up to 1024 pipes at their default size. When set to 0, no limit is
applied.


protected_fifos
---------------

The intent of this protection is to avoid unintentional writes to
an attacker-controlled FIFO, where a program expected to create a regular
file.

When set to "0", writing to FIFOs is unrestricted.

When set to "1" don't allow ``O_CREAT`` open on FIFOs that we don't own
in world writable sticky directories, unless they are owned by the
owner of the directory.

When set to "2" it also applies to group writable sticky directories.

This protection is based on the restrictions in Openwall.


protected_hardlinks
--------------------

A long-standing class of security issues is the hardlink-based
time-of-check-time-of-use race, most commonly seen in world-writable
directories like ``/tmp``. The common method of exploitation of this flaw
is to cross privilege boundaries when following a given hardlink (i.e. a
root process follows a hardlink created by another user). Additionally,
on systems without separated partitions, this stops unauthorized users
from "pinning" vulnerable setuid/setgid files against being upgraded by
the administrator, or linking to special files.

When set to "0", hardlink creation behavior is unrestricted.

When set to "1" hardlinks cannot be created by users if they do not
already own the source file, or do not have read/write access to it.

This protection is based on the restrictions in Openwall and grsecurity.


protected_regular
-----------------

This protection is similar to `protected_fifos`_, but it
avoids writes to an attacker-controlled regular file, where a program
expected to create one.

When set to "0", writing to regular files is unrestricted.

When set to "1" don't allow ``O_CREAT`` open on regular files that we
don't own in world writable sticky directories, unless they are
owned by the owner of the directory.

When set to "2" it also applies to group writable sticky directories.


protected_symlinks
------------------

A long-standing class of security issues is the symlink-based
time-of-check-time-of-use race, most commonly seen in world-writable
directories like ``/tmp``. The common method of exploitation of this flaw
is to cross privilege boundaries when following a given symlink (i.e. a
root process follows a symlink belonging to another user). For a likely
incomplete list of hundreds of examples across the years, please see:
https://cve.mitre.org/cgi-bin/cvekey.cgi?keyword=/tmp

When set to "0", symlink following behavior is unrestricted.

When set to "1" symlinks are permitted to be followed only when outside
a sticky world-writable directory, or when the uid of the symlink and
follower match, or when the directory owner matches the symlink's owner.

This protection is based on the restrictions in Openwall and grsecurity.


suid_dumpable
-------------

This value can be used to query and set the core dump mode for setuid
or otherwise protected/tainted binaries. The modes are

=   ==========  ===============================================================
0   (default)	Traditional behaviour. Any process which has changed
		privilege levels or is execute only will not be dumped.
1   (debug)	All processes dump core when possible. The core dump is
		owned by the current user and no security is applied. This is
		intended for system debugging situations only.
		Ptrace is unchecked.
		This is insecure as it allows regular users to examine the
		memory contents of privileged processes.
2   (suidsafe)	Any binary which normally would not be dumped is dumped
		anyway, but only if the ``core_pattern`` kernel sysctl (see
		:ref:`Documentation/admin-guide/sysctl/kernel.rst <core_pattern>`)
		is set to
		either a pipe handler or a fully qualified path. (For more
		details on this limitation, see CVE-2006-2451.) This mode is
		appropriate when administrators are attempting to debug
		problems in a normal environment, and either have a core dump
		pipe handler that knows to treat privileged core dumps with
		care, or specific directory defined for catching core dumps.
		If a core dump happens without a pipe handler or fully
		qualified path, a message will be emitted to syslog warning
		about the lack of a correct setting.
=   ==========  ===============================================================



2. /proc/sys/fs/binfmt_misc
===========================

Documentation for the files in ``/proc/sys/fs/binfmt_misc`` is
in Documentation/admin-guide/binfmt-misc.rst.


3. /proc/sys/fs/mqueue - POSIX message queues filesystem
========================================================


The "mqueue"  filesystem provides  the necessary kernel features to enable the
creation of a  user space  library that  implements  the  POSIX message queues
API (as noted by the  MSG tag in the  POSIX 1003.1-2001 version  of the System
Interfaces specification.)

The "mqueue" filesystem contains values for determining/setting the
amount of resources used by the file system.

``/proc/sys/fs/mqueue/queues_max`` is a read/write file for
setting/getting the maximum number of message queues allowed on the
system.

``/proc/sys/fs/mqueue/msg_max`` is a read/write file for
setting/getting the maximum number of messages in a queue value.  In
fact it is the limiting value for another (user) limit which is set in
``mq_open`` invocation.  This attribute of a queue must be less than
or equal to ``msg_max``.

``/proc/sys/fs/mqueue/msgsize_max`` is a read/write file for
setting/getting the maximum message size value (it is an attribute of
every message queue, set during its creation).

``/proc/sys/fs/mqueue/msg_default`` is a read/write file for
setting/getting the default number of messages in a queue value if the
``attr`` parameter of ``mq_open(2)`` is ``NULL``. If it exceeds
``msg_max``, the default value is initialized to ``msg_max``.

``/proc/sys/fs/mqueue/msgsize_default`` is a read/write file for
setting/getting the default message size value if the ``attr``
parameter of ``mq_open(2)`` is ``NULL``. If it exceeds
``msgsize_max``, the default value is initialized to ``msgsize_max``.

4. /proc/sys/fs/epoll - Configuration options for the epoll interface
=====================================================================

This directory contains configuration options for the epoll(7) interface.

max_user_watches
----------------

Every epoll file descriptor can store a number of files to be monitored
for event readiness. Each one of these monitored files constitutes a "watch".
This configuration option sets the maximum number of "watches" that are
allowed for each user.
Each "watch" costs roughly 90 bytes on a 32-bit kernel, and roughly 160 bytes
on a 64-bit one.
The current default value for ``max_user_watches`` is 4% of the
available low memory, divided by the "watch" cost in bytes.

5. /proc/sys/fs/fuse - Configuration options for FUSE filesystems
=====================================================================

This directory contains the following configuration options for FUSE
filesystems:

``/proc/sys/fs/fuse/max_pages_limit`` is a read/write file for
setting/getting the maximum number of pages that can be used for servicing
requests in FUSE.

``/proc/sys/fs/fuse/default_request_timeout`` is a read/write file for
setting/getting the default timeout (in seconds) for a fuse server to
reply to a kernel-issued request in the event where the server did not
specify a timeout at mount. If the server set a timeout,
then default_request_timeout will be ignored.  The default
"default_request_timeout" is set to 0. 0 indicates no default timeout.
The maximum value that can be set is 65535.

``/proc/sys/fs/fuse/max_request_timeout`` is a read/write file for
setting/getting the maximum timeout (in seconds) for a fuse server to
reply to a kernel-issued request. A value greater than 0 automatically opts
the server into a timeout that will be set to at most "max_request_timeout",
even if the server did not specify a timeout and default_request_timeout is
set to 0. If max_request_timeout is greater than 0 and the server set a timeout
greater than max_request_timeout or default_request_timeout is set to a value
greater than max_request_timeout, the system will use max_request_timeout as the
timeout. 0 indicates no max request timeout. The maximum value that can be set
is 65535.

For timeouts, if the server does not respond to the request by the time
the set timeout elapses, then the connection to the fuse server will be aborted.
Please note that the timeouts are not 100% precise (eg you may set 60 seconds but
the timeout may kick in after 70 seconds). The upper margin of error for the
timeout is roughly FUSE_TIMEOUT_TIMER_FREQ seconds.
