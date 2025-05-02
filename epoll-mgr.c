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

// A thread can only have one ionotify request armed per file descriptor.  This
// epoll implementation is single threaded.  So, as things stand at this time,
// adding a file descriptor to the interest list of more than one epoll file
// descriptor will most likely produce unexpected results.

// There's a problem with _NOTIFY_ACTION_EDGEARM that poses a challenge to
// implementing EPOLLET.  write to pipe, EDGEARM/INPUT, write, EDGEARM/INPUT
// does not produce an immediate response to the second EDGEARM.  Clearly,
// it should.

// Currently, adding an epoll file descriptor to the interest list of another
// epoll file descriptor from the same resource manager will result in the
// resource manager thread becoming SEND blocked because it tries to send an
// _IO_NOTIFY to itself.  A possible solution for this problem would be to
// bypass _IO_NOTIFY for epoll file descriptors.

#define _FILE_OFFSET_BITS 64
#define RESMGR_HANDLE_T struct epoll_resmgr
#define IOFUNC_ATTR_T struct _epoll_attr
#define RESMGR_OCB_T struct _iofunc_ocb

struct epoll_resmgr;
struct _epoll_attr;
struct _iofunc_ocb;

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <share.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/poll.h>
#include <sys/iofunc.h>
#include <sys/iomsg.h>
#include <sys/netmgr.h>
#include <sys/queue.h>
#include <sys/resmgr.h>
#include <sys/slog2.h>
#include <sys/types.h>
#include <unistd.h>

#include "sys/epoll.h"
#include "epoll-iomsg.h"

#define ARRAY_ENTRIES(x) (sizeof (x) / sizeof (x)[0])

#define FATAL(fmt,...) slog2f(NULL, 0, SLOG2_CRITICAL, "[FATAL] " fmt, ##__VA_ARGS__)
#define ERROR(fmt,...) slog2f(NULL, 0, SLOG2_ERROR, "[ERROR] " fmt, ##__VA_ARGS__)
#define WARN(fmt,...) slog2f(NULL, 0, SLOG2_WARNING, "[WARN] " fmt, ##__VA_ARGS__)
#define DEBUG(fmt,...) slog2f(NULL, 0, SLOG2_DEBUG1, "[DEBUG] " fmt, ##__VA_ARGS__)

#define TAILQ_INIT_ENTRY(elm, field) \
		do { (elm)->field.tqe_prev = NULL; (elm)->field.tqe_next = NULL; } while (0)

#define TAILQ_ENTRY_EMPTY(elm, field) \
		(((elm)->field.tqe_prev == NULL) && ((elm)->field.tqe_next == NULL))

/* Comment out these lines to enable debug output */
#undef DEBUG
#define DEBUG(fmt, ...) do {} while(0)

#ifdef NDEBUG
	#define debug_log(...)
#else
	#define debug_log(...) (void)slogf(_SLOGC_TEST, _SLOG_DEBUG2, __VA_ARGS__)
#endif

#ifndef __RCVID_T_SIZE
typedef int rcvid_t;
#endif

struct _epoll_interest_fd;
TAILQ_HEAD(_epoll_interest_fd_list_head, _epoll_interest_fd);

struct _epoll_waitobj;
TAILQ_HEAD(_epoll_waitobj_list_head, _epoll_waitobj);

struct _epoll_attr;
TAILQ_HEAD(_epoll_timer_list_head, _epoll_attr);

struct _epoll_interest_fd {
	struct _epoll_attr *attr;
	int fd;
	struct epoll_event event;
	uint32_t revents;
	TAILQ_ENTRY(_epoll_interest_fd) interest_entry;
	TAILQ_ENTRY(_epoll_interest_fd) ready_entry;
	TAILQ_ENTRY(_epoll_interest_fd) armed_entry;
};

// 'struct _epoll_attr' represents a single epoll object.
// We make a new one for every epoll_create() call.
struct _epoll_attr {
	iofunc_attr_t hdr;  // must be first
	struct epoll_resmgr *resmgr;
	TAILQ_ENTRY(_epoll_attr) list_entry;
	struct _epoll_interest_fd_list_head interest_list;
	struct _epoll_interest_fd_list_head ready_list;
	iofunc_notify_t notify[3];  // EVENT_NOTIFY_{INPUT,OUTPUT,OBAND}
	struct _epoll_waitobj_list_head waiters;
	int timeout_code;
};

struct epoll_resmgr {
	int chid, coid, attach_id;
	dispatch_t *dpp;
	bool quit;
	int quit_code;
	iofunc_attr_t devnode_attr;
	resmgr_attr_t resmgr_attr;
	resmgr_connect_funcs_t connect_funcs;
	resmgr_io_funcs_t devnode_io_funcs;
	resmgr_io_funcs_t epoll_io_funcs;
	ino_t inode_counter;
	struct sigevent notify_event;
	struct _epoll_interest_fd_list_head armed_list;
	uint32_t waitobj_id;

};

struct epoll_server_info_t
{
	int reference_count;
	int chid;
	int coid;
	int attach_id;
	int quit_code;
};

struct _epoll_waitobj {
	TAILQ_ENTRY(_epoll_waitobj) epoll_waitobj_list_entry;
	uint32_t id;
	struct _epoll_attr *attr;
	rcvid_t rcvid;
	int coid, scoid;
	io_epoll_wait msg;
	timer_t timerid;
};

static pthread_mutex_t _epoll_server_info_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct epoll_server_info_t *_epoll_server_info = NULL;

