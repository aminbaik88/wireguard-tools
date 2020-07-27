// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2020 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#ifdef __linux__
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <linux/wireguard.h>
#include "netlink.h"
#endif
#ifdef __OpenBSD__
#include <net/if_wg.h>
#endif
#include <netinet/in.h>
#include <sys/socket.h>
#include <net/if.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <netdb.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include "ipc.h"
#include "containers.h"
#include "encoding.h"
#include "curve25519.h"

#define SOCK_PATH RUNSTATEDIR "/wireguard/"
#define SOCK_SUFFIX ".sock"
#ifdef __linux__
#define SOCKET_BUFFER_SIZE (mnl_ideal_socket_buffer_size())
#else
#define SOCKET_BUFFER_SIZE 8192
#endif

struct string_list {
	char *buffer;
	size_t len;
	size_t cap;
};

static int string_list_add(struct string_list *list, const char *str)
{
	size_t len = strlen(str) + 1;

	if (len == 1)
		return 0;

	if (len >= list->cap - list->len) {
		char *new_buffer;
		size_t new_cap = list->cap * 2;

		if (new_cap < list->len + len + 1)
			new_cap = list->len + len + 1;
		new_buffer = realloc(list->buffer, new_cap);
		if (!new_buffer)
			return -errno;
		list->buffer = new_buffer;
		list->cap = new_cap;
	}
	memcpy(list->buffer + list->len, str, len);
	list->len += len;
	list->buffer[list->len] = '\0';
	return 0;
}

#ifndef WINCOMPAT
static FILE *userspace_interface_file(const char *iface)
{
	struct stat sbuf;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd = -1, ret;
	FILE *f = NULL;

	errno = EINVAL;
	if (strchr(iface, '/'))
		goto out;
	ret = snprintf(addr.sun_path, sizeof(addr.sun_path), SOCK_PATH "%s" SOCK_SUFFIX, iface);
	if (ret < 0)
		goto out;
	ret = stat(addr.sun_path, &sbuf);
	if (ret < 0)
		goto out;
	errno = EBADF;
	if (!S_ISSOCK(sbuf.st_mode))
		goto out;

	ret = fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ret < 0)
		goto out;

	ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		if (errno == ECONNREFUSED) /* If the process is gone, we try to clean up the socket. */
			unlink(addr.sun_path);
		goto out;
	}
	f = fdopen(fd, "r+");
	if (f)
		errno = 0;
out:
	ret = -errno;
	if (ret) {
		if (fd >= 0)
			close(fd);
		errno = -ret;
		return NULL;
	}
	return f;
}

static bool userspace_has_wireguard_interface(const char *iface)
{
	struct stat sbuf;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int fd, ret;

	if (strchr(iface, '/'))
		return false;
	if (snprintf(addr.sun_path, sizeof(addr.sun_path), SOCK_PATH "%s" SOCK_SUFFIX, iface) < 0)
		return false;
	if (stat(addr.sun_path, &sbuf) < 0)
		return false;
	if (!S_ISSOCK(sbuf.st_mode))
		return false;
	ret = fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ret < 0)
		return false;
	ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0 && errno == ECONNREFUSED) { /* If the process is gone, we try to clean up the socket. */
		close(fd);
		unlink(addr.sun_path);
		return false;
	}
	close(fd);
	return true;
}

static int userspace_get_wireguard_interfaces(struct string_list *list)
{
	DIR *dir;
	struct dirent *ent;
	size_t len;
	char *end;
	int ret = 0;

	dir = opendir(SOCK_PATH);
	if (!dir)
		return errno == ENOENT ? 0 : -errno;
	while ((ent = readdir(dir))) {
		len = strlen(ent->d_name);
		if (len <= strlen(SOCK_SUFFIX))
			continue;
		end = &ent->d_name[len - strlen(SOCK_SUFFIX)];
		if (strncmp(end, SOCK_SUFFIX, strlen(SOCK_SUFFIX)))
			continue;
		*end = '\0';
		if (!userspace_has_wireguard_interface(ent->d_name))
			continue;
		ret = string_list_add(list, ent->d_name);
		if (ret < 0)
			goto out;
	}
out:
	closedir(dir);
	return ret;
}
#else
#include "wincompat/ipc.c"
#endif

