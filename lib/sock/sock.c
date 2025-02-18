/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020 Mellanox Technologies LTD. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/sock.h"
#include "spdk_internal/sock.h"
#include "spdk/log.h"

#define SPDK_SOCK_DEFAULT_PRIORITY 0
#define SPDK_SOCK_DEFAULT_ZCOPY true
#define SPDK_SOCK_DEFAULT_CTX NULL
#define SPDK_SOCK_OPTS_FIELD_OK(opts, field) (offsetof(struct spdk_sock_opts, field) + sizeof(opts->field) <= (opts->opts_size))

static STAILQ_HEAD(, spdk_net_impl) g_net_impls = STAILQ_HEAD_INITIALIZER(g_net_impls);
static struct spdk_net_impl *g_default_impl;

struct spdk_sock_placement_id_entry {
	int placement_id;
	uint32_t ref;
	struct spdk_sock_group *group;
	STAILQ_ENTRY(spdk_sock_placement_id_entry) link;
};

static STAILQ_HEAD(, spdk_sock_placement_id_entry) g_placement_id_map = STAILQ_HEAD_INITIALIZER(
			g_placement_id_map);
static pthread_mutex_t g_map_table_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Insert a group into the placement map.
 * If the group is already in the map, take a reference.
 */
static int
sock_map_insert(int placement_id, struct spdk_sock_group *group)
{
	struct spdk_sock_placement_id_entry *entry;

	pthread_mutex_lock(&g_map_table_mutex);
	STAILQ_FOREACH(entry, &g_placement_id_map, link) {
		if (placement_id == entry->placement_id) {
			/* The mapping already exists, it means that different sockets have
			 * the same placement_ids.
			 */
			entry->ref++;
			pthread_mutex_unlock(&g_map_table_mutex);
			return 0;
		}
	}

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		SPDK_ERRLOG("Cannot allocate an entry for placement_id=%u\n", placement_id);
		pthread_mutex_unlock(&g_map_table_mutex);
		return -ENOMEM;
	}

	entry->placement_id = placement_id;
	entry->group = group;
	entry->ref++;

	STAILQ_INSERT_TAIL(&g_placement_id_map, entry, link);
	pthread_mutex_unlock(&g_map_table_mutex);

	return 0;
}

/* Release a reference to the group for a given placement_id.
 * If the reference count is 0, remove the group.
 */
static void
sock_map_release(int placement_id)
{
	struct spdk_sock_placement_id_entry *entry;

	pthread_mutex_lock(&g_map_table_mutex);
	STAILQ_FOREACH(entry, &g_placement_id_map, link) {
		if (placement_id == entry->placement_id) {
			assert(entry->ref > 0);
			entry->ref--;
			break;
		}
	}

	pthread_mutex_unlock(&g_map_table_mutex);
}

/* Look up the group for a placement_id. */
static void
sock_map_lookup(int placement_id, struct spdk_sock_group **group)
{
	struct spdk_sock_placement_id_entry *entry;

	*group = NULL;
	pthread_mutex_lock(&g_map_table_mutex);
	STAILQ_FOREACH(entry, &g_placement_id_map, link) {
		if (placement_id == entry->placement_id) {
			assert(entry->group != NULL);
			*group = entry->group;
			break;
		}
	}
	pthread_mutex_unlock(&g_map_table_mutex);
}

/* Remove the socket group from the map table */
static void
sock_remove_sock_group_from_map_table(struct spdk_sock_group *group)
{
	struct spdk_sock_placement_id_entry *entry, *tmp;

	pthread_mutex_lock(&g_map_table_mutex);
	STAILQ_FOREACH_SAFE(entry, &g_placement_id_map, link, tmp) {
		if (entry->group == group) {
			STAILQ_REMOVE(&g_placement_id_map, entry, spdk_sock_placement_id_entry, link);
			free(entry);
		}
	}
	pthread_mutex_unlock(&g_map_table_mutex);

}

static int
sock_get_placement_id(struct spdk_sock *sock)
{
	int rc;
	int placement_id;

	if (!sock->placement_id) {
		rc = sock->net_impl->get_placement_id(sock, &placement_id);
		if (!rc && (placement_id != 0)) {
			sock->placement_id = placement_id;
		}
	}

	return sock->placement_id;
}

int
spdk_sock_get_optimal_sock_group(struct spdk_sock *sock, struct spdk_sock_group **group)
{
	int placement_id;

	placement_id = sock_get_placement_id(sock);
	if (placement_id != 0) {
		sock_map_lookup(placement_id, group);
		return 0;
	} else {
		return -1;
	}
}