static int _epoll_wait_timeout_handler(message_context_t * ctp, int code, unsigned flags, void * handle);
static void _epoll_reply_to_waiter(rcvid_t rcvid, struct _epoll_attr *attr, int maximum_event_count);

static struct _epoll_interest_fd *
_epoll_find_interest_fd(struct _epoll_attr *attr, int fd)
{
	struct _epoll_interest_fd *interest_fd, *next_interest_fd;
	TAILQ_FOREACH_SAFE(interest_fd, &attr->interest_list, interest_entry, next_interest_fd) {
		if (interest_fd->fd == fd)
			return interest_fd;
	}
	return NULL;
}

static errno_t
_epoll_attr_alloc(struct _epoll_attr **attr_out,
		struct epoll_resmgr *resmgr,
		struct _client_info *owner)
{
	errno_t err = EOK;
	struct _epoll_attr *attr;

	attr = malloc(sizeof *attr);
	if (!attr) {
		return ENOMEM;
	}

	attr->resmgr = resmgr;
	iofunc_attr_init(&attr->hdr, 0666, NULL /*dattr*/, owner);
	IOFUNC_NOTIFY_INIT(attr->notify);
	TAILQ_INIT(&attr->waiters);
	TAILQ_INIT(&attr->interest_list);
	TAILQ_INIT(&attr->ready_list);

	attr->hdr.inode = ++resmgr->inode_counter;

	/* Attach the timeout pulse handler for this epoll object.
	 *  Note that this implementation is limited by the number of pulse codes that can be allocated by
	 *  pulse_attach() (128).
	 */
	attr->timeout_code = pulse_attach(resmgr->dpp, MSG_FLAG_ALLOC_PULSE, 0, _epoll_wait_timeout_handler, attr);
	if (attr->timeout_code == -1) {
		err = errno;
		goto out;
	}

	if (epoll_server_info_acquire(NULL, NULL) == -1) {
		err = errno;
		goto out;
	}

out:
	if (err) {
		free(attr);
	} else {
		*attr_out = attr;
	}
	return err;
}

static unsigned int
_epoll_interest_fd_ionotify(struct _epoll_interest_fd *interest_fd, int action, struct sigevent *event)
{
	// This is based on _poll from lib/c/xopen/poll.c.
	//
	// Extended _IO_NOTIFY is...complex and apparently, unexplained.
	// Each server receives the poll list or some contiguous portion
	// of it.  The server scans the poll list for file descriptors
	// (coids) that the client has open with the server.  For any
	// such fds, it runs the _IO_NOTIFY handler and knocks down the
	// POLLRESERVED bit in pollfd::event. At the same time, it moves
	// fd_first forward over any such fds at the beginning of the list
	// _and_ any fds that no longer have the POLLRESERVED bit set in
	// pollfd::event.  Similar, it decreases nfds so that any such fds
	// at the end of the list are no longer counted _and_ any fds that
	// no longer have the POLLRESERVED bit set in pollfd::event are
	// also no longer counted.  So, servers end up eliminating fds
	// from the list that were processed by another server.  Why the
	// fd_first/nfds changes aren't done in _poll, I don't know.
	// Example:
	//     Suppose a client supplies a poll list with file descriptors
	//     (coids) for servers A, B and C like so:
	//          A1 B1 C1 C2 A2 B2
	//
	//     When server A processes the list, it will move fd_first to
	//     B1 and knock down the POLLRESERVED bit for A2.  nfds will
	//     be reduced by one to reflect these changes.
	//
	//     When server B processes the list, it will move fd_first to
	//     C1 and reduce nfds by 3.  One for B1, one for B2 and one for
	//     A2 because it no longer has POLLRESERVED set.
	//
	//     Finally, server C will process the list, moving fd_first
	//     foward by 2 and reducing nfds by 2.
	//
	// You'll need to know this if you decide that epoll needs to
	// let servers process more than one epoll interest fd at a time.
	int extra_mask = EPOLLERR | EPOLLHUP | EPOLLNVAL;
	struct pollfd notify_fd;
	notify_fd.fd = interest_fd->fd;
	notify_fd.events = POLLRESERVED | interest_fd->event.events;  // Knock off any EPOLL specific flags.  Currently, there aren't any.
	notify_fd.revents = 0;
	io_notify_t msg;
	iov_t iov[2];
	int exten_flag;

	msg.i.flags_extra_mask = extra_mask;
	msg.i.mgr[0] = 0;
	msg.i.mgr[1] = 0;
	msg.i.timo       = -1;
	msg.i.nfds       = 1;
	msg.i.fd_first   = 0;
	msg.i.nfds_ready = 0;

	SETIOV(&iov[0], &msg, sizeof(msg.i));
	SETIOV(&iov[1], &notify_fd, sizeof(notify_fd));

	exten_flag = _NOTIFY_COND_EXTEN;

	// The "loop" is just here for the ENOSYS continue.
	do {
		msg.i.type = _IO_NOTIFY;
		msg.i.combine_len = sizeof(msg.i);
		msg.i.action = action;

		msg.i.flags_exten = 0;

		msg.i.flags = (unsigned short)notify_fd.events |
			  ((unsigned short)notify_fd.events << 28);
		msg.i.flags &= ~POLLRESERVED;
		msg.i.flags |= exten_flag | extra_mask;

		if (event)
#if __QNX__ < 800
			msg.i.event = *(struct __sigevent32 *)event;
#else
		  msg.i.event = *event;
#endif
		else
			SIGEV_NONE_INIT(&msg.i.event);

		if (MsgSendv(notify_fd.fd, iov, 2, iov, 2) == -1) {
			if (errno == ENOSYS) {
				// The server rejects "unsupported" bits instead of simply
				// ignoring them.  Try again without _NOTIFY_COND_EXTEN.
				if (exten_flag != 0) {
					exten_flag = 0;
					continue;
				}
				notify_fd.revents = notify_fd.events & (POLLRDNORM | POLLWRNORM);
			}
			else if (errno == EBADF) {
				notify_fd.revents = POLLNVAL;
			}
			else {
				return UINT_MAX;
			}
		}
		else if (notify_fd.events & POLLRESERVED) {
			// The server doesn't understand extended _IO_NOTIFY.
			if (msg.o.flags) {
				// I'm not sure why this is done this way since the assignment
				// to revents chops off the upper bits (pollfd::revents is a
				// short).
				msg.o.flags |= (msg.o.flags & ~_NOTIFY_COND_EXTEN) >> 28;
				notify_fd.revents = msg.o.flags;
			}
		}
	} while (0);

	return notify_fd.revents;
}