static int userspace_set_device(struct wgdevice *dev)
{
	char hex[WG_KEY_LEN_HEX], ip[INET6_ADDRSTRLEN], host[4096 + 1], service[512 + 1];
	struct wgpeer *peer;
	struct wgallowedip *allowedip;
	FILE *f;
	int ret;
	socklen_t addr_len;

	f = userspace_interface_file(dev->name);
	if (!f)
		return -errno;
	fprintf(f, "set=1\n");

	if (dev->flags & WGDEVICE_HAS_PRIVATE_KEY) {
		key_to_hex(hex, dev->private_key);
		fprintf(f, "private_key=%s\n", hex);
	}
	if (dev->flags & WGDEVICE_HAS_LISTEN_PORT)
		fprintf(f, "listen_port=%u\n", dev->listen_port);
	if (dev->flags & WGDEVICE_HAS_FWMARK)
		fprintf(f, "fwmark=%u\n", dev->fwmark);
	if (dev->flags & WGDEVICE_REPLACE_PEERS)
		fprintf(f, "replace_peers=true\n");

	for_each_wgpeer(dev, peer) {
		key_to_hex(hex, peer->public_key);
		fprintf(f, "public_key=%s\n", hex);
		if (peer->flags & WGPEER_REMOVE_ME) {
			fprintf(f, "remove=true\n");
			continue;
		}
		if (peer->flags & WGPEER_HAS_PRESHARED_KEY) {
			key_to_hex(hex, peer->preshared_key);
			fprintf(f, "preshared_key=%s\n", hex);
		}
		if (peer->endpoint.addr.sa_family == AF_INET || peer->endpoint.addr.sa_family == AF_INET6) {
			addr_len = 0;
			if (peer->endpoint.addr.sa_family == AF_INET)
				addr_len = sizeof(struct sockaddr_in);
			else if (peer->endpoint.addr.sa_family == AF_INET6)
				addr_len = sizeof(struct sockaddr_in6);
			if (!getnameinfo(&peer->endpoint.addr, addr_len, host, sizeof(host), service, sizeof(service), NI_DGRAM | NI_NUMERICSERV | NI_NUMERICHOST)) {
				if (peer->endpoint.addr.sa_family == AF_INET6 && strchr(host, ':'))
					fprintf(f, "endpoint=[%s]:%s\n", host, service);
				else
					fprintf(f, "endpoint=%s:%s\n", host, service);
			}
		}
		if (peer->flags & WGPEER_HAS_PERSISTENT_KEEPALIVE_INTERVAL)
			fprintf(f, "persistent_keepalive_interval=%u\n", peer->persistent_keepalive_interval);
		if (peer->flags & WGPEER_REPLACE_ALLOWEDIPS)
			fprintf(f, "replace_allowed_ips=true\n");
		for_each_wgallowedip(peer, allowedip) {
			if (allowedip->family == AF_INET) {
				if (!inet_ntop(AF_INET, &allowedip->ip4, ip, INET6_ADDRSTRLEN))
					continue;
			} else if (allowedip->family == AF_INET6) {
				if (!inet_ntop(AF_INET6, &allowedip->ip6, ip, INET6_ADDRSTRLEN))
					continue;
			} else
				continue;
			fprintf(f, "allowed_ip=%s/%d\n", ip, allowedip->cidr);
		}
	}
	fprintf(f, "\n");
	fflush(f);

	if (fscanf(f, "errno=%d\n\n", &ret) != 1)
		ret = errno ? -errno : -EPROTO;
	fclose(f);
	errno = -ret;
	return ret;
}

#define NUM(max) ({ \
	unsigned long long num; \
	char *end; \
	if (!isdigit(value[0])) \
		break; \
	num = strtoull(value, &end, 10); \
	if (*end || num > max) \
		break; \
	num; \
})

static int userspace_get_device(struct wgdevice **out, const char *iface)
{
	struct wgdevice *dev;
	struct wgpeer *peer = NULL;
	struct wgallowedip *allowedip = NULL;
	size_t line_buffer_len = 0, line_len;
	char *key = NULL, *value;
	FILE *f;
	int ret = -EPROTO;

	*out = dev = calloc(1, sizeof(*dev));
	if (!dev)
		return -errno;

	f = userspace_interface_file(iface);
	if (!f) {
		ret = -errno;
		free(dev);
		*out = NULL;
		return ret;
	}

	fprintf(f, "get=1\n\n");
	fflush(f);

	strncpy(dev->name, iface, IFNAMSIZ - 1);
	dev->name[IFNAMSIZ - 1] = '\0';

	while (getline(&key, &line_buffer_len, f) > 0) {
		line_len = strlen(key);
		if (line_len == 1 && key[0] == '\n')
			goto err;
		value = strchr(key, '=');
		if (!value || line_len == 0 || key[line_len - 1] != '\n')
			break;
		*value++ = key[--line_len] = '\0';

		if (!peer && !strcmp(key, "private_key")) {
			if (!key_from_hex(dev->private_key, value))
				break;
			curve25519_generate_public(dev->public_key, dev->private_key);
			dev->flags |= WGDEVICE_HAS_PRIVATE_KEY | WGDEVICE_HAS_PUBLIC_KEY;
		} else if (!peer && !strcmp(key, "listen_port")) {
			dev->listen_port = NUM(0xffffU);
			dev->flags |= WGDEVICE_HAS_LISTEN_PORT;
		} else if (!peer && !strcmp(key, "fwmark")) {
			dev->fwmark = NUM(0xffffffffU);
			dev->flags |= WGDEVICE_HAS_FWMARK;
		} else if (!strcmp(key, "public_key")) {
			struct wgpeer *new_peer = calloc(1, sizeof(*new_peer));

			if (!new_peer) {
				ret = -ENOMEM;
				goto err;
			}
			allowedip = NULL;
			if (peer)
				peer->next_peer = new_peer;
			else
				dev->first_peer = new_peer;
			peer = new_peer;
			if (!key_from_hex(peer->public_key, value))
				break;
			peer->flags |= WGPEER_HAS_PUBLIC_KEY;
		} else if (peer && !strcmp(key, "preshared_key")) {
			if (!key_from_hex(peer->preshared_key, value))
				break;
			if (!key_is_zero(peer->preshared_key))
				peer->flags |= WGPEER_HAS_PRESHARED_KEY;
		} else if (peer && !strcmp(key, "endpoint")) {
			char *begin, *end;
			struct addrinfo *resolved;
			struct addrinfo hints = {
				.ai_family = AF_UNSPEC,
				.ai_socktype = SOCK_DGRAM,
				.ai_protocol = IPPROTO_UDP
			};
			if (!strlen(value))
				break;
			if (value[0] == '[') {
				begin = &value[1];
				end = strchr(value, ']');
				if (!end)
					break;
				*end++ = '\0';
				if (*end++ != ':' || !*end)
					break;
			} else {
				begin = value;
				end = strrchr(value, ':');
				if (!end || !*(end + 1))
					break;
				*end++ = '\0';
			}
			if (getaddrinfo(begin, end, &hints, &resolved) != 0) {
				ret = ENETUNREACH;
				goto err;
			}
			if ((resolved->ai_family == AF_INET && resolved->ai_addrlen == sizeof(struct sockaddr_in)) ||
			    (resolved->ai_family == AF_INET6 && resolved->ai_addrlen == sizeof(struct sockaddr_in6)))
				memcpy(&peer->endpoint.addr, resolved->ai_addr, resolved->ai_addrlen);
			else  {
				freeaddrinfo(resolved);
				break;
			}
			freeaddrinfo(resolved);
		} else if (peer && !strcmp(key, "persistent_keepalive_interval")) {
			peer->persistent_keepalive_interval = NUM(0xffffU);
			peer->flags |= WGPEER_HAS_PERSISTENT_KEEPALIVE_INTERVAL;
		} else if (peer && !strcmp(key, "allowed_ip")) {
			struct wgallowedip *new_allowedip;
			char *end, *mask = value, *ip = strsep(&mask, "/");

			if (!mask || !isdigit(mask[0]))
				break;
			new_allowedip = calloc(1, sizeof(*new_allowedip));
			if (!new_allowedip) {
				ret = -ENOMEM;
				goto err;
			}
			if (allowedip)
				allowedip->next_allowedip = new_allowedip;
			else
				peer->first_allowedip = new_allowedip;
			allowedip = new_allowedip;
			allowedip->family = AF_UNSPEC;
			if (strchr(ip, ':')) {
				if (inet_pton(AF_INET6, ip, &allowedip->ip6) == 1)
					allowedip->family = AF_INET6;
			} else {
				if (inet_pton(AF_INET, ip, &allowedip->ip4) == 1)
					allowedip->family = AF_INET;
			}
			allowedip->cidr = strtoul(mask, &end, 10);
			if (*end || allowedip->family == AF_UNSPEC || (allowedip->family == AF_INET6 && allowedip->cidr > 128) || (allowedip->family == AF_INET && allowedip->cidr > 32))
				break;
		} else if (peer && !strcmp(key, "last_handshake_time_sec"))
			peer->last_handshake_time.tv_sec = NUM(0x7fffffffffffffffULL);
		else if (peer && !strcmp(key, "last_handshake_time_nsec"))
			peer->last_handshake_time.tv_nsec = NUM(0x7fffffffffffffffULL);
		else if (peer && !strcmp(key, "rx_bytes"))
			peer->rx_bytes = NUM(0xffffffffffffffffULL);
		else if (peer && !strcmp(key, "tx_bytes"))
			peer->tx_bytes = NUM(0xffffffffffffffffULL);
		else if (!strcmp(key, "errno"))
			ret = -NUM(0x7fffffffU);
	}
	ret = -EPROTO;
err:
	free(key);
	if (ret) {
		free_wgdevice(dev);
		*out = NULL;
	}
	fclose(f);
	errno = -ret;
	return ret;

}
#undef NUM

