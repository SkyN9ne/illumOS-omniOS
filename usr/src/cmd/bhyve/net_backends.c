/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2019 Vincenzo Maffione <vmaffione@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD$
 */

/*
 * This file implements multiple network backends (tap, netmap, ...),
 * to be used by network frontends such as virtio-net and e1000.
 * The API to access the backend (e.g. send/receive packets, negotiate
 * features) is exported by net_backends.h.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>		/* u_short etc */
#ifndef WITHOUT_CAPSICUM
#include <sys/capsicum.h>
#endif
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <net/if.h>
#ifdef __FreeBSD__
#if defined(INET6) || defined(INET)
#include <net/if_tap.h>
#endif
#include <net/netmap.h>
#include <net/netmap_virt.h>
#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>
#endif /* __FreeBSD__ */

#ifndef WITHOUT_CAPSICUM
#include <capsicum_helpers.h>
#endif
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sysexits.h>
#include <assert.h>
#include <pthread.h>
#include <pthread_np.h>
#include <poll.h>
#include <assert.h>

#ifdef NETGRAPH
#include <sys/param.h>
#include <sys/sysctl.h>
#include <netgraph.h>
#endif

#ifndef __FreeBSD__
#include <libdlpi.h>
#include <net/ethernet.h>
#endif

#include "config.h"
#include "debug.h"
#include "iov.h"
#include "mevent.h"
#include "net_backends.h"
#include "pci_emul.h"

#include <sys/linker_set.h>

/*
 * Each network backend registers a set of function pointers that are
 * used to implement the net backends API.
 * This might need to be exposed if we implement backends in separate files.
 */
struct net_backend {
	const char *prefix;	/* prefix matching this backend */

	/*
	 * Routines used to initialize and cleanup the resources needed
	 * by a backend. The cleanup function is used internally,
	 * and should not be called by the frontend.
	 */
	int (*init)(struct net_backend *be, const char *devname,
	    nvlist_t *nvl, net_be_rxeof_t cb, void *param);
	void (*cleanup)(struct net_backend *be);

	/*
	 * Called to serve a guest transmit request. The scatter-gather
	 * vector provided by the caller has 'iovcnt' elements and contains
	 * the packet to send.
	 */
	ssize_t (*send)(struct net_backend *be, const struct iovec *iov,
	    int iovcnt);

	/*
	 * Get the length of the next packet that can be received from
	 * the backend. If no packets are currently available, this
	 * function returns 0.
	 */
	ssize_t (*peek_recvlen)(struct net_backend *be);

	/*
	 * Called to receive a packet from the backend. When the function
	 * returns a positive value 'len', the scatter-gather vector
	 * provided by the caller contains a packet with such length.
	 * The function returns 0 if the backend doesn't have a new packet to
	 * receive.
	 */
	ssize_t (*recv)(struct net_backend *be, const struct iovec *iov,
	    int iovcnt);

	/*
	 * Ask the backend to enable or disable receive operation in the
	 * backend. On return from a disable operation, it is guaranteed
	 * that the receive callback won't be called until receive is
	 * enabled again. Note however that it is up to the caller to make
	 * sure that netbe_recv() is not currently being executed by another
	 * thread.
	 */
	void (*recv_enable)(struct net_backend *be);
	void (*recv_disable)(struct net_backend *be);

	/*
	 * Ask the backend for the virtio-net features it is able to
	 * support. Possible features are TSO, UFO and checksum offloading
	 * in both rx and tx direction and for both IPv4 and IPv6.
	 */
	uint64_t (*get_cap)(struct net_backend *be);

	/*
	 * Tell the backend to enable/disable the specified virtio-net
	 * features (capabilities).
	 */
	int (*set_cap)(struct net_backend *be, uint64_t features,
	    unsigned int vnet_hdr_len);

#ifndef __FreeBSD__
	int (*get_mac)(struct net_backend *be, void *, size_t *);
#endif

	struct pci_vtnet_softc *sc;
	int fd;

	/*
	 * Length of the virtio-net header used by the backend and the
	 * frontend, respectively. A zero value means that the header
	 * is not used.
	 */
	unsigned int be_vnet_hdr_len;
	unsigned int fe_vnet_hdr_len;

	/* Size of backend-specific private data. */
	size_t priv_size;

	/* Room for backend-specific data. */
	char opaque[0];
};

SET_DECLARE(net_backend_set, struct net_backend);

#define VNET_HDR_LEN	sizeof(struct virtio_net_rxhdr)

#define WPRINTF(params) PRINTLN params

#ifdef __FreeBSD__

/*
 * The tap backend
 */

#if defined(INET6) || defined(INET)
const int pf_list[] = {
#if defined(INET6)
	PF_INET6,
#endif
#if defined(INET)
	PF_INET,
#endif
};
#endif

struct tap_priv {
	struct mevent *mevp;
	/*
	 * A bounce buffer that allows us to implement the peek_recvlen
	 * callback. In the future we may get the same information from
	 * the kevent data.
	 */
	char bbuf[1 << 16];
	ssize_t bbuflen;
};

static void
tap_cleanup(struct net_backend *be)
{
	struct tap_priv *priv = (struct tap_priv *)be->opaque;

	if (priv->mevp) {
		mevent_delete(priv->mevp);
	}
	if (be->fd != -1) {
		close(be->fd);
		be->fd = -1;
	}
}