static void
_epoll_interest_fd_pollarm(struct _epoll_interest_fd* interest_fd)
{
	struct sigevent event = interest_fd->attr->resmgr->notify_event;
	event.sigev_value.sival_int = interest_fd->fd;
	unsigned int ionotify_result = _epoll_interest_fd_ionotify(interest_fd, _NOTIFY_ACTION_POLLARM, &event);
	if (ionotify_result != UINT_MAX) {
		DEBUG("%s %d %x %x", __FUNCTION__, interest_fd->fd, interest_fd->event.events, ionotify_result);
		interest_fd->revents = ionotify_result;
		if (ionotify_result == 0) {
			TAILQ_INSERT_HEAD(&interest_fd->attr->resmgr->armed_list, interest_fd, armed_entry);
		} else {
			TAILQ_INSERT_TAIL(&interest_fd->attr->ready_list, interest_fd, ready_entry);
		}
	}
}

static void
_epoll_rearm_interest_list(struct _epoll_attr *attr)
{
	struct _epoll_interest_fd *interest_fd, *next_interest_fd;
	TAILQ_FOREACH_SAFE(interest_fd, &attr->interest_list, interest_entry, next_interest_fd) {
		if (TAILQ_ENTRY_EMPTY(interest_fd, ready_entry) &&
				TAILQ_ENTRY_EMPTY(interest_fd, armed_entry)) {
			_epoll_interest_fd_pollarm(interest_fd);
		}
	}
}

static bool
_epoll_can_read(const struct _epoll_attr *attr)
{
	return !TAILQ_EMPTY(&attr->ready_list);
}

static void
_epoll_destroy_waitobj(struct _epoll_waitobj* waitobj)
{
	if (waitobj->timerid)
		timer_delete(waitobj->timerid);

	free(waitobj);
}

static int
_epoll_wait_timeout_handler(message_context_t * ctp, int code, unsigned flags, void * handle)
{
	uint32_t id = ctp->msg->pulse.value.sival_int;
	struct _epoll_attr *attr = handle;

	struct _epoll_waitobj *waitobj, *next_waitobj;
	TAILQ_FOREACH_SAFE(waitobj, &attr->waiters, epoll_waitobj_list_entry, next_waitobj) {
		if (waitobj->id == id) {
			/* Found the waitobj for this timeout pulse, remove from the waiters list and reply to waiter */
			TAILQ_REMOVE(&attr->waiters, waitobj, epoll_waitobj_list_entry);
			_epoll_reply_to_waiter(waitobj->rcvid, attr, waitobj->msg.i.maximum_event_count);
			_epoll_destroy_waitobj(waitobj);
			return 0;
		}
	}
	return 0;
}

static int
_epoll_queue_wait(resmgr_context_t *ctp, io_epoll_wait *msg, iofunc_ocb_t *ocb)
{
	struct _epoll_attr *attr = ocb->attr;
	struct _epoll_waitobj *waitobj;

	waitobj = malloc(sizeof *waitobj);
	if (!waitobj) {
		return ENOMEM;
	}
	*waitobj = (struct _epoll_waitobj){
		.rcvid = ctp->rcvid,
		.coid = ctp->info.coid,
		.scoid = ctp->info.scoid,
		.msg = *msg,
		.id = 0
	};

	/* If non-zero, not-forever timeout */
	if (msg->i.timeout > 0) {
		/* Setup the timer's pulse event with a unique value (unique to this resmgr) */
		struct sigevent timeout_event;
		waitobj->id = ++attr->resmgr->waitobj_id;
		SIGEV_PULSE_INT_INIT(&timeout_event, attr->resmgr->coid, SIGEV_PULSE_PRIO_INHERIT, attr->timeout_code, waitobj->id);
		/* Create the timer, binding it to the timeout pulse event */
		if (timer_create(CLOCK_MONOTONIC, &timeout_event, &waitobj->timerid) == -1) {
			debug_log("timer_create() for timeout failed (rcvid=%#x), errno=%d", ctp->rcvid, errno);
			return -1;
		}
		/* Set the timeout, no repeating interval */
		struct itimerspec timer;
		timer.it_value.tv_sec = msg->i.timeout / 1000;
		timer.it_value.tv_nsec = (msg->i.timeout % 1000) * (1000 * 1000);
		timer.it_interval.tv_sec = 0;
		timer.it_interval.tv_nsec = 0;
		/* Arm the timer */
		timer_settime(waitobj->timerid, 0, &timer, NULL);
	}

	TAILQ_INSERT_TAIL(&attr->waiters, waitobj, epoll_waitobj_list_entry);

	return _RESMGR_NOREPLY;
}