#ifdef __linux__

struct interface {
	const char *name;
	bool is_wireguard;
};

static int parse_linkinfo(const struct nlattr *attr, void *data)
{
	struct interface *interface = data;

	if (mnl_attr_get_type(attr) == IFLA_INFO_KIND && !strcmp(WG_GENL_NAME, mnl_attr_get_str(attr)))
		interface->is_wireguard = true;
	return MNL_CB_OK;
}

static int parse_infomsg(const struct nlattr *attr, void *data)
{
	struct interface *interface = data;

	if (mnl_attr_get_type(attr) == IFLA_LINKINFO)
		return mnl_attr_parse_nested(attr, parse_linkinfo, data);
	else if (mnl_attr_get_type(attr) == IFLA_IFNAME)
		interface->name = mnl_attr_get_str(attr);
	return MNL_CB_OK;
}

static int read_devices_cb(const struct nlmsghdr *nlh, void *data)
{
	struct string_list *list = data;
	struct interface interface = { 0 };
	int ret;

	ret = mnl_attr_parse(nlh, sizeof(struct ifinfomsg), parse_infomsg, &interface);
	if (ret != MNL_CB_OK)
		return ret;
	if (interface.name && interface.is_wireguard)
		ret = string_list_add(list, interface.name);
	if (ret < 0)
		return ret;
	if (nlh->nlmsg_type != NLMSG_DONE)
		return MNL_CB_OK + 1;
	return MNL_CB_OK;
}

static int kernel_get_wireguard_interfaces(struct string_list *list)
{
	struct mnl_socket *nl = NULL;
	char *rtnl_buffer = NULL;
	size_t message_len;
	unsigned int portid, seq;
	ssize_t len;
	int ret = 0;
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifm;

	ret = -ENOMEM;
	rtnl_buffer = calloc(SOCKET_BUFFER_SIZE, 1);
	if (!rtnl_buffer)
		goto cleanup;

	nl = mnl_socket_open(NETLINK_ROUTE);
	if (!nl) {
		ret = -errno;
		goto cleanup;
	}

	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		ret = -errno;
		goto cleanup;
	}

	seq = time(NULL);
	portid = mnl_socket_get_portid(nl);
	nlh = mnl_nlmsg_put_header(rtnl_buffer);
	nlh->nlmsg_type = RTM_GETLINK;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP;
	nlh->nlmsg_seq = seq;
	ifm = mnl_nlmsg_put_extra_header(nlh, sizeof(*ifm));
	ifm->ifi_family = AF_UNSPEC;
	message_len = nlh->nlmsg_len;

	if (mnl_socket_sendto(nl, rtnl_buffer, message_len) < 0) {
		ret = -errno;
		goto cleanup;
	}

another:
	if ((len = mnl_socket_recvfrom(nl, rtnl_buffer, SOCKET_BUFFER_SIZE)) < 0) {
		ret = -errno;
		goto cleanup;
	}
	if ((len = mnl_cb_run(rtnl_buffer, len, seq, portid, read_devices_cb, list)) < 0) {
		/* Netlink returns NLM_F_DUMP_INTR if the set of all tunnels changed
		 * during the dump. That's unfortunate, but is pretty common on busy
		 * systems that are adding and removing tunnels all the time. Rather
		 * than retrying, potentially indefinitely, we just work with the
		 * partial results. */
		if (errno != EINTR) {
			ret = -errno;
			goto cleanup;
		}
	}
	if (len == MNL_CB_OK + 1)
		goto another;
	ret = 0;