int
spdk_sock_getaddr(struct spdk_sock *sock, char *saddr, int slen, uint16_t *sport,
		  char *caddr, int clen, uint16_t *cport)
{
	return sock->net_impl->getaddr(sock, saddr, slen, sport, caddr, clen, cport);
}

void
spdk_sock_get_default_opts(struct spdk_sock_opts *opts)
{
	assert(opts);

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, priority)) {
		opts->priority = SPDK_SOCK_DEFAULT_PRIORITY;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, zcopy)) {
		opts->zcopy = SPDK_SOCK_DEFAULT_ZCOPY;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, ctx)) {
		opts->ctx = SPDK_SOCK_DEFAULT_CTX;
	}
}

/*
 * opts The opts allocated in the current library.
 * opts_user The opts passed by the caller.
 * */
static void
sock_init_opts(struct spdk_sock_opts *opts, struct spdk_sock_opts *opts_user)
{
	assert(opts);
	assert(opts_user);

	opts->opts_size = sizeof(*opts);
	spdk_sock_get_default_opts(opts);

	/* reset the size according to the user */
	opts->opts_size = opts_user->opts_size;
	if (SPDK_SOCK_OPTS_FIELD_OK(opts, priority)) {
		opts->priority = opts_user->priority;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, zcopy)) {
		opts->zcopy = opts_user->zcopy;
	}

	if (SPDK_SOCK_OPTS_FIELD_OK(opts, ctx)) {
		opts->ctx = opts_user->ctx;
	}
}

struct spdk_sock *
spdk_sock_connect(const char *ip, int port, char *impl_name)
{
	struct spdk_sock_opts opts;

	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	return spdk_sock_connect_ext(ip, port, impl_name, &opts);
}

struct spdk_sock *
spdk_sock_connect_ext(const char *ip, int port, char *_impl_name, struct spdk_sock_opts *opts)
{
	struct spdk_net_impl *impl = NULL;
	struct spdk_sock *sock;
	struct spdk_sock_opts opts_local;
	const char *impl_name = NULL;

	if (opts == NULL) {
		SPDK_ERRLOG("the opts should not be NULL pointer\n");
		return NULL;
	}

	if (_impl_name) {
		impl_name = _impl_name;
	} else if (g_default_impl) {
		impl_name = g_default_impl->name;
	}

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {
		if (impl_name && strncmp(impl_name, impl->name, strlen(impl->name) + 1)) {
			continue;
		}

		SPDK_DEBUGLOG(sock, "Creating a client socket using impl %s\n", impl->name);
		sock_init_opts(&opts_local, opts);
		sock = impl->connect(ip, port, &opts_local);
		if (sock != NULL) {
			/* Copy the contents, both the two structures are the same ABI version */
			memcpy(&sock->opts, &opts_local, sizeof(sock->opts));
			sock->net_impl = impl;
			TAILQ_INIT(&sock->queued_reqs);
			TAILQ_INIT(&sock->pending_reqs);
			return sock;
		}
	}

	return NULL;
}

struct spdk_sock *
spdk_sock_listen(const char *ip, int port, char *impl_name)
{
	struct spdk_sock_opts opts;

	opts.opts_size = sizeof(opts);
	spdk_sock_get_default_opts(&opts);
	return spdk_sock_listen_ext(ip, port, impl_name, &opts);
}

struct spdk_sock *
spdk_sock_listen_ext(const char *ip, int port, char *_impl_name, struct spdk_sock_opts *opts)
{
	struct spdk_net_impl *impl = NULL;
	struct spdk_sock *sock;
	struct spdk_sock_opts opts_local;
	const char *impl_name = NULL;

	if (opts == NULL) {
		SPDK_ERRLOG("the opts should not be NULL pointer\n");
		return NULL;
	}

	if (_impl_name) {
		impl_name = _impl_name;
	} else if (g_default_impl) {
		impl_name = g_default_impl->name;
	}

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {
		if (impl_name && strncmp(impl_name, impl->name, strlen(impl->name) + 1)) {
			continue;
		}

		SPDK_DEBUGLOG(sock, "Creating a listening socket using impl %s\n", impl->name);
		sock_init_opts(&opts_local, opts);
		sock = impl->listen(ip, port, &opts_local);
		if (sock != NULL) {
			/* Copy the contents, both the two structures are the same ABI version */
			memcpy(&sock->opts, &opts_local, sizeof(sock->opts));
			sock->net_impl = impl;
			/* Don't need to initialize the request queues for listen
			 * sockets. */
			return sock;
		}
	}

	return NULL;
}