static void
_epoll_reply_to_waiter(rcvid_t rcvid, struct _epoll_attr *attr, int maximum_event_count)
{
	_io_epoll_wait_reply reply;
	reply.event_count = 0;
	int offset = sizeof(reply);
	struct _epoll_interest_fd *ready_fd, *next_ready_fd;
	TAILQ_FOREACH_SAFE(ready_fd, &attr->ready_list, ready_entry, next_ready_fd) {
		struct epoll_event event = ready_fd->event;
		event.events = ready_fd->revents;
		DEBUG("%s %d %x", __FUNCTION__, ready_fd->fd, event.events);
		if (MsgWrite(rcvid, &event, sizeof(event), offset) == -1) {
			(void)MsgError_r(rcvid, errno);
			return;
		}
		offset += sizeof(event);
		TAILQ_REMOVE(&attr->ready_list, ready_fd, ready_entry);
		TAILQ_INIT_ENTRY(ready_fd, ready_entry);
		if (!TAILQ_ENTRY_EMPTY(ready_fd, armed_entry)) {
			TAILQ_REMOVE(&attr->resmgr->armed_list, ready_fd, armed_entry);
			TAILQ_INIT_ENTRY(ready_fd, armed_entry);
		}
		++reply.event_count;
		if (reply.event_count == maximum_event_count)
			break;
	}

	DEBUG("%s %d", __FUNCTION__, reply.event_count);
	if (MsgReply(rcvid, 0, &reply, sizeof(reply)) == -1)
		(void)MsgError_r(rcvid, errno);
}

static bool
_epoll_wake_waiters(struct _epoll_attr *attr)
{
	bool woke_any = false;
	while (_epoll_can_read(attr) && !TAILQ_EMPTY(&attr->waiters)) {
		struct _epoll_waitobj *waitobj = TAILQ_FIRST(&attr->waiters);
		TAILQ_REMOVE(&attr->waiters, waitobj, epoll_waitobj_list_entry);
		_epoll_reply_to_waiter(waitobj->rcvid, attr, waitobj->msg.i.maximum_event_count);

		_epoll_destroy_waitobj(waitobj);
		woke_any = true;
	}
	return woke_any;
}

static void
_epoll_send_notifications(struct _epoll_attr *attr)
{
	while (_epoll_wake_waiters(attr)) {}
	if (_epoll_can_read(attr)) {
		iofunc_notify_trigger(attr->notify, 1, IOFUNC_NOTIFY_INPUT);
	}
}

static void
_epoll_waitobj_wake_coid_ebadf(struct _epoll_waitobj_list_head *list,
		int coid, int scoid)
{
	struct _epoll_waitobj *waitobj, *next_waitobj;
	TAILQ_FOREACH_SAFE(waitobj, list, epoll_waitobj_list_entry, next_waitobj) {
		if ((waitobj->coid == coid) && (waitobj->scoid == scoid)) {
			TAILQ_REMOVE(list, waitobj, epoll_waitobj_list_entry);
			if (-1 == MsgError(waitobj->rcvid, EBADF)) {
				debug_log("MsgError(rcvid=%#x) failed, errno=%d", waitobj->rcvid, errno);
			}
			_epoll_destroy_waitobj(waitobj);
		}
	}
}

static bool
_epoll_waitobj_wipe_rcvid(struct _epoll_waitobj_list_head *list, int rcvid)
{
	bool deleted_waitobj = false;
	struct _epoll_waitobj *waitobj, *next_waitobj;
	TAILQ_FOREACH_SAFE(waitobj, list, epoll_waitobj_list_entry, next_waitobj) {
		if (waitobj->rcvid == rcvid) {
			TAILQ_REMOVE(list, waitobj, epoll_waitobj_list_entry);
			_epoll_destroy_waitobj(waitobj);

			assert(!deleted_waitobj);
			deleted_waitobj = true;
		}
	}
	return deleted_waitobj;
}

static int
_epoll_quit_handler(message_context_t * ctp, int code, unsigned flags, void * handle)
{
	struct epoll_resmgr *resmgr = handle;
	resmgr->quit = true;
	return 0;
}

static int
_epoll_interest_fd_ionotify_handler(message_context_t * ctp, int code, unsigned flags, void * handle)
{
	int fd = ctp->msg->pulse.value.sival_int;
	struct epoll_resmgr *resmgr = handle;

	struct _epoll_interest_fd *armed_fd, *next_armed_fd;
	TAILQ_FOREACH_SAFE(armed_fd, &resmgr->armed_list, armed_entry, next_armed_fd) {
		if (armed_fd->fd == fd) {
			struct sigevent event = resmgr->notify_event;
			event.sigev_value.sival_int = armed_fd->fd;
			unsigned int ionotify_result = _epoll_interest_fd_ionotify(armed_fd, _NOTIFY_ACTION_POLLARM, &event);
			if (ionotify_result == UINT_MAX)
				return 0;

			DEBUG("%s %d %x", __FUNCTION__, armed_fd->fd, ionotify_result);
			armed_fd->revents = ionotify_result;

			if (ionotify_result == 0) {
				// Probably a stale notification.
				DEBUG("%s stale", __FUNCTION__);
				if (!TAILQ_ENTRY_EMPTY(armed_fd, ready_entry)) {
					TAILQ_REMOVE(&armed_fd->attr->ready_list, armed_fd, ready_entry);
					TAILQ_INIT_ENTRY(armed_fd, ready_entry);
				}
				return 0;
			}

			if (TAILQ_ENTRY_EMPTY(armed_fd, ready_entry))
				TAILQ_INSERT_TAIL(&armed_fd->attr->ready_list, armed_fd, ready_entry);
			TAILQ_REMOVE(&resmgr->armed_list, armed_fd, armed_entry);
			TAILQ_INIT_ENTRY(armed_fd, armed_entry);
			_epoll_send_notifications(armed_fd->attr);
			return 0;
		}
	}
	return 0;
}


