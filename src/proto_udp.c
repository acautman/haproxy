/*
 * AF_CUST_UDP/AF_CUST_UDP6 UDP protocol layer
 *
 * Copyright 2019 HAProxy Technologies, Frédéric Lécaille <flecaille@haproxy.com>
 *
 * Partial merge by Emeric Brun <ebrun@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <netinet/udp.h>
#include <netinet/in.h>

#include <haproxy/fd.h>
#include <haproxy/listener.h>
#include <haproxy/log.h>
#include <haproxy/namespace.h>
#include <haproxy/port_range.h>
#include <haproxy/protocol.h>
#include <haproxy/proto_udp.h>
#include <haproxy/proxy.h>
#include <haproxy/server.h>
#include <haproxy/sock.h>
#include <haproxy/sock_inet.h>
#include <haproxy/task.h>

static int udp_bind_listeners(struct protocol *proto, char *errmsg, int errlen);
static int udp_bind_listener(struct listener *listener, char *errmsg, int errlen);
static void udp4_add_listener(struct listener *listener, int port);
static void udp6_add_listener(struct listener *listener, int port);

/* Note: must not be declared <const> as its list will be overwritten */
static struct protocol proto_udp4 = {
	.name = "udp4",
	.sock_domain = AF_CUST_UDP4,
	.sock_type = SOCK_DGRAM,
	.sock_prot = IPPROTO_UDP,
	.sock_family = AF_INET,
	.sock_addrlen = sizeof(struct sockaddr_in),
	.l3_addrlen = 32/8,
	.accept = NULL,
	.connect = NULL,
	.listen = udp_bind_listener,
	.bind_all = udp_bind_listeners,
	.unbind_all = unbind_all_listeners,
	.enable_all = enable_all_listeners,
	.get_src = udp_get_src,
	.get_dst = udp_get_dst,
	.pause = udp_pause_listener,
	.add = udp4_add_listener,
	.addrcmp = sock_inet4_addrcmp,
	.listeners = LIST_HEAD_INIT(proto_udp4.listeners),
	.nb_listeners = 0,
};

INITCALL1(STG_REGISTER, protocol_register, &proto_udp4);

/* Note: must not be declared <const> as its list will be overwritten */
static struct protocol proto_udp6 = {
	.name = "udp6",
	.sock_domain = AF_CUST_UDP6,
	.sock_type = SOCK_DGRAM,
	.sock_prot = IPPROTO_UDP,
	.sock_family = AF_INET6,
	.sock_addrlen = sizeof(struct sockaddr_in6),
	.l3_addrlen = 128/8,
	.accept = NULL,
	.connect = NULL,
	.listen = udp_bind_listener,
	.bind_all = udp_bind_listeners,
	.unbind_all = unbind_all_listeners,
	.enable_all = enable_all_listeners,
	.get_src = udp6_get_src,
	.get_dst = udp6_get_dst,
	.pause = udp_pause_listener,
	.add = udp6_add_listener,
	.addrcmp = sock_inet6_addrcmp,
	.listeners = LIST_HEAD_INIT(proto_udp6.listeners),
	.nb_listeners = 0,
};

INITCALL1(STG_REGISTER, protocol_register, &proto_udp6);

/*
 * Retrieves the source address for the socket <fd>, with <dir> indicating
 * if we're a listener (=0) or an initiator (!=0). It returns 0 in case of
 * success, -1 in case of error. The socket's source address is stored in
 * <sa> for <salen> bytes.
 */
int udp_get_src(int fd, struct sockaddr *sa, socklen_t salen, int dir)
{
	int ret;

	ret = sock_get_src(fd, sa, salen, dir);
	if (!ret)
		sa->sa_family = AF_CUST_UDP4;

	return ret;
}

/*
 * Retrieves the source address for the socket <fd>, with <dir> indicating
 * if we're a listener (=0) or an initiator (!=0). It returns 0 in case of
 * success, -1 in case of error. The socket's source address is stored in
 * <sa> for <salen> bytes.
 */
int udp6_get_src(int fd, struct sockaddr *sa, socklen_t salen, int dir)
{
	int ret;

	ret = sock_get_src(fd, sa, salen, dir);
	if (!ret)
		sa->sa_family = AF_CUST_UDP6;

	return ret;
}

/*
 * Retrieves the original destination address for the socket <fd>, with <dir>
 * indicating if we're a listener (=0) or an initiator (!=0). In the case of a
 * listener, if the original destination address was translated, the original
 * address is retrieved. It returns 0 in case of success, -1 in case of error.
 * The socket's source address is stored in <sa> for <salen> bytes.
 */
int udp_get_dst(int fd, struct sockaddr *sa, socklen_t salen, int dir)
{
	int ret;

	ret = sock_inet_get_dst(fd, sa, salen, dir);
	if (!ret)
		sa->sa_family = AF_CUST_UDP4;

	return ret;
}