cleanup:
	free(rtnl_buffer);
	if (nl)
		mnl_socket_close(nl);
	return ret;
}

static int kernel_set_device(struct wgdevice *dev)
{
	int ret = 0;
	struct wgpeer *peer = NULL;
	struct wgallowedip *allowedip = NULL;
	struct nlattr *peers_nest, *peer_nest, *allowedips_nest, *allowedip_nest;
	struct nlmsghdr *nlh;
	struct mnlg_socket *nlg;

	nlg = mnlg_socket_open(WG_GENL_NAME, WG_GENL_VERSION);
	if (!nlg)
		return -errno;

again:
	nlh = mnlg_msg_prepare(nlg, WG_CMD_SET_DEVICE, NLM_F_REQUEST | NLM_F_ACK);
	mnl_attr_put_strz(nlh, WGDEVICE_A_IFNAME, dev->name);

	if (!peer) {
		uint32_t flags = 0;

		if (dev->flags & WGDEVICE_HAS_PRIVATE_KEY)
			mnl_attr_put(nlh, WGDEVICE_A_PRIVATE_KEY, sizeof(dev->private_key), dev->private_key);
		if (dev->flags & WGDEVICE_HAS_LISTEN_PORT)
			mnl_attr_put_u16(nlh, WGDEVICE_A_LISTEN_PORT, dev->listen_port);
		if (dev->flags & WGDEVICE_HAS_FWMARK)
			mnl_attr_put_u32(nlh, WGDEVICE_A_FWMARK, dev->fwmark);
		if (dev->flags & WGDEVICE_REPLACE_PEERS)
			flags |= WGDEVICE_F_REPLACE_PEERS;
		if (flags)
			mnl_attr_put_u32(nlh, WGDEVICE_A_FLAGS, flags);
	}
	if (!dev->first_peer)
		goto send;
	peers_nest = peer_nest = allowedips_nest = allowedip_nest = NULL;
	peers_nest = mnl_attr_nest_start(nlh, WGDEVICE_A_PEERS);
	for (peer = peer ? peer : dev->first_peer; peer; peer = peer->next_peer) {
		uint32_t flags = 0;

		peer_nest = mnl_attr_nest_start_check(nlh, SOCKET_BUFFER_SIZE, 0);
		if (!peer_nest)
			goto toobig_peers;
		if (!mnl_attr_put_check(nlh, SOCKET_BUFFER_SIZE, WGPEER_A_PUBLIC_KEY, sizeof(peer->public_key), peer->public_key))
			goto toobig_peers;
		if (peer->flags & WGPEER_REMOVE_ME)
			flags |= WGPEER_F_REMOVE_ME;
		if (!allowedip) {
			if (peer->flags & WGPEER_REPLACE_ALLOWEDIPS)
				flags |= WGPEER_F_REPLACE_ALLOWEDIPS;
			if (peer->flags & WGPEER_HAS_PRESHARED_KEY) {
				if (!mnl_attr_put_check(nlh, SOCKET_BUFFER_SIZE, WGPEER_A_PRESHARED_KEY, sizeof(peer->preshared_key), peer->preshared_key))
					goto toobig_peers;
			}
			if (peer->endpoint.addr.sa_family == AF_INET) {
				if (!mnl_attr_put_check(nlh, SOCKET_BUFFER_SIZE, WGPEER_A_ENDPOINT, sizeof(peer->endpoint.addr4), &peer->endpoint.addr4))
					goto toobig_peers;
			} else if (peer->endpoint.addr.sa_family == AF_INET6) {
				if (!mnl_attr_put_check(nlh, SOCKET_BUFFER_SIZE, WGPEER_A_ENDPOINT, sizeof(peer->endpoint.addr6), &peer->endpoint.addr6))
					goto toobig_peers;
			}
			if (peer->flags & WGPEER_HAS_PERSISTENT_KEEPALIVE_INTERVAL) {
				if (!mnl_attr_put_u16_check(nlh, SOCKET_BUFFER_SIZE, WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL, peer->persistent_keepalive_interval))
					goto toobig_peers;
			}
		}
		if (flags) {
			if (!mnl_attr_put_u32_check(nlh, SOCKET_BUFFER_SIZE, WGPEER_A_FLAGS, flags))
				goto toobig_peers;
		}
		if (peer->first_allowedip) {
			if (!allowedip)
				allowedip = peer->first_allowedip;
			allowedips_nest = mnl_attr_nest_start_check(nlh, SOCKET_BUFFER_SIZE, WGPEER_A_ALLOWEDIPS);
			if (!allowedips_nest)
				goto toobig_allowedips;
			for (; allowedip; allowedip = allowedip->next_allowedip) {
				allowedip_nest = mnl_attr_nest_start_check(nlh, SOCKET_BUFFER_SIZE, 0);
				if (!allowedip_nest)
					goto toobig_allowedips;
				if (!mnl_attr_put_u16_check(nlh, SOCKET_BUFFER_SIZE, WGALLOWEDIP_A_FAMILY, allowedip->family))
					goto toobig_allowedips;
				if (allowedip->family == AF_INET) {
					if (!mnl_attr_put_check(nlh, SOCKET_BUFFER_SIZE, WGALLOWEDIP_A_IPADDR, sizeof(allowedip->ip4), &allowedip->ip4))
						goto toobig_allowedips;
				} else if (allowedip->family == AF_INET6) {
					if (!mnl_attr_put_check(nlh, SOCKET_BUFFER_SIZE, WGALLOWEDIP_A_IPADDR, sizeof(allowedip->ip6), &allowedip->ip6))
						goto toobig_allowedips;
				}
				if (!mnl_attr_put_u8_check(nlh, SOCKET_BUFFER_SIZE, WGALLOWEDIP_A_CIDR_MASK, allowedip->cidr))
					goto toobig_allowedips;
				mnl_attr_nest_end(nlh, allowedip_nest);
				allowedip_nest = NULL;
			}
			mnl_attr_nest_end(nlh, allowedips_nest);
			allowedips_nest = NULL;
		}

		mnl_attr_nest_end(nlh, peer_nest);
		peer_nest = NULL;
	}
	mnl_attr_nest_end(nlh, peers_nest);
	peers_nest = NULL;
	goto send;
toobig_allowedips:
	if (allowedip_nest)
		mnl_attr_nest_cancel(nlh, allowedip_nest);
	if (allowedips_nest)
		mnl_attr_nest_end(nlh, allowedips_nest);
	mnl_attr_nest_end(nlh, peer_nest);
	mnl_attr_nest_end(nlh, peers_nest);
	goto send;
toobig_peers:
	if (peer_nest)
		mnl_attr_nest_cancel(nlh, peer_nest);
	mnl_attr_nest_end(nlh, peers_nest);
	goto send;
send:
	if (mnlg_socket_send(nlg, nlh) < 0) {
		ret = -errno;
		goto out;
	}
	errno = 0;
	if (mnlg_socket_recv_run(nlg, NULL, NULL) < 0) {
		ret = errno ? -errno : -EINVAL;
		goto out;
	}
	if (peer)
		goto again;

out:
	mnlg_socket_close(nlg);
	errno = -ret;
	return ret;
}