// resmgr I/O handlers

static int
_epoll_devnode_open(resmgr_context_t *ctp, io_open_t *msg,
		/*RESMGR_HANDLE_T*/ struct epoll_resmgr *resmgr, void *extra_in)
{
	errno_t err;
	struct _epoll_connect_extra *extra = extra_in;
	struct _client_info *cinfo = NULL;
	struct _epoll_attr *attr = NULL;

	switch (msg->connect.extra_type) {
	case _IO_CONNECT_EXTRA_NONE:
		// This is a regular open() for our device node (e.g., from 'ls -l').
		// The OCB will use the default I/O handlers from 'devnode_io_funcs',
		// meaning that no more of our _epoll_*() handlers will be called.
		return iofunc_open_default(ctp, msg, &resmgr->devnode_attr, extra);
	case _EPOLL_IO_CONNECT_EXTRA:
		// This is an epoll_create() call.
		break;
	default:
		return ENOSYS;
	}

	if (msg->connect.file_type != _FTYPE_ANY) {
		return ENOSYS;
	}

	if (msg->connect.extra_len != sizeof *extra) {
		return EBADMSG;
	} else if ((size_t)ctp->info.msglen < (sizeof msg + sizeof extra)) {
		return EMSGSIZE;
	}

	err = iofunc_client_info_ext(ctp, (int)msg->connect.ioflag, &cinfo, IOFUNC_CLIENTINFO_GETGROUPS);
	if (err) {
		return err;
	}

	// Create a new epoll object.
	err = _epoll_attr_alloc(&attr, resmgr, cinfo);
	if (err) {
		return err;
	}
	assert(attr);

	// Check permissions, flags, etc.
	err = iofunc_open(ctp, msg, &attr->hdr, NULL /*dattr*/, cinfo);
	if (err) {
		goto out;
	}

	// We're overriding the I/O functions here to make the new object
	// act like an epoll (rather than a device node).
	err = iofunc_ocb_attach(ctp, msg, NULL /*ocb*/,
			&attr->hdr, &resmgr->epoll_io_funcs);

out:
	if (err) {
		free(attr);
	}
	if (cinfo) {
		(void)iofunc_client_info_ext_free(&cinfo);
	}
	return err;
}

static int
_epoll_io_close_dup(resmgr_context_t *ctp, io_close_t *msg, iofunc_ocb_t *ocb)
{
	struct _epoll_attr *attr = ocb->attr;

	iofunc_lock_ocb_default(ctp, NULL, ocb);
	{
		// unblock any threads waiting for notification
		iofunc_notify_trigger_strict(ctp, attr->notify, INT_MAX, IOFUNC_NOTIFY_INPUT);
		iofunc_notify_remove(ctp, attr->notify);

		// unblock any threads reply-blocked on the FD being closed
		_epoll_waitobj_wake_coid_ebadf(&attr->waiters,
				ctp->info.coid, ctp->info.scoid);
		_epoll_send_notifications(attr);
	}
	iofunc_unlock_ocb_default(ctp, NULL, ocb);

	return iofunc_close_dup_default(ctp, msg, ocb);
}

static int
_epoll_io_close_ocb(resmgr_context_t *ctp, void *reserved, iofunc_ocb_t *ocb)
{
	struct _epoll_attr *attr = ocb->attr;
	int detach_rv;

	(void)reserved;

	iofunc_lock_ocb_default(ctp, NULL, ocb);
	detach_rv = iofunc_ocb_detach(ctp, ocb);
	iofunc_unlock_ocb_default(ctp, NULL, ocb);

	free(ocb);
	ocb = NULL;

	// Destroy 'attr' if nobody else is using it.
	// (Another OCB could be using it if openfd() were used.)
	if (detach_rv & IOFUNC_OCB_LAST_INUSE) {
		struct _epoll_interest_fd *interest_fd;
		while ((interest_fd = TAILQ_FIRST(&attr->interest_list))) {
			TAILQ_REMOVE(&attr->interest_list, interest_fd, interest_entry);
			free(interest_fd);
		}
		pulse_detach(attr->resmgr->dpp, attr->timeout_code, 0);
		epoll_server_info_release();
		free(attr);
		attr = NULL;
	}

	return EOK;
}

static int
_epoll_io_msg_epoll_ctl_add(resmgr_context_t *ctp, io_epoll_ctl *msg, iofunc_ocb_t *ocb)
{
	struct _epoll_attr *attr = ocb->attr;

	struct _epoll_interest_fd *interest_fd = _epoll_find_interest_fd(attr, msg->i.fd);
	if (interest_fd)
		return EEXIST;

	interest_fd = malloc(sizeof(*interest_fd));
	if (!interest_fd)
		return ENOMEM;

	interest_fd->attr = attr;
	interest_fd->fd = msg->i.fd;
	interest_fd->event = msg->i.event;

	DEBUG("%s %d %x", __FUNCTION__, interest_fd->fd, interest_fd->event.events);
	TAILQ_INIT_ENTRY(interest_fd, ready_entry);
	TAILQ_INIT_ENTRY(interest_fd, armed_entry);
	TAILQ_INSERT_TAIL(&attr->interest_list, interest_fd, interest_entry);

	_epoll_interest_fd_pollarm(interest_fd);

	return EOK;
}