/*
 * Retrieves the original destination address for the socket <fd>, with <dir>
 * indicating if we're a listener (=0) or an initiator (!=0). In the case of a
 * listener, if the original destination address was translated, the original
 * address is retrieved. It returns 0 in case of success, -1 in case of error.
 * The socket's source address is stored in <sa> for <salen> bytes.
 */
int udp6_get_dst(int fd, struct sockaddr *sa, socklen_t salen, int dir)
{
	int ret;

	ret = sock_get_dst(fd, sa, salen, dir);
	if (!ret)
		sa->sa_family = AF_CUST_UDP6;

	return ret;
}

/* This function tries to bind a UDPv4/v6 listener. It may return a warning or
 * an error message in <errmsg> if the message is at most <errlen> bytes long
 * (including '\0'). Note that <errmsg> may be NULL if <errlen> is also zero.
 * The return value is composed from ERR_ABORT, ERR_WARN,
 * ERR_ALERT, ERR_RETRYABLE and ERR_FATAL. ERR_NONE indicates that everything
 * was alright and that no message was returned. ERR_RETRYABLE means that an
 * error occurred but that it may vanish after a retry (eg: port in use), and
 * ERR_FATAL indicates a non-fixable error. ERR_WARN and ERR_ALERT do not alter
 * the meaning of the error, but just indicate that a message is present which
 * should be displayed with the respective level. Last, ERR_ABORT indicates
 * that it's pointless to try to start other listeners. No error message is
 * returned if errlen is NULL.
 */
int udp_bind_listener(struct listener *listener, char *errmsg, int errlen)
{
	int err = ERR_NONE;
	void *handler = NULL;
	char *msg = NULL;

	/* ensure we never return garbage */
	if (errlen)
		*errmsg = 0;

	if (listener->state != LI_ASSIGNED)
		return ERR_NONE; /* already bound */

	switch (listener->bind_conf->frontend->mode) {
	case PR_MODE_SYSLOG:
		handler = syslog_fd_handler;
		break;
	default:
		err |= ERR_FATAL | ERR_ALERT;
		msg = "UDP is not yet supported on this proxy mode";
		goto udp_return;
	}

	err = sock_inet_bind_receiver(&listener->rx,
	                              handler, listener,
	                              listener->bind_conf->bind_thread, &msg);

	if (err != ERR_NONE) {
		snprintf(errmsg, errlen, "%s", msg);
		free(msg); msg = NULL;
		return err;
	}
	listener->state = LI_LISTEN;

 udp_return:
	if (msg && errlen) {
		char pn[INET6_ADDRSTRLEN];

		addr_to_str(&listener->rx.addr, pn, sizeof(pn));
		snprintf(errmsg, errlen, "%s [%s:%d]", msg, pn, get_host_port(&listener->rx.addr));
	}
	return err;
}

/* This function creates all UDP sockets bound to the protocol entry <proto>.
 * It is intended to be used as the protocol's bind_all() function.
 * The sockets will be registered but not added to any fd_set, in order not to
 * loose them across the fork(). A call to enable_all_listeners() is needed
 * to complete initialization. The return value is composed from ERR_*.
 */
static int udp_bind_listeners(struct protocol *proto, char *errmsg, int errlen)
{
	struct listener *listener;
	int err = ERR_NONE;

	list_for_each_entry(listener, &proto->listeners, rx.proto_list) {
		err |= udp_bind_listener(listener, errmsg, errlen);
		if (err & ERR_ABORT)
			break;
	}

	return err;
}

/* Add <listener> to the list of udp4 listeners, on port <port>. The
 * listener's state is automatically updated from LI_INIT to LI_ASSIGNED.
 * The number of listeners for the protocol is updated.
 */
static void udp4_add_listener(struct listener *listener, int port)
{
	if (listener->state != LI_INIT)
		return;
	listener->state = LI_ASSIGNED;
	listener->rx.proto = &proto_udp4;
	((struct sockaddr_in *)(&listener->rx.addr))->sin_port = htons(port);
	LIST_ADDQ(&proto_udp4.listeners, &listener->rx.proto_list);
	proto_udp4.nb_listeners++;
}

/* Add <listener> to the list of udp6 listeners, on port <port>. The
 * listener's state is automatically updated from LI_INIT to LI_ASSIGNED.
 * The number of listeners for the protocol is updated.
 */
static void udp6_add_listener(struct listener *listener, int port)
{
	if (listener->state != LI_INIT)
		return;
	listener->state = LI_ASSIGNED;
	listener->rx.proto = &proto_udp6;
	((struct sockaddr_in *)(&listener->rx.addr))->sin_port = htons(port);
	LIST_ADDQ(&proto_udp6.listeners, &listener->rx.proto_list);
	proto_udp6.nb_listeners++;
}

/* Pause a listener. Returns < 0 in case of failure, 0 if the listener
 * was totally stopped, or > 0 if correctly paused.
 */
int udp_pause_listener(struct listener *l)
{
	/* we don't support pausing on UDP */
	return -1;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