static int
tap_init(struct net_backend *be, const char *devname,
	 nvlist_t *nvl, net_be_rxeof_t cb, void *param)
{
	struct tap_priv *priv = (struct tap_priv *)be->opaque;
	char tbuf[80];
	int opt = 1;
#if defined(INET6) || defined(INET)
	struct ifreq ifrq;
	int i, s;
#endif
#ifndef WITHOUT_CAPSICUM
	cap_rights_t rights;
#endif

	if (cb == NULL) {
		WPRINTF(("TAP backend requires non-NULL callback"));
		return (-1);
	}

	strcpy(tbuf, "/dev/");
	strlcat(tbuf, devname, sizeof(tbuf));

	be->fd = open(tbuf, O_RDWR);
	if (be->fd == -1) {
		WPRINTF(("open of tap device %s failed", tbuf));
		goto error;
	}

	/*
	 * Set non-blocking and register for read
	 * notifications with the event loop
	 */
	if (ioctl(be->fd, FIONBIO, &opt) < 0) {
		WPRINTF(("tap device O_NONBLOCK failed"));
		goto error;
	}

#if defined(INET6) || defined(INET)
	/*
	 * Try to UP the interface rather than relying on
	 * net.link.tap.up_on_open.
	  */
	bzero(&ifrq, sizeof(ifrq));
	if (ioctl(be->fd, TAPGIFNAME, &ifrq) < 0) {
		WPRINTF(("Could not get interface name"));
		goto error;
	}

	s = -1;
	for (i = 0; s == -1 && i < nitems(pf_list); i++)
		s = socket(pf_list[i], SOCK_DGRAM, 0);
	if (s == -1) {
		WPRINTF(("Could open socket"));
		goto error;
	}

	if (ioctl(s, SIOCGIFFLAGS, &ifrq) < 0) {
		(void)close(s);
		WPRINTF(("Could not get interface flags"));
		goto error;
	}
	ifrq.ifr_flags |= IFF_UP;
	if (ioctl(s, SIOCSIFFLAGS, &ifrq) < 0) {
		(void)close(s);
		WPRINTF(("Could not set interface flags"));
		goto error;
	}
	(void)close(s);
#endif

#ifndef WITHOUT_CAPSICUM
	cap_rights_init(&rights, CAP_EVENT, CAP_READ, CAP_WRITE);
	if (caph_rights_limit(be->fd, &rights) == -1)
		errx(EX_OSERR, "Unable to apply rights for sandbox");
#endif

	memset(priv->bbuf, 0, sizeof(priv->bbuf));
	priv->bbuflen = 0;

	priv->mevp = mevent_add_disabled(be->fd, EVF_READ, cb, param);
	if (priv->mevp == NULL) {
		WPRINTF(("Could not register event"));
		goto error;
	}

	return (0);

error:
	tap_cleanup(be);
	return (-1);
}

/*
 * Called to send a buffer chain out to the tap device
 */
static ssize_t
tap_send(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	return (writev(be->fd, iov, iovcnt));
}

static ssize_t
tap_peek_recvlen(struct net_backend *be)
{
	struct tap_priv *priv = (struct tap_priv *)be->opaque;
	ssize_t ret;

	if (priv->bbuflen > 0) {
		/*
		 * We already have a packet in the bounce buffer.
		 * Just return its length.
		 */
		return priv->bbuflen;
	}

	/*
	 * Read the next packet (if any) into the bounce buffer, so
	 * that we get to know its length and we can return that
	 * to the caller.
	 */
	ret = read(be->fd, priv->bbuf, sizeof(priv->bbuf));
	if (ret < 0 && errno == EWOULDBLOCK) {
		return (0);
	}

	if (ret > 0)
		priv->bbuflen = ret;

	return (ret);
}

static ssize_t
tap_recv(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	struct tap_priv *priv = (struct tap_priv *)be->opaque;
	ssize_t ret;

	if (priv->bbuflen > 0) {
		/*
		 * A packet is available in the bounce buffer, so
		 * we read it from there.
		 */
		ret = buf_to_iov(priv->bbuf, priv->bbuflen,
		    iov, iovcnt, 0);

		/* Mark the bounce buffer as empty. */
		priv->bbuflen = 0;

		return (ret);
	}

	ret = readv(be->fd, iov, iovcnt);
	if (ret < 0 && errno == EWOULDBLOCK) {
		return (0);
	}

	return (ret);
}

static void
tap_recv_enable(struct net_backend *be)
{
	struct tap_priv *priv = (struct tap_priv *)be->opaque;

	mevent_enable(priv->mevp);
}

static void
tap_recv_disable(struct net_backend *be)
{
	struct tap_priv *priv = (struct tap_priv *)be->opaque;

	mevent_disable(priv->mevp);
}

static uint64_t
tap_get_cap(struct net_backend *be)
{

	return (0); /* no capabilities for now */
}

static int
tap_set_cap(struct net_backend *be, uint64_t features,
		unsigned vnet_hdr_len)
{

	return ((features || vnet_hdr_len) ? -1 : 0);
}

static struct net_backend tap_backend = {
	.prefix = "tap",
	.priv_size = sizeof(struct tap_priv),
	.init = tap_init,
	.cleanup = tap_cleanup,
	.send = tap_send,
	.peek_recvlen = tap_peek_recvlen,
	.recv = tap_recv,
	.recv_enable = tap_recv_enable,
	.recv_disable = tap_recv_disable,
	.get_cap = tap_get_cap,
	.set_cap = tap_set_cap,
};