static int
_epoll_io_msg_epoll_ctl_mod(resmgr_context_t *ctp, io_epoll_ctl *msg, iofunc_ocb_t *ocb)
{
	struct _epoll_attr *attr = ocb->attr;

	struct _epoll_interest_fd *interest_fd = _epoll_find_interest_fd(attr, msg->i.fd);
	if (!interest_fd)
		return ENOENT;

	interest_fd->event = msg->i.event;

	DEBUG("%s %d %x", __FUNCTION__, interest_fd->fd, interest_fd->event.events);
	if (!TAILQ_ENTRY_EMPTY(interest_fd, ready_entry)) {
		TAILQ_REMOVE(&attr->ready_list, interest_fd, ready_entry);
		TAILQ_INIT_ENTRY(interest_fd, ready_entry);
	}
	if (!TAILQ_ENTRY_EMPTY(interest_fd, armed_entry)) {
		TAILQ_REMOVE(&attr->resmgr->armed_list, interest_fd, armed_entry);
		TAILQ_INIT_ENTRY(interest_fd, armed_entry);
	}

	_epoll_interest_fd_pollarm(interest_fd);

	return EOK;
}

static int
_epoll_io_msg_epoll_ctl_del(resmgr_context_t *ctp, io_epoll_ctl *msg, iofunc_ocb_t *ocb)
{
	struct _epoll_attr *attr = ocb->attr;

	struct _epoll_interest_fd *interest_fd = _epoll_find_interest_fd(attr, msg->i.fd);
	if (!interest_fd)
		return ENOENT;

	DEBUG("%s %d", __FUNCTION__, interest_fd->fd);
	if (!TAILQ_ENTRY_EMPTY(interest_fd, ready_entry))
		TAILQ_REMOVE(&attr->ready_list, interest_fd, ready_entry);
	if (!TAILQ_ENTRY_EMPTY(interest_fd, armed_entry))
		TAILQ_REMOVE(&attr->resmgr->armed_list, interest_fd, armed_entry);
	TAILQ_REMOVE(&attr->interest_list, interest_fd, interest_entry);

	return EOK;
}

static int
_epoll_io_msg_epoll_ctl(resmgr_context_t *ctp, io_epoll_ctl *msg, iofunc_ocb_t *ocb)
{
	switch (msg->i.op) {
	case EPOLL_CTL_ADD:
		return _epoll_io_msg_epoll_ctl_add(ctp, msg, ocb);
	case EPOLL_CTL_DEL:
		return _epoll_io_msg_epoll_ctl_del(ctp, msg, ocb);
	case EPOLL_CTL_MOD:
		return _epoll_io_msg_epoll_ctl_mod(ctp, msg, ocb);
	}
	return EOK;
}

static int
_epoll_io_msg_epoll_wait(resmgr_context_t *ctp, io_epoll_wait *msg, iofunc_ocb_t *ocb)
{
	struct _epoll_attr *attr = ocb->attr;

	/* Allow non-zero, don't wait (0), and wait forever (-1) timeouts */
	if (msg->i.timeout < -1)
		return EINVAL;

	_epoll_rearm_interest_list(attr);

	if ((msg->i.timeout == 0) || _epoll_can_read(attr)) {
		_epoll_reply_to_waiter(ctp->rcvid, attr, msg->i.maximum_event_count);
		return _RESMGR_NOREPLY;
	} else {
		return _epoll_queue_wait(ctp, msg, ocb);
	}
}

static int
_epoll_io_msg(resmgr_context_t *ctp, io_msg_t *msg, iofunc_ocb_t *ocb)
{
	switch (msg->i.subtype) {
	case _IO_EPOLL_CTL:
		return _epoll_io_msg_epoll_ctl(ctp, (io_epoll_ctl *)msg, ocb);
	case _IO_EPOLL_WAIT:
		return _epoll_io_msg_epoll_wait(ctp, (io_epoll_wait *)msg, ocb);
	}
	return _RESMGR_NOREPLY;
}

static int
_epoll_io_notify(resmgr_context_t *ctp, io_notify_t *msg, iofunc_ocb_t *ocb)
{
	DEBUG("%s %p %d %x", __FUNCTION__, ocb, ctp->info.coid, msg->i.flags);

	struct _epoll_attr *attr = ocb->attr;
	int ready = 0;

	_epoll_rearm_interest_list(attr);

	if (_epoll_can_read(attr)) {
		ready |= _NOTIFY_COND_INPUT;
	}
	return iofunc_notify(ctp, msg, attr->notify, ready, NULL, NULL);
}