static int parse_allowedip(const struct nlattr *attr, void *data)
{
	struct wgallowedip *allowedip = data;

	switch (mnl_attr_get_type(attr)) {
	case WGALLOWEDIP_A_UNSPEC:
		break;
	case WGALLOWEDIP_A_FAMILY:
		if (!mnl_attr_validate(attr, MNL_TYPE_U16))
			allowedip->family = mnl_attr_get_u16(attr);
		break;
	case WGALLOWEDIP_A_IPADDR:
		if (mnl_attr_get_payload_len(attr) == sizeof(allowedip->ip4))
			memcpy(&allowedip->ip4, mnl_attr_get_payload(attr), sizeof(allowedip->ip4));
		else if (mnl_attr_get_payload_len(attr) == sizeof(allowedip->ip6))
			memcpy(&allowedip->ip6, mnl_attr_get_payload(attr), sizeof(allowedip->ip6));
		break;
	case WGALLOWEDIP_A_CIDR_MASK:
		if (!mnl_attr_validate(attr, MNL_TYPE_U8))
			allowedip->cidr = mnl_attr_get_u8(attr);
		break;
	}

	return MNL_CB_OK;
}

static int parse_allowedips(const struct nlattr *attr, void *data)
{
	struct wgpeer *peer = data;
	struct wgallowedip *new_allowedip = calloc(1, sizeof(*new_allowedip));
	int ret;

	if (!new_allowedip) {
		perror("calloc");
		return MNL_CB_ERROR;
	}
	if (!peer->first_allowedip)
		peer->first_allowedip = peer->last_allowedip = new_allowedip;
	else {
		peer->last_allowedip->next_allowedip = new_allowedip;
		peer->last_allowedip = new_allowedip;
	}
	ret = mnl_attr_parse_nested(attr, parse_allowedip, new_allowedip);
	if (!ret)
		return ret;
	if (!((new_allowedip->family == AF_INET && new_allowedip->cidr <= 32) || (new_allowedip->family == AF_INET6 && new_allowedip->cidr <= 128)))
		return MNL_CB_ERROR;
	return MNL_CB_OK;
}

static int parse_peer(const struct nlattr *attr, void *data)
{
	struct wgpeer *peer = data;

	switch (mnl_attr_get_type(attr)) {
	case WGPEER_A_UNSPEC:
		break;
	case WGPEER_A_PUBLIC_KEY:
		if (mnl_attr_get_payload_len(attr) == sizeof(peer->public_key)) {
			memcpy(peer->public_key, mnl_attr_get_payload(attr), sizeof(peer->public_key));
			peer->flags |= WGPEER_HAS_PUBLIC_KEY;
		}
		break;
	case WGPEER_A_PRESHARED_KEY:
		if (mnl_attr_get_payload_len(attr) == sizeof(peer->preshared_key)) {
			memcpy(peer->preshared_key, mnl_attr_get_payload(attr), sizeof(peer->preshared_key));
			if (!key_is_zero(peer->preshared_key))
				peer->flags |= WGPEER_HAS_PRESHARED_KEY;
		}
		break;
	case WGPEER_A_ENDPOINT: {
		struct sockaddr *addr;

		if (mnl_attr_get_payload_len(attr) < sizeof(*addr))
			break;
		addr = mnl_attr_get_payload(attr);
		if (addr->sa_family == AF_INET && mnl_attr_get_payload_len(attr) == sizeof(peer->endpoint.addr4))
			memcpy(&peer->endpoint.addr4, addr, sizeof(peer->endpoint.addr4));
		else if (addr->sa_family == AF_INET6 && mnl_attr_get_payload_len(attr) == sizeof(peer->endpoint.addr6))
			memcpy(&peer->endpoint.addr6, addr, sizeof(peer->endpoint.addr6));
		break;
	}
	case WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL:
		if (!mnl_attr_validate(attr, MNL_TYPE_U16))
			peer->persistent_keepalive_interval = mnl_attr_get_u16(attr);
		break;
	case WGPEER_A_LAST_HANDSHAKE_TIME:
		if (mnl_attr_get_payload_len(attr) == sizeof(peer->last_handshake_time))
			memcpy(&peer->last_handshake_time, mnl_attr_get_payload(attr), sizeof(peer->last_handshake_time));
		break;
	case WGPEER_A_RX_BYTES:
		if (!mnl_attr_validate(attr, MNL_TYPE_U64))
			peer->rx_bytes = mnl_attr_get_u64(attr);
		break;
	case WGPEER_A_TX_BYTES:
		if (!mnl_attr_validate(attr, MNL_TYPE_U64))
			peer->tx_bytes = mnl_attr_get_u64(attr);
		break;
	case WGPEER_A_ALLOWEDIPS:
		return mnl_attr_parse_nested(attr, parse_allowedips, peer);
	}

	return MNL_CB_OK;
}