/* A clone of the tap backend, with a different prefix. */
static struct net_backend vmnet_backend = {
	.prefix = "vmnet",
	.priv_size = sizeof(struct tap_priv),
	.init = tap_init,
	.cleanup = tap_cleanup,
	.send = tap_send,
	.peek_recvlen = tap_peek_recvlen,
	.recv = tap_recv,
	.recv_enable = tap_recv_enable,
	.recv_disable = tap_recv_disable,
	.get_cap = tap_get_cap,
	.set_cap = tap_set_cap,
};

DATA_SET(net_backend_set, tap_backend);
DATA_SET(net_backend_set, vmnet_backend);

#ifdef NETGRAPH

/*
 * Netgraph backend
 */

#define NG_SBUF_MAX_SIZE (4 * 1024 * 1024)

static int
ng_init(struct net_backend *be, const char *devname,
	 nvlist_t *nvl, net_be_rxeof_t cb, void *param)
{
	struct tap_priv *p = (struct tap_priv *)be->opaque;
	struct ngm_connect ngc;
	const char *value, *nodename;
	int sbsz;
	int ctrl_sock;
	int flags;
	unsigned long maxsbsz;
	size_t msbsz;
#ifndef WITHOUT_CAPSICUM
	cap_rights_t rights;
#endif

	if (cb == NULL) {
		WPRINTF(("Netgraph backend requires non-NULL callback"));
		return (-1);
	}

	be->fd = -1;

	memset(&ngc, 0, sizeof(ngc));

	value = get_config_value_node(nvl, "path");
	if (value == NULL) {
		WPRINTF(("path must be provided"));
		return (-1);
	}
	strncpy(ngc.path, value, NG_PATHSIZ - 1);

	value = get_config_value_node(nvl, "hook");
	if (value == NULL)
		value = "vmlink";
	strncpy(ngc.ourhook, value, NG_HOOKSIZ - 1);

	value = get_config_value_node(nvl, "peerhook");
	if (value == NULL) {
		WPRINTF(("peer hook must be provided"));
		return (-1);
	}
	strncpy(ngc.peerhook, value, NG_HOOKSIZ - 1);

	nodename = get_config_value_node(nvl, "socket");
	if (NgMkSockNode(nodename,
		&ctrl_sock, &be->fd) < 0) {
		WPRINTF(("can't get Netgraph sockets"));
		return (-1);
	}

	if (NgSendMsg(ctrl_sock, ".",
		NGM_GENERIC_COOKIE,
		NGM_CONNECT, &ngc, sizeof(ngc)) < 0) {
		WPRINTF(("can't connect to node"));
		close(ctrl_sock);
		goto error;
	}

	close(ctrl_sock);

	flags = fcntl(be->fd, F_GETFL);

	if (flags < 0) {
		WPRINTF(("can't get socket flags"));
		goto error;
	}

	if (fcntl(be->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		WPRINTF(("can't set O_NONBLOCK flag"));
		goto error;
	}

	/*
	 * The default ng_socket(4) buffer's size is too low.
	 * Calculate the minimum value between NG_SBUF_MAX_SIZE
	 * and kern.ipc.maxsockbuf.
	 */
	msbsz = sizeof(maxsbsz);
	if (sysctlbyname("kern.ipc.maxsockbuf", &maxsbsz, &msbsz,
		NULL, 0) < 0) {
		WPRINTF(("can't get 'kern.ipc.maxsockbuf' value"));
		goto error;
	}

	/*
	 * We can't set the socket buffer size to kern.ipc.maxsockbuf value,
	 * as it takes into account the mbuf(9) overhead.
	 */
	maxsbsz = maxsbsz * MCLBYTES / (MSIZE + MCLBYTES);

	sbsz = MIN(NG_SBUF_MAX_SIZE, maxsbsz);

	if (setsockopt(be->fd, SOL_SOCKET, SO_SNDBUF, &sbsz,
		sizeof(sbsz)) < 0) {
		WPRINTF(("can't set TX buffer size"));
		goto error;
	}

	if (setsockopt(be->fd, SOL_SOCKET, SO_RCVBUF, &sbsz,
		sizeof(sbsz)) < 0) {
		WPRINTF(("can't set RX buffer size"));
		goto error;
	}

#ifndef WITHOUT_CAPSICUM
	cap_rights_init(&rights, CAP_EVENT, CAP_READ, CAP_WRITE);
	if (caph_rights_limit(be->fd, &rights) == -1)
		errx(EX_OSERR, "Unable to apply rights for sandbox");
#endif

	memset(p->bbuf, 0, sizeof(p->bbuf));
	p->bbuflen = 0;

	p->mevp = mevent_add_disabled(be->fd, EVF_READ, cb, param);
	if (p->mevp == NULL) {
		WPRINTF(("Could not register event"));
		goto error;
	}

	return (0);

error:
	tap_cleanup(be);
	return (-1);
}

static struct net_backend ng_backend = {
	.prefix = "netgraph",
	.priv_size = sizeof(struct tap_priv),
	.init = ng_init,
	.cleanup = tap_cleanup,
	.send = tap_send,
	.peek_recvlen = tap_peek_recvlen,
	.recv = tap_recv,
	.recv_enable = tap_recv_enable,
	.recv_disable = tap_recv_disable,
	.get_cap = tap_get_cap,
	.set_cap = tap_set_cap,
};

DATA_SET(net_backend_set, ng_backend);

#endif /* NETGRAPH */

/*
 * The netmap backend
 */

/* The virtio-net features supported by netmap. */
#define NETMAP_FEATURES (VIRTIO_NET_F_CSUM | VIRTIO_NET_F_HOST_TSO4 | \
		VIRTIO_NET_F_HOST_TSO6 | VIRTIO_NET_F_HOST_UFO | \
		VIRTIO_NET_F_GUEST_CSUM | VIRTIO_NET_F_GUEST_TSO4 | \
		VIRTIO_NET_F_GUEST_TSO6 | VIRTIO_NET_F_GUEST_UFO)