static int
_epoll_io_unblock(resmgr_context_t *ctp, io_pulse_t *msg, iofunc_ocb_t *ocb)
{
	struct _epoll_attr *attr = ocb->attr;
	struct _msg_info info;
	bool deleted_waitobj = false;
	int status;

	assert(ctp->rcvid);
	if ((MsgInfo(ctp->rcvid, &info) == -1)
			|| !(info.flags & _NTO_MI_UNBLOCK_REQ)) {
		// the request is no longer pending
		return _RESMGR_NOREPLY;
	}

	status = iofunc_unblock_default(ctp, msg, ocb);
	if (status != _RESMGR_DEFAULT) {
		return status;
	}

	// iterate over IOFUNC_NOTIFY_{INPUT,OUTPUT}
	deleted_waitobj |= _epoll_waitobj_wipe_rcvid(&attr->waiters, ctp->rcvid);
	if (!deleted_waitobj) {
		return _RESMGR_NOREPLY;  // shouldn't happen
	}

	// wake the blocked thread with EINTR or ETIMEDOUT as appropriate
	// (assuming we're on kernel 7.0.1 or later)
	if (-1 == MsgError(ctp->rcvid, -1)) {
		debug_log("MsgError(rcvid=%#x) failed, errno=%d", ctp->rcvid, errno);
	}

	return _RESMGR_NOREPLY;
}

static void
_epoll_resmgr_destroy(struct epoll_resmgr *resmgr)
{
	if (resmgr) {
		if ((resmgr->coid != -1) && (resmgr->notify_event.sigev_notify & SIGEV_FLAG_HANDLE)) {
			MsgUnregisterEvent((struct sigevent *)&resmgr->notify_event);
			SIGEV_NONE_INIT(&resmgr->notify_event);
		}
		if (resmgr->coid != -1) {
			if (-1 == ConnectDetach(resmgr->coid))
				assert(0 && "ConnectDetach");
		}
		if (resmgr->attach_id != -1) {
			assert(resmgr->dpp);
			// resmgr_detach closes all the OCBs
			if (-1 == resmgr_detach(resmgr->dpp,
					resmgr->attach_id, 0 /*flags*/)) {
				assert(0 && "resmgr_detach_ctp");
			}
		}
		if (resmgr->dpp) {
			// dispatch_destroy() destroys the channel
			dispatch_destroy(resmgr->dpp);
			resmgr->chid = -1;
		}
		if (resmgr->chid != -1) {
			if (-1 == ChannelDestroy(resmgr->chid)) {
				assert(0 && "ChannelDestroy");
			}
		}
		free(resmgr);
	}
}

static errno_t
_epoll_resmgr_create(struct epoll_resmgr **resmgr_out,
		const char *path, dev_t devno)
{
	errno_t err;
	struct epoll_resmgr *resmgr = NULL;

	resmgr = malloc(sizeof *resmgr);
	if (!resmgr) {
		err = ENOMEM;
		goto out;
	}
	*resmgr = (struct epoll_resmgr){
		.chid = -1,
		.coid = -1,
		.attach_id = -1,
	};

	resmgr->chid = ChannelCreate(_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT
			| (path ? 0 : _NTO_CHF_PRIVATE));
	if (resmgr->chid < 0) {
		err = errno;
		goto out;
	}

	resmgr->dpp = dispatch_create_channel(resmgr->chid, 0 /*flags*/);
	if (!resmgr->dpp) {
		err = errno;
		goto out;
	}

	iofunc_attr_init(&resmgr->devnode_attr, S_IFNAM | 0666,
			NULL /*dattr*/, NULL /*client_info*/);
	resmgr->devnode_attr.rdev = devno;

	memset(&resmgr->resmgr_attr, '\0', sizeof resmgr->resmgr_attr);
	resmgr->resmgr_attr.flags |= (path ? 0 : RESMGR_FLAG_ATTACH_LOCAL);
	resmgr->resmgr_attr.nparts_max = 1;
	resmgr->resmgr_attr.msg_max_size = 2048;

	iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &resmgr->connect_funcs,
	                 _RESMGR_IO_NFUNCS, &resmgr->devnode_io_funcs);
	resmgr->connect_funcs.open = &_epoll_devnode_open;

	iofunc_func_init(0, NULL, _RESMGR_IO_NFUNCS, &resmgr->epoll_io_funcs);
	resmgr->epoll_io_funcs.close_dup = &_epoll_io_close_dup;
	resmgr->epoll_io_funcs.close_ocb = &_epoll_io_close_ocb;
	resmgr->epoll_io_funcs.msg = &_epoll_io_msg;
	resmgr->epoll_io_funcs.notify = &_epoll_io_notify;
	resmgr->epoll_io_funcs.unblock = &_epoll_io_unblock;

	resmgr->quit = false;
	resmgr->quit_code = pulse_attach(resmgr->dpp, MSG_FLAG_ALLOC_PULSE,
			0, _epoll_quit_handler, resmgr);
	if (resmgr->quit_code == -1) {
		err = errno;
		goto out;
	}

	int ionotify_code = pulse_attach(resmgr->dpp, MSG_FLAG_ALLOC_PULSE, 0, _epoll_interest_fd_ionotify_handler, resmgr);
	if (ionotify_code == -1) {
		err = errno;
		goto out;
	}

	resmgr->attach_id = resmgr_attach(resmgr->dpp, &resmgr->resmgr_attr,
			path, _FTYPE_ANY, 0 /*attach_flags*/, &resmgr->connect_funcs,
			&resmgr->devnode_io_funcs, resmgr /*RESMGR_HANDLE_T*/);
	if (resmgr->attach_id == -1) {
		err = errno;
		goto out;
	}

	resmgr->coid = message_connect(resmgr->dpp, MSG_FLAG_SIDE_CHANNEL);
	if (resmgr->coid == -1) {
		err = errno;
		goto out;
	}

	SIGEV_PULSE_INT_INIT(&resmgr->notify_event, resmgr->coid, SIGEV_PULSE_PRIO_INHERIT, ionotify_code, 0);
	SIGEV_MAKE_UPDATEABLE(&resmgr->notify_event);
	if (MsgRegisterEvent((struct sigevent *)&resmgr->notify_event, -1) == -1) {
		err = errno;
		goto out;
	}

	resmgr->inode_counter = 0;
	resmgr->waitobj_id = 0;
	TAILQ_INIT(&resmgr->armed_list);

	err = EOK;