static int parse_peers(const struct nlattr *attr, void *data)
{
	struct wgdevice *device = data;
	struct wgpeer *new_peer = calloc(1, sizeof(*new_peer));
	int ret;

	if (!new_peer) {
		perror("calloc");
		return MNL_CB_ERROR;
	}
	if (!device->first_peer)
		device->first_peer = device->last_peer = new_peer;
	else {
		device->last_peer->next_peer = new_peer;
		device->last_peer = new_peer;
	}
	ret = mnl_attr_parse_nested(attr, parse_peer, new_peer);
	if (!ret)
		return ret;
	if (!(new_peer->flags & WGPEER_HAS_PUBLIC_KEY))
		return MNL_CB_ERROR;
	return MNL_CB_OK;
}

static int parse_device(const struct nlattr *attr, void *data)
{
	struct wgdevice *device = data;

	switch (mnl_attr_get_type(attr)) {
	case WGDEVICE_A_UNSPEC:
		break;
	case WGDEVICE_A_IFINDEX:
		if (!mnl_attr_validate(attr, MNL_TYPE_U32))
			device->ifindex = mnl_attr_get_u32(attr);
		break;
	case WGDEVICE_A_IFNAME:
		if (!mnl_attr_validate(attr, MNL_TYPE_STRING)) {
			strncpy(device->name, mnl_attr_get_str(attr), sizeof(device->name) - 1);
			device->name[sizeof(device->name) - 1] = '\0';
		}
		break;
	case WGDEVICE_A_PRIVATE_KEY:
		if (mnl_attr_get_payload_len(attr) == sizeof(device->private_key)) {
			memcpy(device->private_key, mnl_attr_get_payload(attr), sizeof(device->private_key));
			device->flags |= WGDEVICE_HAS_PRIVATE_KEY;
		}
		break;
	case WGDEVICE_A_PUBLIC_KEY:
		if (mnl_attr_get_payload_len(attr) == sizeof(device->public_key)) {
			memcpy(device->public_key, mnl_attr_get_payload(attr), sizeof(device->public_key));
			device->flags |= WGDEVICE_HAS_PUBLIC_KEY;
		}
		break;
	case WGDEVICE_A_LISTEN_PORT:
		if (!mnl_attr_validate(attr, MNL_TYPE_U16))
			device->listen_port = mnl_attr_get_u16(attr);
		break;
	case WGDEVICE_A_FWMARK:
		if (!mnl_attr_validate(attr, MNL_TYPE_U32))
			device->fwmark = mnl_attr_get_u32(attr);
		break;
	case WGDEVICE_A_PEERS:
		return mnl_attr_parse_nested(attr, parse_peers, device);
	}

	return MNL_CB_OK;
}

static int read_device_cb(const struct nlmsghdr *nlh, void *data)
{
	return mnl_attr_parse(nlh, sizeof(struct genlmsghdr), parse_device, data);
}

static void coalesce_peers(struct wgdevice *device)
{
	struct wgpeer *old_next_peer, *peer = device->first_peer;

	while (peer && peer->next_peer) {
		if (memcmp(peer->public_key, peer->next_peer->public_key, sizeof(peer->public_key))) {
			peer = peer->next_peer;
			continue;
		}
		if (!peer->first_allowedip) {
			peer->first_allowedip = peer->next_peer->first_allowedip;
			peer->last_allowedip = peer->next_peer->last_allowedip;
		} else {
			peer->last_allowedip->next_allowedip = peer->next_peer->first_allowedip;
			peer->last_allowedip = peer->next_peer->last_allowedip;
		}
		old_next_peer = peer->next_peer;
		peer->next_peer = old_next_peer->next_peer;
		free(old_next_peer);
	}
}

static int kernel_get_device(struct wgdevice **device, const char *iface)
{
	int ret;
	struct nlmsghdr *nlh;
	struct mnlg_socket *nlg;

try_again:
	ret = 0;
	*device = calloc(1, sizeof(**device));
	if (!*device)
		return -errno;

	nlg = mnlg_socket_open(WG_GENL_NAME, WG_GENL_VERSION);
	if (!nlg) {
		free_wgdevice(*device);
		*device = NULL;
		return -errno;
	}

	nlh = mnlg_msg_prepare(nlg, WG_CMD_GET_DEVICE, NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP);
	mnl_attr_put_strz(nlh, WGDEVICE_A_IFNAME, iface);
	if (mnlg_socket_send(nlg, nlh) < 0) {
		ret = -errno;
		goto out;
	}
	errno = 0;
	if (mnlg_socket_recv_run(nlg, read_device_cb, *device) < 0) {
		ret = errno ? -errno : -EINVAL;
		goto out;
	}
	coalesce_peers(*device);

out:
	if (nlg)
		mnlg_socket_close(nlg);
	if (ret) {
		free_wgdevice(*device);
		if (ret == -EINTR)
			goto try_again;
		*device = NULL;
	}
	errno = -ret;
	return ret;
}
#endif

#if defined(__OpenBSD__) || defined(__FreeBSD__)
static int get_dgram_socket(void)
{
	static int sock = -1;
	if (sock < 0)
		sock = socket(AF_INET, SOCK_DGRAM, 0);
	return sock;
}