struct netmap_priv {
	char ifname[IFNAMSIZ];
	struct nm_desc *nmd;
	uint16_t memid;
	struct netmap_ring *rx;
	struct netmap_ring *tx;
	struct mevent *mevp;
	net_be_rxeof_t cb;
	void *cb_param;
};

static void
nmreq_init(struct nmreq *req, char *ifname)
{

	memset(req, 0, sizeof(*req));
	strlcpy(req->nr_name, ifname, sizeof(req->nr_name));
	req->nr_version = NETMAP_API;
}

static int
netmap_set_vnet_hdr_len(struct net_backend *be, int vnet_hdr_len)
{
	int err;
	struct nmreq req;
	struct netmap_priv *priv = (struct netmap_priv *)be->opaque;

	nmreq_init(&req, priv->ifname);
	req.nr_cmd = NETMAP_BDG_VNET_HDR;
	req.nr_arg1 = vnet_hdr_len;
	err = ioctl(be->fd, NIOCREGIF, &req);
	if (err) {
		WPRINTF(("Unable to set vnet header length %d",
				vnet_hdr_len));
		return (err);
	}

	be->be_vnet_hdr_len = vnet_hdr_len;

	return (0);
}

static int
netmap_has_vnet_hdr_len(struct net_backend *be, unsigned vnet_hdr_len)
{
	int prev_hdr_len = be->be_vnet_hdr_len;
	int ret;

	if (vnet_hdr_len == prev_hdr_len) {
		return (1);
	}

	ret = netmap_set_vnet_hdr_len(be, vnet_hdr_len);
	if (ret) {
		return (0);
	}

	netmap_set_vnet_hdr_len(be, prev_hdr_len);

	return (1);
}

static uint64_t
netmap_get_cap(struct net_backend *be)
{

	return (netmap_has_vnet_hdr_len(be, VNET_HDR_LEN) ?
	    NETMAP_FEATURES : 0);
}

static int
netmap_set_cap(struct net_backend *be, uint64_t features,
	       unsigned vnet_hdr_len)
{

	return (netmap_set_vnet_hdr_len(be, vnet_hdr_len));
}

static int
netmap_init(struct net_backend *be, const char *devname,
	    nvlist_t *nvl, net_be_rxeof_t cb, void *param)
{
	struct netmap_priv *priv = (struct netmap_priv *)be->opaque;

	strlcpy(priv->ifname, devname, sizeof(priv->ifname));
	priv->ifname[sizeof(priv->ifname) - 1] = '\0';

	priv->nmd = nm_open(priv->ifname, NULL, NETMAP_NO_TX_POLL, NULL);
	if (priv->nmd == NULL) {
		WPRINTF(("Unable to nm_open(): interface '%s', errno (%s)",
			devname, strerror(errno)));
		free(priv);
		return (-1);
	}

	priv->memid = priv->nmd->req.nr_arg2;
	priv->tx = NETMAP_TXRING(priv->nmd->nifp, 0);
	priv->rx = NETMAP_RXRING(priv->nmd->nifp, 0);
	priv->cb = cb;
	priv->cb_param = param;
	be->fd = priv->nmd->fd;

	priv->mevp = mevent_add_disabled(be->fd, EVF_READ, cb, param);
	if (priv->mevp == NULL) {
		WPRINTF(("Could not register event"));
		return (-1);
	}

	return (0);
}

static void
netmap_cleanup(struct net_backend *be)
{
	struct netmap_priv *priv = (struct netmap_priv *)be->opaque;

	if (priv->mevp) {
		mevent_delete(priv->mevp);
	}
	if (priv->nmd) {
		nm_close(priv->nmd);
	}
	be->fd = -1;
}

static ssize_t
netmap_send(struct net_backend *be, const struct iovec *iov,
	    int iovcnt)
{
	struct netmap_priv *priv = (struct netmap_priv *)be->opaque;
	struct netmap_ring *ring;
	ssize_t totlen = 0;
	int nm_buf_size;
	int nm_buf_len;
	uint32_t head;
	void *nm_buf;
	int j;

	ring = priv->tx;
	head = ring->head;
	if (head == ring->tail) {
		WPRINTF(("No space, drop %zu bytes", count_iov(iov, iovcnt)));
		goto txsync;
	}
	nm_buf = NETMAP_BUF(ring, ring->slot[head].buf_idx);
	nm_buf_size = ring->nr_buf_size;
	nm_buf_len = 0;

	for (j = 0; j < iovcnt; j++) {
		int iov_frag_size = iov[j].iov_len;
		void *iov_frag_buf = iov[j].iov_base;

		totlen += iov_frag_size;

		/*
		 * Split each iovec fragment over more netmap slots, if
		 * necessary.
		 */
		for (;;) {
			int copylen;

			copylen = iov_frag_size < nm_buf_size ? iov_frag_size : nm_buf_size;
			memcpy(nm_buf, iov_frag_buf, copylen);

			iov_frag_buf += copylen;
			iov_frag_size -= copylen;
			nm_buf += copylen;
			nm_buf_size -= copylen;
			nm_buf_len += copylen;

			if (iov_frag_size == 0) {
				break;
			}

			ring->slot[head].len = nm_buf_len;
			ring->slot[head].flags = NS_MOREFRAG;
			head = nm_ring_next(ring, head);
			if (head == ring->tail) {
				/*
				 * We ran out of netmap slots while
				 * splitting the iovec fragments.
				 */
				WPRINTF(("No space, drop %zu bytes",
				   count_iov(iov, iovcnt)));
				goto txsync;
			}
			nm_buf = NETMAP_BUF(ring, ring->slot[head].buf_idx);
			nm_buf_size = ring->nr_buf_size;
			nm_buf_len = 0;
		}
	}

	/* Complete the last slot, which must not have NS_MOREFRAG set. */
	ring->slot[head].len = nm_buf_len;
	ring->slot[head].flags = 0;
	head = nm_ring_next(ring, head);

	/* Now update ring->head and ring->cur. */
	ring->head = ring->cur = head;
txsync:
	ioctl(be->fd, NIOCTXSYNC, NULL);

	return (totlen);
}

