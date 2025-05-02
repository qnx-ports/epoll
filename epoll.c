/*
 * Copyright 2025 QNX Software Systems Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <share.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/iomsg.h>
#include <sys/netmgr.h>
#include <sys/neutrino.h>
#include <sys/slog2.h>
#include <sys/types.h>
#include <unistd.h>

#include "sys/epoll.h"
#include "epoll-iomsg.h"

// If no-inherit connections aren't available at build (or run) time then
// epoll file descriptors are side connection identifiers (coids).  Using side
// coids ensures that epoll file descriptors are not inherited during fork/exec,
// posix_spawn, etc.  Inheriting an epoll file descriptor isn't possible when
// the epoll resource manager is in-process rather than in a separate process.
// The in-process resource manager thread stops or ceases to exist in response
// to these actions.  Usually, the fact that an epoll file descriptor is a side
// coid isn't important but it does mean that it isn't possible to include an
// epoll file descriptor in an fd_set for the select function.  It would make
// the set far too big.  Hopefully, you are using poll, epoll or just doing
// epoll_wait instead of using select.  There may be other file functionality
// where use of a side coid will be problematic.

#define DEBUG(fmt,...) slog2f(NULL, 0, SLOG2_DEBUG1, "[DEBUG] " fmt, ##__VA_ARGS__)

/* Comment out these lines to enable debug output */
#undef DEBUG
#define DEBUG(fmt, ...) do {} while(0)

//extern int _lowest_fd;  // from libc

#define ARRAY_ENTRIES(x) (sizeof (x) / sizeof (x)[0])

extern int _lowest_fd;  // from libc

__attribute__ ((visibility("default"))) int
epoll_create(int size)
{
	return epoll_create1(0);
}

__attribute__ ((visibility("default"))) int
epoll_create1(int flags)
{
	int saved_errno = EOK;
	int chid, attach_id;
	if (epoll_server_info_acquire(&chid, &attach_id) == -1)
		return -1;

	int attach_flags = 0;
	int fd = -1;
	const size_t path_offset = offsetof(struct _io_connect, path);
	static const char padding[_QNX_MSG_ALIGN - 1];
	struct iovec siov[4];
	size_t sparts;

	struct _epoll_connect_extra extra = {};
	struct _io_connect connect = {
		.type = _IO_CONNECT,
		.subtype = _IO_CONNECT_OPEN,
		.file_type = _FTYPE_ANY,
		.handle = (uint32_t)attach_id,
		.ioflag = _IO_FLAG_RD | _IO_FLAG_WR,
		.sflag = SH_DENYNO,
		.access = _IO_FLAG_RD | _IO_FLAG_WR,
		.path_len = 1,  // we always need a null byte
		.extra_type = _EPOLL_IO_CONNECT_EXTRA,
		.extra_len = sizeof extra,
	};

	if (flags & EPOLL_CLOEXEC) {
		attach_flags |= _NTO_COF_CLOEXEC;
		flags &= ~EPOLL_CLOEXEC;
	}
	if (flags) {
		saved_errno = EINVAL;
		goto err_flags;
	}

#if defined(_NTO_COF_NOINHERIT)
	// Try for a no-inherit file descriptor first.  Currently, there's some possibility that
	// this code is built with a newer SDP but gets run on an image built with an older SDP.
	// The older kernel will produce an EINVAL in response to _NTO_COF_NOINHERIT.
	fd = ConnectAttach(0, (pid_t)0, chid, _lowest_fd, attach_flags | _NTO_COF_NOINHERIT);
	if ((fd == -1) && (errno != EINVAL)) {
		saved_errno = errno;
		goto err_attach;
	}
#endif

	if (fd == -1) {
		// No file descriptor yet.  Try a side-channel (instead).
		fd = ConnectAttach(0, (pid_t)0, chid, _NTO_SIDE_CHANNEL, attach_flags);
		if (fd == -1) {
			saved_errno = errno;
			goto err_attach;
		}
	}

	// build the message
	{
		const size_t extra_offset = path_offset + connect.path_len;

		sparts = 0;
		siov[sparts++] = (struct iovec){
			.iov_base = &connect,
			.iov_len = path_offset,
		};
		siov[sparts++] = (struct iovec){
			.iov_base = (char*)&padding[0],
			.iov_len = 1,  // path ""
		};
		if (extra_offset % _QNX_MSG_ALIGN) {
			const size_t pad = _QNX_MSG_ALIGN - (extra_offset % _QNX_MSG_ALIGN);
			siov[sparts++] = (struct iovec){
				.iov_base = (char*)&padding[0],
				.iov_len = pad,
			};
		}
		siov[sparts++] = (struct iovec){
			.iov_base = &extra,
			.iov_len = sizeof extra,
		};
	}

	assert(sparts <= ARRAY_ENTRIES(siov));
	if (-1 == MsgSendvsnc(fd, siov, sparts, NULL, 0)) {
		saved_errno = errno;
		goto err_send;
	}

err_send:
	if (saved_errno != EOK) {
		ConnectDetach_r(fd);
		fd = -1;
	}
err_attach:
err_flags:
	epoll_server_info_release();
	if (saved_errno == EOK)
		return fd;
    errno = saved_errno;
    return -1;
}

__attribute__ ((visibility("default"))) int
epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	io_epoll_ctl msg;

	msg.i.base.type = _IO_MSG;
	msg.i.base.combine_len = sizeof msg.i;
	msg.i.base.mgrid = _IOMGR_PRIVATE_BASE;
	msg.i.base.subtype = _IO_EPOLL_CTL;
	msg.i.op = op;
	msg.i.fd = fd;
	if (event)
		msg.i.event = *event;

	if(MsgSend(epfd, &msg.i, sizeof(msg.i), NULL, 0) == -1) {
		return -1;
	}

	return 0;
}

__attribute__ ((visibility("default"))) int
epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout/*milliseconds*/)
{
	io_epoll_wait msg;
	iov_t iov[2];

	msg.i.base.type = _IO_MSG;
	msg.i.base.combine_len = sizeof msg.i;
	msg.i.base.mgrid = _IOMGR_PRIVATE_BASE;
	msg.i.base.subtype = _IO_EPOLL_WAIT;
	msg.i.maximum_event_count = maxevents;
	msg.i.timeout = timeout;

	SETIOV(iov, &msg.o, sizeof(msg.o));
	SETIOV(iov + 1, events, sizeof(*events) * maxevents);

	if(MsgSendsv(epfd, &msg.i, sizeof(msg.i), iov, 2) == -1) {
		return -1;
	}

	return msg.o.event_count;
}