static int kernel_get_wireguard_interfaces(struct string_list *list)
{
	struct ifgroupreq ifgr = { .ifgr_name = "wg" };
	struct ifg_req *ifg;
	int s = get_dgram_socket(), ret = 0;

	if (s < 0)
		return -errno;

	if (ioctl(s, SIOCGIFGMEMB, (caddr_t)&ifgr) < 0)
		return errno == ENOENT ? 0 : -errno;

	ifgr.ifgr_groups = calloc(1, ifgr.ifgr_len);
	if (!ifgr.ifgr_groups)
		return -errno;
	if (ioctl(s, SIOCGIFGMEMB, (caddr_t)&ifgr) < 0) {
		ret = -errno;
		goto out;
	}

	for (ifg = ifgr.ifgr_groups; ifg && ifgr.ifgr_len > 0; ++ifg) {
		if ((ret = string_list_add(list, ifg->ifgrq_member)) < 0)
			goto out;
		ifgr.ifgr_len -= sizeof(struct ifg_req);
	}

out:
	free(ifgr.ifgr_groups);
	return ret;
}
#endif

#if defined(__OpenBSD__)
static int kernel_get_device(struct wgdevice **device, const char *iface)
{
	struct wg_data_io wgdata = { .wgd_size = 0 };
	struct wg_interface_io *wg_iface;
	struct wg_peer_io *wg_peer;
	struct wg_aip_io *wg_aip;
	struct wgdevice *dev;
	struct wgpeer *peer;
	struct wgallowedip *aip;
	int s = get_dgram_socket(), ret;

	if (s < 0)
		return -errno;

	*device = NULL;
	strlcpy(wgdata.wgd_name, iface, sizeof(wgdata.wgd_name));
	for (size_t last_size = wgdata.wgd_size;; last_size = wgdata.wgd_size) {
		if (ioctl(s, SIOCGWG, (caddr_t)&wgdata) < 0)
			goto out;
		if (last_size >= wgdata.wgd_size)
			break;
		wgdata.wgd_interface = realloc(wgdata.wgd_interface, wgdata.wgd_size);
		if (!wgdata.wgd_interface)
			goto out;
	}

	wg_iface = wgdata.wgd_interface;
	dev = calloc(1, sizeof(*dev));
	if (!dev)
		goto out;
	strlcpy(dev->name, iface, sizeof(dev->name));

	if (wg_iface->i_flags & WG_INTERFACE_HAS_RTABLE) {
		dev->fwmark = wg_iface->i_rtable;
		dev->flags |= WGDEVICE_HAS_FWMARK;
	}

	if (wg_iface->i_flags & WG_INTERFACE_HAS_PORT) {
		dev->listen_port = wg_iface->i_port;
		dev->flags |= WGDEVICE_HAS_LISTEN_PORT;
	}

	if (wg_iface->i_flags & WG_INTERFACE_HAS_PUBLIC) {
		memcpy(dev->public_key, wg_iface->i_public, sizeof(dev->public_key));
		dev->flags |= WGDEVICE_HAS_PUBLIC_KEY;
	}

	if (wg_iface->i_flags & WG_INTERFACE_HAS_PRIVATE) {
		memcpy(dev->private_key, wg_iface->i_private, sizeof(dev->private_key));
		dev->flags |= WGDEVICE_HAS_PRIVATE_KEY;
	}

	wg_peer = &wg_iface->i_peers[0];
	for (size_t i = 0; i < wg_iface->i_peers_count; ++i) {
		peer = calloc(1, sizeof(*peer));
		if (!peer)
			goto out;

		if (dev->first_peer == NULL)
			dev->first_peer = peer;
		else
			dev->last_peer->next_peer = peer;
		dev->last_peer = peer;

		if (wg_peer->p_flags & WG_PEER_HAS_PUBLIC) {
			memcpy(peer->public_key, wg_peer->p_public, sizeof(peer->public_key));
			peer->flags |= WGPEER_HAS_PUBLIC_KEY;
		}

		if (wg_peer->p_flags & WG_PEER_HAS_PSK) {
			memcpy(peer->preshared_key, wg_peer->p_psk, sizeof(peer->preshared_key));
			peer->flags |= WGPEER_HAS_PRESHARED_KEY;
		}

		if (wg_peer->p_flags & WG_PEER_HAS_PKA) {
			peer->persistent_keepalive_interval = wg_peer->p_pka;
			peer->flags |= WGPEER_HAS_PERSISTENT_KEEPALIVE_INTERVAL;
		}

		if (wg_peer->p_flags & WG_PEER_HAS_ENDPOINT && wg_peer->p_sa.sa_len <= sizeof(peer->endpoint.addr))
			memcpy(&peer->endpoint.addr, &wg_peer->p_sa, wg_peer->p_sa.sa_len);

		peer->rx_bytes = wg_peer->p_rxbytes;
		peer->tx_bytes = wg_peer->p_txbytes;

		peer->last_handshake_time.tv_sec = wg_peer->p_last_handshake.tv_sec;
		peer->last_handshake_time.tv_nsec = wg_peer->p_last_handshake.tv_nsec;

		wg_aip = &wg_peer->p_aips[0];
		for (size_t j = 0; j < wg_peer->p_aips_count; ++j) {
			aip = calloc(1, sizeof(*aip));
			if (!aip)
				goto out;

			if (peer->first_allowedip == NULL)
				peer->first_allowedip = aip;
			else
				peer->last_allowedip->next_allowedip = aip;
			peer->last_allowedip = aip;

			aip->family = wg_aip->a_af;
			if (wg_aip->a_af == AF_INET) {
				memcpy(&aip->ip4, &wg_aip->a_ipv4, sizeof(aip->ip4));
				aip->cidr = wg_aip->a_cidr;
			} else if (wg_aip->a_af == AF_INET6) {
				memcpy(&aip->ip6, &wg_aip->a_ipv6, sizeof(aip->ip6));
				aip->cidr = wg_aip->a_cidr;
			}
			++wg_aip;
		}
		wg_peer = (struct wg_peer_io *)wg_aip;
	}
	*device = dev;
	errno = 0;
out:
	ret = -errno;
	free(wgdata.wgd_interface);
	return ret;
}