struct spdk_sock *
spdk_sock_accept(struct spdk_sock *sock)
{
	struct spdk_sock *new_sock;

	new_sock = sock->net_impl->accept(sock);
	if (new_sock != NULL) {
		/* Inherit the opts from the "accept sock" */
		new_sock->opts = sock->opts;
		memcpy(&new_sock->opts, &sock->opts, sizeof(new_sock->opts));
		new_sock->net_impl = sock->net_impl;
		TAILQ_INIT(&new_sock->queued_reqs);
		TAILQ_INIT(&new_sock->pending_reqs);
	}

	return new_sock;
}

int
spdk_sock_close(struct spdk_sock **_sock)
{
	struct spdk_sock *sock = *_sock;
	int rc;

	if (sock == NULL) {
		errno = EBADF;
		return -1;
	}

	if (sock->cb_fn != NULL) {
		/* This sock is still part of a sock_group. */
		errno = EBUSY;
		return -1;
	}

	sock->flags.closed = true;

	if (sock->cb_cnt > 0) {
		/* Let the callback unwind before destroying the socket */
		return 0;
	}

	spdk_sock_abort_requests(sock);

	rc = sock->net_impl->close(sock);
	if (rc == 0) {
		*_sock = NULL;
	}

	return rc;
}

ssize_t
spdk_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	if (sock == NULL) {
		errno = EBADF;
		return -1;
	}

	if (sock->flags.closed) {
		errno = EBADF;
		return -1;
	}

	return sock->net_impl->recv(sock, buf, len);
}

ssize_t
spdk_sock_readv(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	if (sock == NULL) {
		errno = EBADF;
		return -1;
	}

	if (sock->flags.closed) {
		errno = EBADF;
		return -1;
	}

	return sock->net_impl->readv(sock, iov, iovcnt);
}

ssize_t
spdk_sock_writev(struct spdk_sock *sock, struct iovec *iov, int iovcnt)
{
	if (sock == NULL) {
		errno = EBADF;
		return -1;
	}

	if (sock->flags.closed) {
		errno = EBADF;
		return -1;
	}

	return sock->net_impl->writev(sock, iov, iovcnt);
}

void
spdk_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	assert(req->cb_fn != NULL);

	if (sock == NULL) {
		req->cb_fn(req->cb_arg, -EBADF);
		return;
	}

	if (sock->flags.closed) {
		req->cb_fn(req->cb_arg, -EBADF);
		return;
	}

	sock->net_impl->writev_async(sock, req);
}

int
spdk_sock_flush(struct spdk_sock *sock)
{
	if (sock == NULL) {
		return -EBADF;
	}

	if (sock->flags.closed) {
		return -EBADF;
	}

	return sock->net_impl->flush(sock);
}

int
spdk_sock_set_recvlowat(struct spdk_sock *sock, int nbytes)
{
	return sock->net_impl->set_recvlowat(sock, nbytes);
}

int
spdk_sock_set_recvbuf(struct spdk_sock *sock, int sz)
{
	return sock->net_impl->set_recvbuf(sock, sz);
}

int
spdk_sock_set_sendbuf(struct spdk_sock *sock, int sz)
{
	return sock->net_impl->set_sendbuf(sock, sz);
}

bool
spdk_sock_is_ipv6(struct spdk_sock *sock)
{
	return sock->net_impl->is_ipv6(sock);
}

bool
spdk_sock_is_ipv4(struct spdk_sock *sock)
{
	return sock->net_impl->is_ipv4(sock);
}

bool
spdk_sock_is_connected(struct spdk_sock *sock)
{
	return sock->net_impl->is_connected(sock);
}

struct spdk_sock_group *
spdk_sock_group_create(void *ctx)
{
	struct spdk_net_impl *impl = NULL;
	struct spdk_sock_group *group;
	struct spdk_sock_group_impl *group_impl;

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		return NULL;
	}

	STAILQ_INIT(&group->group_impls);

	STAILQ_FOREACH_FROM(impl, &g_net_impls, link) {
		group_impl = impl->group_impl_create();
		if (group_impl != NULL) {
			STAILQ_INSERT_TAIL(&group->group_impls, group_impl, link);
			TAILQ_INIT(&group_impl->socks);
			group_impl->num_removed_socks = 0;
			group_impl->net_impl = impl;
		}
	}

	group->ctx = ctx;
	return group;
}

void *
spdk_sock_group_get_ctx(struct spdk_sock_group *group)
{
	if (group == NULL) {
		return NULL;
	}

	return group->ctx;
}

