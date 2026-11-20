/*
 * Copyright (c) 2026 reygnn
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */


#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "rdp/deadline.h"

struct rdp_deadline {
	pthread_t thread;
	/* Written to by stop: the thread wakes up and ends. */
	int pipe[2];
	uint64_t timeout_us;
	void (*fire)(void* arg);
	void* arg;
	/* Set by the thread before fire. */
	atomic_bool fired;
};

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void* run(void* data)
{
	struct rdp_deadline* self = data;
	uint64_t end = now_us() + self->timeout_us;
	for (;;) {
		uint64_t now = now_us();
		if (now >= end)
			break;
		/* Rounded up, so it never wakes before the time. */
		uint64_t ms = (end - now + 999) / 1000;
		struct pollfd pfd = { .fd = self->pipe[0], .events = POLLIN };
		int rc = poll(&pfd, 1, ms > 1000000 ? 1000000 : (int)ms);
		if (rc > 0)
			return NULL;
		if (rc < 0 && errno != EINTR)
			return NULL;
	}
	atomic_store(&self->fired, true);
	self->fire(self->arg);
	return NULL;
}

struct rdp_deadline* rdp_deadline_start(uint64_t timeout_us,
		void (*fire)(void* arg), void* arg)
{
	struct rdp_deadline* self = calloc(1, sizeof(*self));
	if (!self)
		return NULL;
	self->timeout_us = timeout_us;
	self->fire = fire;
	self->arg = arg;
	if (pipe2(self->pipe, O_CLOEXEC) < 0) {
		free(self);
		return NULL;
	}
	if (pthread_create(&self->thread, NULL, run, self) != 0) {
		close(self->pipe[0]);
		close(self->pipe[1]);
		free(self);
		return NULL;
	}
	return self;
}

bool rdp_deadline_fired(const struct rdp_deadline* self)
{
	return self && atomic_load(&((struct rdp_deadline*)self)->fired);
}

bool rdp_deadline_stop(struct rdp_deadline* self)
{
	if (!self)
		return false;
	char c = 0;
	while (write(self->pipe[1], &c, 1) < 0 && errno == EINTR)
		;
	pthread_join(self->thread, NULL);
	bool fired = atomic_load(&self->fired);
	close(self->pipe[0]);
	close(self->pipe[1]);
	free(self);
	return fired;
}
