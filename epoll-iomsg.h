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

#ifndef __SYS_EPOLL_IOMSG_H_INCLUDED
#define __SYS_EPOLL_IOMSG_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
# include <sys/platform.h>
#endif

#include <_pack64.h>
//#include <sys/epoll.h>

#define _EPOLL_IO_CONNECT_EXTRA 0x80

struct _epoll_connect_extra {
	char reserved[16];
};

enum {
	_IO_EPOLL_CTL = 0x0001,
	_IO_EPOLL_WAIT = 0x0002,
};

typedef struct {
	struct _io_msg base;
	_Uint32t op;
	int fd;
	struct epoll_event event;
} _io_epoll_ctl;

typedef union {
	_io_epoll_ctl i;
} io_epoll_ctl;

typedef struct {
	struct _io_msg base;
	int maximum_event_count;
	int timeout;
} _io_epoll_wait;

typedef struct {
	int event_count;
} _io_epoll_wait_reply;

typedef union {
	_io_epoll_wait i;
	_io_epoll_wait_reply o;
} io_epoll_wait;

#include <_packpop.h>

#endif