int
spdk_sock_group_add_sock(struct spdk_sock_group *group, struct spdk_sock *sock,
			 spdk_sock_cb cb_fn, void *cb_arg)
{
	struct spdk_sock_group_impl *group_impl = NULL;
	int rc, placement_id = 0;

	if (cb_fn == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (sock->group_impl != NULL) {
		/*
		 * This sock is already part of a sock_group.  Currently we don't
		 *  support this.
		 */
		errno = EBUSY;
		return -1;
	}

	placement_id = sock_get_placement_id(sock);
	if (placement_id != 0) {
		rc = sock_map_insert(placement_id, group);
		if (rc < 0) {
			return -1;
		}
	}

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) {
		if (sock->net_impl == group_impl->net_impl) {
			break;
		}
	}

	if (group_impl == NULL) {
		errno = EINVAL;
		return -1;
	}

	rc = group_impl->net_impl->group_impl_add_sock(group_impl, sock);
	if (rc == 0) {
		TAILQ_INSERT_TAIL(&group_impl->socks, sock, link);
		sock->group_impl = group_impl;
		sock->cb_fn = cb_fn;
		sock->cb_arg = cb_arg;
	}

	return rc;
}

int
spdk_sock_group_remove_sock(struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_sock_group_impl *group_impl = NULL;
	int rc, placement_id = 0;

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) {
		if (sock->net_impl == group_impl->net_impl) {
			break;
		}
	}

	if (group_impl == NULL) {
		errno = EINVAL;
		return -1;
	}

	assert(group_impl == sock->group_impl);

	placement_id = sock_get_placement_id(sock);
	if (placement_id != 0) {
		sock_map_release(placement_id);
	}

	rc = group_impl->net_impl->group_impl_remove_sock(group_impl, sock);
	if (rc == 0) {
		TAILQ_REMOVE(&group_impl->socks, sock, link);
		assert(group_impl->num_removed_socks < MAX_EVENTS_PER_POLL);
		group_impl->removed_socks[group_impl->num_removed_socks] = (uintptr_t)sock;
		group_impl->num_removed_socks++;
		sock->group_impl = NULL;
		sock->cb_fn = NULL;
		sock->cb_arg = NULL;
	}

	return rc;
}

int
spdk_sock_group_poll(struct spdk_sock_group *group)
{
	return spdk_sock_group_poll_count(group, MAX_EVENTS_PER_POLL);
}

static int
sock_group_impl_poll_count(struct spdk_sock_group_impl *group_impl,
			   struct spdk_sock_group *group,
			   int max_events)
{
	struct spdk_sock *socks[MAX_EVENTS_PER_POLL];
	int num_events, i;

	if (TAILQ_EMPTY(&group_impl->socks)) {
		return 0;
	}

	/* The number of removed sockets should be reset for each call to poll. */
	group_impl->num_removed_socks = 0;

	num_events = group_impl->net_impl->group_impl_poll(group_impl, max_events, socks);
	if (num_events == -1) {
		return -1;
	}

	for (i = 0; i < num_events; i++) {
		struct spdk_sock *sock = socks[i];
		int j;
		bool valid = true;
		for (j = 0; j < group_impl->num_removed_socks; j++) {
			if ((uintptr_t)sock == group_impl->removed_socks[j]) {
				valid = false;
				break;
			}
		}

		if (valid) {
			assert(sock->cb_fn != NULL);
			sock->cb_fn(sock->cb_arg, group, sock);
		}
	}

	return num_events;
}

int
spdk_sock_group_poll_count(struct spdk_sock_group *group, int max_events)
{
	struct spdk_sock_group_impl *group_impl = NULL;
	int rc, num_events = 0;

	if (max_events < 1) {
		errno = -EINVAL;
		return -1;
	}

	/*
	 * Only poll for up to 32 events at a time - if more events are pending,
	 *  the next call to this function will reap them.
	 */
	if (max_events > MAX_EVENTS_PER_POLL) {
		max_events = MAX_EVENTS_PER_POLL;
	}

	STAILQ_FOREACH_FROM(group_impl, &group->group_impls, link) {
		rc = sock_group_impl_poll_count(group_impl, group, max_events);
		if (rc < 0) {
			num_events = -1;
			SPDK_ERRLOG("group_impl_poll_count for net(%s) failed\n",
				    group_impl->net_impl->name);
		} else if (num_events >= 0) {
			num_events += rc;
		}
	}

	return num_events;
}