static int kernel_set_device(struct wgdevice *dev)
{
	struct wg_data_io wgdata = { .wgd_size = sizeof(struct wg_interface_io) };
	struct wg_interface_io *wg_iface;
	struct wg_peer_io *wg_peer;
	struct wg_aip_io *wg_aip;
	struct wgpeer *peer;
	struct wgallowedip *aip;
	int s = get_dgram_socket(), ret;
	size_t peer_count, aip_count;

	if (s < 0)
		return -errno;

	for_each_wgpeer(dev, peer) {
		wgdata.wgd_size += sizeof(struct wg_peer_io);
		for_each_wgallowedip(peer, aip)
			wgdata.wgd_size += sizeof(struct wg_aip_io);
	}
	wg_iface = wgdata.wgd_interface = calloc(1, wgdata.wgd_size);
	if (!wgdata.wgd_interface)
		return -errno;
	strlcpy(wgdata.wgd_name, dev->name, sizeof(wgdata.wgd_name));

	if (dev->flags & WGDEVICE_HAS_PRIVATE_KEY) {
		memcpy(wg_iface->i_private, dev->private_key, sizeof(wg_iface->i_private));
		wg_iface->i_flags |= WG_INTERFACE_HAS_PRIVATE;
	}

	if (dev->flags & WGDEVICE_HAS_LISTEN_PORT) {
		wg_iface->i_port = dev->listen_port;
		wg_iface->i_flags |= WG_INTERFACE_HAS_PORT;
	}

	if (dev->flags & WGDEVICE_HAS_FWMARK) {
		wg_iface->i_rtable = dev->fwmark;
		wg_iface->i_flags |= WG_INTERFACE_HAS_RTABLE;
	}

	if (dev->flags & WGDEVICE_REPLACE_PEERS)
		wg_iface->i_flags |= WG_INTERFACE_REPLACE_PEERS;

	peer_count = 0;
	wg_peer = &wg_iface->i_peers[0];
	for_each_wgpeer(dev, peer) {
		wg_peer->p_flags = WG_PEER_HAS_PUBLIC;
		memcpy(wg_peer->p_public, peer->public_key, sizeof(wg_peer->p_public));

		if (peer->flags & WGPEER_HAS_PRESHARED_KEY) {
			memcpy(wg_peer->p_psk, peer->preshared_key, sizeof(wg_peer->p_psk));
			wg_peer->p_flags |= WG_PEER_HAS_PSK;
		}

		if (peer->flags & WGPEER_HAS_PERSISTENT_KEEPALIVE_INTERVAL) {
			wg_peer->p_pka = peer->persistent_keepalive_interval;
			wg_peer->p_flags |= WG_PEER_HAS_PKA;
		}

		if ((peer->endpoint.addr.sa_family == AF_INET || peer->endpoint.addr.sa_family == AF_INET6) &&
		    peer->endpoint.addr.sa_len <= sizeof(wg_peer->p_endpoint)) {
			memcpy(&wg_peer->p_endpoint, &peer->endpoint.addr, peer->endpoint.addr.sa_len);
			wg_peer->p_flags |= WG_PEER_HAS_ENDPOINT;
		}

		if (peer->flags & WGPEER_REPLACE_ALLOWEDIPS)
			wg_peer->p_flags |= WG_PEER_REPLACE_AIPS;

		if (peer->flags & WGPEER_REMOVE_ME)
			wg_peer->p_flags |= WG_PEER_REMOVE;

		aip_count = 0;
		wg_aip = &wg_peer->p_aips[0];
		for_each_wgallowedip(peer, aip) {
			wg_aip->a_af = aip->family;
			wg_aip->a_cidr = aip->cidr;

			if (aip->family == AF_INET) {
				memcpy(&wg_aip->a_ipv4, &aip->ip4, sizeof(wg_aip->a_ipv4));
			} else if (aip->family == AF_INET6) {
				memcpy(&wg_aip->a_ipv6, &aip->ip6, sizeof(wg_aip->a_ipv6));
			} else {
				continue;
			}
			++aip_count;
			++wg_aip;
		}
		wg_peer->p_aips_count = aip_count;
		++peer_count;
		wg_peer = (struct wg_peer_io *)wg_aip;
	}
	wg_iface->i_peers_count = peer_count;

	if (ioctl(s, SIOCSWG, (caddr_t)&wgdata) < 0)
		goto out;
	errno = 0;

out:
	ret = -errno;
	free(wgdata.wgd_interface);
	return ret;
}
#endif

/* first\0second\0third\0forth\0last\0\0 */
char *ipc_list_devices(void)
{
	struct string_list list = { 0 };
	int ret;

#if defined(__linux__) || defined(__OpenBSD__) || defined(__FreeBSD__)
	ret = kernel_get_wireguard_interfaces(&list);
	if (ret < 0)
		goto cleanup;
#endif
	ret = userspace_get_wireguard_interfaces(&list);
	if (ret < 0)
		goto cleanup;

cleanup:
	errno = -ret;
	if (errno) {
		free(list.buffer);
		return NULL;
	}
	return list.buffer ?: strdup("\0");
}

#ifdef __FreeBSD__
#define BSD_INTERNAL
#include "ipc_freebsd.c"
#endif


int ipc_get_device(struct wgdevice **dev, const char *iface)
{
#if defined(__linux__) || defined(__OpenBSD__) || defined(__FreeBSD__)
	if (userspace_has_wireguard_interface(iface))
		return userspace_get_device(dev, iface);
	return kernel_get_device(dev, iface);
#else
	return userspace_get_device(dev, iface);
#endif
}

int ipc_set_device(struct wgdevice *dev)
{
#if defined(__linux__) || defined(__OpenBSD__) || defined(__FreeBSD__)
	if (userspace_has_wireguard_interface(dev->name))
		return userspace_set_device(dev);
	return kernel_set_device(dev);
#else
	return userspace_set_device(dev);
#endif
}