out:
	if (err) {
		_epoll_resmgr_destroy(resmgr);
	} else {
		assert(resmgr);
		*resmgr_out = resmgr;
	}
	return err;
}

struct _epoll_resmger_thread_argument
{
	pthread_barrier_t setup_barrier;
	struct epoll_server_info_t *server_info;
	int setup_errno;
};

static void *
_epoll_resmgr_thread_main(void *v_argument)
{
	(void)pthread_setname_np(pthread_self(), "epoll-mgr");

	struct epoll_resmgr *resmgr = NULL;
	dispatch_context_t *ctx;
	{
		struct _epoll_resmger_thread_argument *argument = v_argument;

		argument->setup_errno = _epoll_resmgr_create(&resmgr, NULL, (dev_t)-1);
		if (argument->setup_errno) {
			pthread_barrier_wait(&argument->setup_barrier);
			return NULL;
		}

		ctx = dispatch_context_alloc(resmgr->dpp);
		if (!ctx) {
			_epoll_resmgr_destroy(resmgr);
			argument->setup_errno = errno;
			pthread_barrier_wait(&argument->setup_barrier);
			return NULL;
		}

		argument->server_info->chid = resmgr->chid;
		argument->server_info->coid = resmgr->coid;
		argument->server_info->attach_id = resmgr->attach_id;
		argument->server_info->quit_code = resmgr->quit_code;
		pthread_barrier_wait(&argument->setup_barrier);
	}

	for (; !resmgr->quit; ) {
		if (dispatch_block(ctx))
			dispatch_handler(ctx);
	}
	dispatch_context_free(ctx);
	_epoll_resmgr_destroy(resmgr);
	return NULL;
}

static struct epoll_server_info_t *
_epoll_resmgr_start()
{
	int saved_errno = EOK;
	struct _epoll_resmger_thread_argument thread_argument;
	thread_argument.server_info = malloc(sizeof(*thread_argument.server_info));
	if (!thread_argument.server_info) {
		saved_errno = errno;
		goto err_malloc;
	}

	thread_argument.server_info->reference_count = 0;
	thread_argument.server_info->chid = -1;
	thread_argument.server_info->coid = -1;
	thread_argument.server_info->attach_id = -1;
	thread_argument.server_info->quit_code = 0;

	if (pthread_barrier_init(&thread_argument.setup_barrier, NULL, 2) != 0) {
		saved_errno = errno;
		goto err_barrier;
	}

	pthread_attr_t attr;
	if (pthread_attr_init(&attr) != 0) {
		saved_errno = errno;
		goto err_attr;
	}

	if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
		saved_errno = errno;
		goto err_detached;
	}

	if (pthread_create(NULL, &attr, _epoll_resmgr_thread_main, &thread_argument) != 0) {
		saved_errno = errno;
		goto err_detached;
	}

	int wait_result = pthread_barrier_wait(&thread_argument.setup_barrier);
	if ((wait_result != 0) && (wait_result != PTHREAD_BARRIER_SERIAL_THREAD)) {
		perror("barrier wait failed");
		saved_errno = errno;
		goto err_detached; // What can you possibly do?
	}

	if (thread_argument.server_info->chid == -1) {
		saved_errno = thread_argument.setup_errno;
		goto err_detached;
	}

err_detached:
	pthread_attr_destroy(&attr);
err_attr:
	pthread_barrier_destroy(&thread_argument.setup_barrier);
err_barrier:
	if (saved_errno != EOK) {
		free(thread_argument.server_info);
		thread_argument.server_info = NULL;
	}
err_malloc:
	if (saved_errno == EOK)
		return thread_argument.server_info;
	errno = saved_errno;
	return NULL;
}

// public functions

__attribute__ ((visibility("default"))) int
epoll_server_info_acquire(int *chid, int *attach_id)
{
	int result = -1;

	pthread_mutex_lock(&_epoll_server_info_mutex);

	if (!_epoll_server_info)
		_epoll_server_info = _epoll_resmgr_start();
	int saved_errno = errno;

	if (_epoll_server_info) {
		++_epoll_server_info->reference_count;
		if (chid)
			*chid = _epoll_server_info->chid;
		if (attach_id)
			*attach_id = _epoll_server_info->attach_id;
		result = 0;
	}

	pthread_mutex_unlock(&_epoll_server_info_mutex);

	errno = saved_errno;
	return result;
}

__attribute__ ((visibility("default"))) void
epoll_server_info_release()
{
	struct epoll_server_info_t *tmp = NULL;

	pthread_mutex_lock(&_epoll_server_info_mutex);
	--_epoll_server_info->reference_count;
	if (_epoll_server_info->reference_count <= 0) {
		tmp = _epoll_server_info;
		_epoll_server_info = NULL;
	}
	pthread_mutex_unlock(&_epoll_server_info_mutex);

	if (tmp) {
		if (MsgSendPulse(tmp->coid, -1, tmp->quit_code, 0) == -1)
			perror("failed to deliver quit request");
		free(tmp);
	}
}