static ssize_t
netmap_peek_recvlen(struct net_backend *be)
{
	struct netmap_priv *priv = (struct netmap_priv *)be->opaque;
	struct netmap_ring *ring = priv->rx;
	uint32_t head = ring->head;
	ssize_t totlen = 0;

	while (head != ring->tail) {
		struct netmap_slot *slot = ring->slot + head;

		totlen += slot->len;
		if ((slot->flags & NS_MOREFRAG) == 0)
			break;
		head = nm_ring_next(ring, head);
	}

	return (totlen);
}

static ssize_t
netmap_recv(struct net_backend *be, const struct iovec *iov, int iovcnt)
{
	struct netmap_priv *priv = (struct netmap_priv *)be->opaque;
	struct netmap_slot *slot = NULL;
	struct netmap_ring *ring;
	void *iov_frag_buf;
	int iov_frag_size;
	ssize_t totlen = 0;
	uint32_t head;

	assert(iovcnt);

	ring = priv->rx;
	head = ring->head;
	iov_frag_buf = iov->iov_base;
	iov_frag_size = iov->iov_len;

	do {
		int nm_buf_len;
		void *nm_buf;

		if (head == ring->tail) {
			return (0);
		}

		slot = ring->slot + head;
		nm_buf = NETMAP_BUF(ring, slot->buf_idx);
		nm_buf_len = slot->len;

		for (;;) {
			int copylen = nm_buf_len < iov_frag_size ?
			    nm_buf_len : iov_frag_size;

			memcpy(iov_frag_buf, nm_buf, copylen);
			nm_buf += copylen;
			nm_buf_len -= copylen;
			iov_frag_buf += copylen;
			iov_frag_size -= copylen;
			totlen += copylen;

			if (nm_buf_len == 0) {
				break;
			}

			iov++;
			iovcnt--;
			if (iovcnt == 0) {
				/* No space to receive. */
				WPRINTF(("Short iov, drop %zd bytes",
				    totlen));
				return (-ENOSPC);
			}
			iov_frag_buf = iov->iov_base;
			iov_frag_size = iov->iov_len;
		}

		head = nm_ring_next(ring, head);

	} while (slot->flags & NS_MOREFRAG);

	/* Release slots to netmap. */
	ring->head = ring->cur = head;

	return (totlen);
}

static void
netmap_recv_enable(struct net_backend *be)
{
	struct netmap_priv *priv = (struct netmap_priv *)be->opaque;

	mevent_enable(priv->mevp);
}

static void
netmap_recv_disable(struct net_backend *be)
{
	struct netmap_priv *priv = (struct netmap_priv *)be->opaque;

	mevent_disable(priv->mevp);
}

static struct net_backend netmap_backend = {
	.prefix = "netmap",
	.priv_size = sizeof(struct netmap_priv),
	.init = netmap_init,
	.cleanup = netmap_cleanup,
	.send = netmap_send,
	.peek_recvlen = netmap_peek_recvlen,
	.recv = netmap_recv,
	.recv_enable = netmap_recv_enable,
	.recv_disable = netmap_recv_disable,
	.get_cap = netmap_get_cap,
	.set_cap = netmap_set_cap,
};

/* A clone of the netmap backend, with a different prefix. */
static struct net_backend vale_backend = {
	.prefix = "vale",
	.priv_size = sizeof(struct netmap_priv),
	.init = netmap_init,
	.cleanup = netmap_cleanup,
	.send = netmap_send,
	.peek_recvlen = netmap_peek_recvlen,
	.recv = netmap_recv,
	.recv_enable = netmap_recv_enable,
	.recv_disable = netmap_recv_disable,
	.get_cap = netmap_get_cap,
	.set_cap = netmap_set_cap,
};

DATA_SET(net_backend_set, netmap_backend);
DATA_SET(net_backend_set, vale_backend);

#else /* __FreeBSD__ */

/*
 * The illumos dlpi backend
 */

/*
 * The size of the bounce buffer used to implement the peek callback.
 * This value should be big enough to accommodate the largest of all possible
 * frontend packet lengths. The value here matches the definition of
 * VTNET_MAX_PKT_LEN in pci_virtio_net.c
 */
#define	DLPI_BBUF_SIZE (65536 + 64)