int
spdk_sock_group_close(struct spdk_sock_group **group)
{
	struct spdk_sock_group_impl *group_impl = NULL, *tmp;
	int rc;

	if (*group == NULL) {
		errno = EBADF;
		return -1;
	}

	STAILQ_FOREACH_SAFE(group_impl, &(*group)->group_impls, link, tmp) {
		if (!TAILQ_EMPTY(&group_impl->socks)) {
			errno = EBUSY;
			return -1;
		}
	}

	STAILQ_FOREACH_SAFE(group_impl, &(*group)->group_impls, link, tmp) {
		rc = group_impl->net_impl->group_impl_close(group_impl);
		if (rc != 0) {
			SPDK_ERRLOG("group_impl_close for net(%s) failed\n",
				    group_impl->net_impl->name);
		}
	}

	sock_remove_sock_group_from_map_table(*group);
	free(*group);
	*group = NULL;

	return 0;
}

static inline struct spdk_net_impl *
sock_get_impl_by_name(const char *impl_name)
{
	struct spdk_net_impl *impl;

	assert(impl_name != NULL);
	STAILQ_FOREACH(impl, &g_net_impls, link) {
		if (0 == strcmp(impl_name, impl->name)) {
			return impl;
		}
	}

	return NULL;
}

int
spdk_sock_impl_get_opts(const char *impl_name, struct spdk_sock_impl_opts *opts, size_t *len)
{
	struct spdk_net_impl *impl;

	if (!impl_name || !opts || !len) {
		errno = EINVAL;
		return -1;
	}

	impl = sock_get_impl_by_name(impl_name);
	if (!impl) {
		errno = EINVAL;
		return -1;
	}

	if (!impl->get_opts) {
		errno = ENOTSUP;
		return -1;
	}

	return impl->get_opts(opts, len);
}

int
spdk_sock_impl_set_opts(const char *impl_name, const struct spdk_sock_impl_opts *opts, size_t len)
{
	struct spdk_net_impl *impl;

	if (!impl_name || !opts) {
		errno = EINVAL;
		return -1;
	}

	impl = sock_get_impl_by_name(impl_name);
	if (!impl) {
		errno = EINVAL;
		return -1;
	}

	if (!impl->set_opts) {
		errno = ENOTSUP;
		return -1;
	}

	return impl->set_opts(opts, len);
}

void
spdk_sock_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_net_impl *impl;
	struct spdk_sock_impl_opts opts;
	size_t len;

	assert(w != NULL);

	spdk_json_write_array_begin(w);

	if (g_default_impl) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "sock_set_default_impl");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "impl_name", g_default_impl->name);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}

	STAILQ_FOREACH(impl, &g_net_impls, link) {
		if (!impl->get_opts) {
			continue;
		}

		len = sizeof(opts);
		if (impl->get_opts(&opts, &len) == 0) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_string(w, "method", "sock_impl_set_options");
			spdk_json_write_named_object_begin(w, "params");
			spdk_json_write_named_string(w, "impl_name", impl->name);
			spdk_json_write_named_uint32(w, "recv_buf_size", opts.recv_buf_size);
			spdk_json_write_named_uint32(w, "send_buf_size", opts.send_buf_size);
			spdk_json_write_named_bool(w, "enable_recv_pipe", opts.enable_recv_pipe);
			spdk_json_write_named_bool(w, "enable_zerocopy_send", opts.enable_zerocopy_send);
			spdk_json_write_object_end(w);
			spdk_json_write_object_end(w);
		} else {
			SPDK_ERRLOG("Failed to get socket options for socket implementation %s\n", impl->name);
		}
	}

	spdk_json_write_array_end(w);
}

void
spdk_net_impl_register(struct spdk_net_impl *impl, int priority)
{
	struct spdk_net_impl *cur, *prev;

	impl->priority = priority;
	prev = NULL;
	STAILQ_FOREACH(cur, &g_net_impls, link) {
		if (impl->priority > cur->priority) {
			break;
		}
		prev = cur;
	}

	if (prev) {
		STAILQ_INSERT_AFTER(&g_net_impls, prev, impl, link);
	} else {
		STAILQ_INSERT_HEAD(&g_net_impls, impl, link);
	}
}

int spdk_sock_set_default_impl(const char *impl_name)
{
	struct spdk_net_impl *impl;

	if (!impl_name) {
		errno = EINVAL;
		return -1;
	}

	impl = sock_get_impl_by_name(impl_name);
	if (!impl) {
		errno = EINVAL;
		return -1;
	}

	if (impl == g_default_impl) {
		return 0;
	}

	if (g_default_impl) {
		SPDK_DEBUGLOG(sock, "Change the default sock impl from %s to %s\n", g_default_impl->name,
			      impl->name);
	} else {
		SPDK_DEBUGLOG(sock, "Set default sock implementation to %s\n", impl_name);
	}

	g_default_impl = impl;

	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(sock)