typedef struct be_dlpi_priv {
	dlpi_handle_t bdp_dhp;
	struct mevent *bdp_mevp;
	/*
	 * A bounce buffer that allows us to implement the peek_recvlen
	 * callback. Each structure is only used by a single thread so
	 * one is enough.
	 */
	uint8_t bdp_bbuf[DLPI_BBUF_SIZE];
	ssize_t bdp_bbuflen;
} be_dlpi_priv_t;

static void
be_dlpi_cleanup(net_backend_t *be)
{
	be_dlpi_priv_t *priv = (be_dlpi_priv_t *)be->opaque;

	if (priv->bdp_dhp != NULL)
		dlpi_close(priv->bdp_dhp);
	priv->bdp_dhp = NULL;

	if (priv->bdp_mevp != NULL)
		mevent_delete(priv->bdp_mevp);
	priv->bdp_mevp = NULL;

	priv->bdp_bbuflen = 0;
	be->fd = -1;
}

static void
be_dlpi_err(int ret, const char *dev, char *msg)
{
	WPRINTF(("%s: %s (%s)", dev, msg, dlpi_strerror(ret)));
}

static int
be_dlpi_init(net_backend_t *be, const char *devname __unused,
     nvlist_t *nvl, net_be_rxeof_t cb, void *param)
{
	be_dlpi_priv_t *priv = (be_dlpi_priv_t *)be->opaque;
	const char *vnic;
	int ret;

	if (cb == NULL) {
		WPRINTF(("dlpi backend requires non-NULL callback"));
		return (-1);
	}

	vnic = get_config_value_node(nvl, "vnic");
	if (vnic == NULL) {
		WPRINTF(("dlpi backend requires a VNIC"));
		return (-1);
	}

	priv->bdp_bbuflen = 0;

	ret = dlpi_open(vnic, &priv->bdp_dhp, DLPI_RAW);

	if (ret != DLPI_SUCCESS) {
		be_dlpi_err(ret, vnic, "open failed");
		goto error;
	}

	if ((ret = dlpi_bind(priv->bdp_dhp, DLPI_ANY_SAP, NULL)) !=
	    DLPI_SUCCESS) {
		be_dlpi_err(ret, vnic, "bind failed");
		goto error;
	}

	if (get_config_bool_node_default(nvl, "promiscrxonly", true)) {
		if ((ret = dlpi_promiscon(priv->bdp_dhp, DL_PROMISC_RX_ONLY)) !=
		    DLPI_SUCCESS) {
			be_dlpi_err(ret, vnic,
			    "enable promiscuous mode(rxonly) failed");
			goto error;
		}
	}
	if (get_config_bool_node_default(nvl, "promiscphys", false)) {
		if ((ret = dlpi_promiscon(priv->bdp_dhp, DL_PROMISC_PHYS)) !=
		    DLPI_SUCCESS) {
			be_dlpi_err(ret, vnic,
			    "enable promiscuous mode(physical) failed");
			goto error;
		}
	}
	if (get_config_bool_node_default(nvl, "promiscsap", true)) {
		if ((ret = dlpi_promiscon(priv->bdp_dhp, DL_PROMISC_SAP)) !=
		    DLPI_SUCCESS) {
			be_dlpi_err(ret, vnic,
			    "enable promiscuous mode(SAP) failed");
			goto error;
		}
	}
	if (get_config_bool_node_default(nvl, "promiscmulti", true)) {
		if ((ret = dlpi_promiscon(priv->bdp_dhp, DL_PROMISC_MULTI)) !=
		    DLPI_SUCCESS) {
			be_dlpi_err(ret, vnic,
			    "enable promiscuous mode(muticast) failed");
			goto error;
		}
	}

        be->fd = dlpi_fd(priv->bdp_dhp);

        if (fcntl(be->fd, F_SETFL, O_NONBLOCK) < 0) {
                WPRINTF(("%s: enable O_NONBLOCK failed", vnic));
		goto error;
        }

	priv->bdp_mevp = mevent_add_disabled(be->fd, EVF_READ, cb, param);
	if (priv->bdp_mevp == NULL) {
		WPRINTF(("Could not register event"));
		goto error;
	}

	return (0);

error:
	be_dlpi_cleanup(be);
	return (-1);
}

/*
 * Called to send a buffer chain out to the dlpi device
 */
static ssize_t
be_dlpi_send(net_backend_t *be, const struct iovec *iov, int iovcnt)
{
	be_dlpi_priv_t *priv = (be_dlpi_priv_t *)be->opaque;
	ssize_t len = 0;
	int ret;

	if (iovcnt == 1) {
		len = iov[0].iov_len;
		ret = dlpi_send(priv->bdp_dhp, NULL, 0, iov[0].iov_base, len,
		    NULL);
	} else {
		void *buf = NULL;

		len = iov_to_buf(iov, iovcnt, &buf);

		if (len <= 0 || buf == NULL)
			return (-1);

		ret = dlpi_send(priv->bdp_dhp, NULL, 0, buf, len, NULL);
		free(buf);
	}

	if (ret != DLPI_SUCCESS)
		return (-1);

	return (len);
}

static ssize_t
be_dlpi_peek_recvlen(net_backend_t *be)
{
	be_dlpi_priv_t *priv = (be_dlpi_priv_t *)be->opaque;
	dlpi_recvinfo_t recv;
	size_t len;
	int ret;

	/*
	 * We already have a packet in the bounce buffer.
	 * Just return its length.
	 */
	if (priv->bdp_bbuflen > 0)
		return (priv->bdp_bbuflen);

	/*
	 * Read the next packet (if any) into the bounce buffer, so
	 * that we get to know its length and we can return that
	 * to the caller.
	 */
	len = sizeof (priv->bdp_bbuf);
	ret = dlpi_recv(priv->bdp_dhp, NULL, NULL, priv->bdp_bbuf, &len,
	    0, &recv);
	if (ret == DL_SYSERR) {
		if (errno == EWOULDBLOCK)
			return (0);
		return (-1);
	} else if (ret == DLPI_ETIMEDOUT) {
		return (0);
	} else if (ret != DLPI_SUCCESS) {
		return (-1);
	}

	if (recv.dri_totmsglen > sizeof (priv->bdp_bbuf)) {
		EPRINTLN("DLPI bounce buffer was too small! - needed %x bytes",
		    recv.dri_totmsglen);
	}

	priv->bdp_bbuflen = len;

	return (len);
}

static ssize_t
be_dlpi_recv(net_backend_t *be, const struct iovec *iov, int iovcnt)
{
	be_dlpi_priv_t *priv = (be_dlpi_priv_t *)be->opaque;
	size_t len;
	int ret;

	if (priv->bdp_bbuflen > 0) {
		/*
		 * A packet is available in the bounce buffer, so
		 * we read it from there.
		 */
		len = buf_to_iov(priv->bdp_bbuf, priv->bdp_bbuflen,
		    iov, iovcnt, 0);

		/* Mark the bounce buffer as empty. */
		priv->bdp_bbuflen = 0;

		return (len);
	}

	len = iov[0].iov_len;
	ret = dlpi_recv(priv->bdp_dhp, NULL, NULL,
	    (uint8_t *)iov[0].iov_base, &len, 0, NULL);
	if (ret == DL_SYSERR) {
		if (errno == EWOULDBLOCK)
			return (0);
		return (-1);
	} else if (ret == DLPI_ETIMEDOUT) {
		return (0);
	} else if (ret != DLPI_SUCCESS) {
		return (-1);
	}

	return (len);
}

static void
be_dlpi_recv_enable(net_backend_t *be)
{
	be_dlpi_priv_t *priv = (be_dlpi_priv_t *)be->opaque;

	mevent_enable(priv->bdp_mevp);
}

static void
be_dlpi_recv_disable(net_backend_t *be)
{
	be_dlpi_priv_t *priv = (be_dlpi_priv_t *)be->opaque;

	mevent_disable(priv->bdp_mevp);
}

static uint64_t
be_dlpi_get_cap(net_backend_t *be)
{
	return (0); /* no capabilities for now */
}

static int
be_dlpi_set_cap(net_backend_t *be, uint64_t features,
    unsigned vnet_hdr_len)
{
	return ((features || vnet_hdr_len) ? -1 : 0);
}

static int
be_dlpi_get_mac(net_backend_t *be, void *buf, size_t *buflen)
{
	be_dlpi_priv_t *priv = (be_dlpi_priv_t *)be->opaque;
	uchar_t physaddr[DLPI_PHYSADDR_MAX];
	size_t physaddrlen = DLPI_PHYSADDR_MAX;
	int ret;

	if ((ret = dlpi_get_physaddr(priv->bdp_dhp, DL_CURR_PHYS_ADDR,
	    physaddr, &physaddrlen)) != DLPI_SUCCESS) {
		be_dlpi_err(ret, dlpi_linkname(priv->bdp_dhp),
		    "read MAC address failed");
		return (EINVAL);
	}

	if (physaddrlen != ETHERADDRL) {
		WPRINTF(("%s: bad MAC address len %d",
		    dlpi_linkname(priv->bdp_dhp), physaddrlen));
		return (EINVAL);
	}

	if (physaddrlen > *buflen) {
		WPRINTF(("%s: MAC address too long (%d bytes required)",
		    dlpi_linkname(priv->bdp_dhp), physaddrlen));
		return (ENOMEM);
	}

	*buflen = physaddrlen;
	memcpy(buf, physaddr, *buflen);

	return (0);
}

static struct net_backend dlpi_backend = {
	.prefix = "dlpi",
	.priv_size = sizeof(struct be_dlpi_priv),
	.init = be_dlpi_init,
	.cleanup = be_dlpi_cleanup,
	.send = be_dlpi_send,
	.peek_recvlen = be_dlpi_peek_recvlen,
	.recv = be_dlpi_recv,
	.recv_enable = be_dlpi_recv_enable,
	.recv_disable = be_dlpi_recv_disable,
	.get_cap = be_dlpi_get_cap,
	.set_cap = be_dlpi_set_cap,
	.get_mac = be_dlpi_get_mac,
};

DATA_SET(net_backend_set, dlpi_backend);

#endif /* __FreeBSD__ */

#ifdef __FreeBSD__
int
netbe_legacy_config(nvlist_t *nvl, const char *opts)
{
	char *backend, *cp;

	if (opts == NULL)
		return (0);

	cp = strchr(opts, ',');
	if (cp == NULL) {
		set_config_value_node(nvl, "backend", opts);
		return (0);
	}
	backend = strndup(opts, cp - opts);
	set_config_value_node(nvl, "backend", backend);
	free(backend);
	return (pci_parse_legacy_config(nvl, cp + 1));
}
#else
int
netbe_legacy_config(nvlist_t *nvl, const char *opts)
{
	char *config, *name, *tofree, *value;

	if (opts == NULL)
		return (0);

	/* Default to the 'dlpi' backend - can still be overridden by opts */
	set_config_value_node(nvl, "backend", "dlpi");

	config = tofree = strdup(opts);
	if (config == NULL)
		err(4, "netbe_legacy_config strdup()");
	while ((name = strsep(&config, ",")) != NULL) {
		value = strchr(name, '=');
		if (value != NULL) {
			*value++ = '\0';
			set_config_value_node(nvl, name, value);
		} else {
			set_config_value_node(nvl, "vnic", name);
		}
	}
	free(tofree);

	return (0);
}
#endif

/*
 * Initialize a backend and attach to the frontend.
 * This is called during frontend initialization.
 *  @ret is a pointer to the backend to be initialized
 *  @devname is the backend-name as supplied on the command line,
 * 	e.g. -s 2:0,frontend-name,backend-name[,other-args]
 *  @cb is the receive callback supplied by the frontend,
 *	and it is invoked in the event loop when a receive
 *	event is generated in the hypervisor,
 *  @param is a pointer to the frontend, and normally used as
 *	the argument for the callback.
 */
int
netbe_init(struct net_backend **ret, nvlist_t *nvl, net_be_rxeof_t cb,
    void *param)
{
	struct net_backend **pbe, *nbe, *tbe = NULL;
	const char *value;
	char *devname;
	int err;

	value = get_config_value_node(nvl, "backend");
	if (value == NULL) {
		return (-1);
	}
	devname = strdup(value);

	/*
	 * Find the network backend that matches the user-provided
	 * device name. net_backend_set is built using a linker set.
	 */
	SET_FOREACH(pbe, net_backend_set) {
		if (strncmp(devname, (*pbe)->prefix,
		    strlen((*pbe)->prefix)) == 0) {
			tbe = *pbe;
			assert(tbe->init != NULL);
			assert(tbe->cleanup != NULL);
			assert(tbe->send != NULL);
			assert(tbe->recv != NULL);
			assert(tbe->get_cap != NULL);
			assert(tbe->set_cap != NULL);
			break;
		}
	}

	*ret = NULL;
	if (tbe == NULL) {
		free(devname);
		return (EINVAL);
	}

	nbe = calloc(1, sizeof(*nbe) + tbe->priv_size);
	*nbe = *tbe;	/* copy the template */
	nbe->fd = -1;
	nbe->sc = param;
	nbe->be_vnet_hdr_len = 0;
	nbe->fe_vnet_hdr_len = 0;

	/* Initialize the backend. */
	err = nbe->init(nbe, devname, nvl, cb, param);
	if (err) {
		free(devname);
		free(nbe);
		return (err);
	}

	*ret = nbe;
	free(devname);

	return (0);
}

void
netbe_cleanup(struct net_backend *be)
{

	if (be != NULL) {
		be->cleanup(be);
		free(be);
	}
}

uint64_t
netbe_get_cap(struct net_backend *be)
{

	assert(be != NULL);
	return (be->get_cap(be));
}

int
netbe_set_cap(struct net_backend *be, uint64_t features,
	      unsigned vnet_hdr_len)
{
	int ret;

	assert(be != NULL);

	/* There are only three valid lengths, i.e., 0, 10 and 12. */
	if (vnet_hdr_len && vnet_hdr_len != VNET_HDR_LEN
		&& vnet_hdr_len != (VNET_HDR_LEN - sizeof(uint16_t)))
		return (-1);

	be->fe_vnet_hdr_len = vnet_hdr_len;

	ret = be->set_cap(be, features, vnet_hdr_len);
	assert(be->be_vnet_hdr_len == 0 ||
	       be->be_vnet_hdr_len == be->fe_vnet_hdr_len);

	return (ret);
}

ssize_t
netbe_send(struct net_backend *be, const struct iovec *iov, int iovcnt)
{

	return (be->send(be, iov, iovcnt));
}

ssize_t
netbe_peek_recvlen(struct net_backend *be)
{

	return (be->peek_recvlen(be));
}

/*
 * Try to read a packet from the backend, without blocking.
 * If no packets are available, return 0. In case of success, return
 * the length of the packet just read. Return -1 in case of errors.
 */
ssize_t
netbe_recv(struct net_backend *be, const struct iovec *iov, int iovcnt)
{

	return (be->recv(be, iov, iovcnt));
}

/*
 * Read a packet from the backend and discard it.
 * Returns the size of the discarded packet or zero if no packet was available.
 * A negative error code is returned in case of read error.
 */
ssize_t
netbe_rx_discard(struct net_backend *be)
{
	/*
	 * MP note: the dummybuf is only used to discard frames,
	 * so there is no need for it to be per-vtnet or locked.
	 * We only make it large enough for TSO-sized segment.
	 */
	static uint8_t dummybuf[65536 + 64];
	struct iovec iov;

#ifdef __FreeBSD__
	iov.iov_base = dummybuf;
#else
	iov.iov_base = (caddr_t)dummybuf;
#endif
	iov.iov_len = sizeof(dummybuf);

	return netbe_recv(be, &iov, 1);
}

void
netbe_rx_disable(struct net_backend *be)
{

	return be->recv_disable(be);
}

void
netbe_rx_enable(struct net_backend *be)
{

	return be->recv_enable(be);
}

size_t
netbe_get_vnet_hdr_len(struct net_backend *be)
{

	return (be->be_vnet_hdr_len);
}

#ifndef __FreeBSD__
int
netbe_get_mac(net_backend_t *be, void *buf, size_t *buflen)
{
	if (be->get_mac == NULL)
		return (ENOTSUP);
	return (be->get_mac(be, buf, buflen));
}
#endif
