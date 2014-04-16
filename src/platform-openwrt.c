/*
 * Author: Steven Barth <steven@midlink.org>
 *
 * Copyright (c) 2014 cisco Systems, Inc.
 */

#include <errno.h>
#include <assert.h>

#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <resolv.h>

#include <sys/un.h>
#include <sys/socket.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#include "dhcpv6.h"
#include "dhcp.h"
#include "platform.h"
#include "iface.h"

static struct ubus_context *ubus = NULL;
static struct ubus_subscriber netifd;
static uint32_t ubus_network_interface = 0;
static struct pa_data *pa_data = NULL;

static int handle_update(__unused struct ubus_context *ctx,
		__unused struct ubus_object *obj, __unused struct ubus_request_data *req,
		__unused const char *method, struct blob_attr *msg);
static void handle_dump(__unused struct ubus_request *req,
		__unused int type, struct blob_attr *msg);

static struct ubus_request req_dump = { .list = LIST_HEAD_INIT(req_dump.list) };

static void platform_commit(struct uloop_timeout *t);
struct platform_iface {
	struct iface *iface;
	struct uloop_timeout update;
	struct ubus_request req;
	char handle[];
};


/* ubus subscribe / handle control code */
static void sync_netifd(bool subscribe)
{
	if (subscribe)
		ubus_subscribe(ubus, &netifd, ubus_network_interface);

	ubus_abort_request(ubus, &req_dump);
	if (!ubus_invoke_async(ubus, ubus_network_interface, "dump", NULL, &req_dump)) {
		req_dump.data_cb = handle_dump;
		ubus_complete_request_async(ubus, &req_dump);
	}
}

enum {
	OBJ_ATTR_ID,
	OBJ_ATTR_PATH,
	OBJ_ATTR_MAX
};

static const struct blobmsg_policy obj_attrs[OBJ_ATTR_MAX] = {
	[OBJ_ATTR_ID] = { .name = "id", .type = BLOBMSG_TYPE_INT32 },
	[OBJ_ATTR_PATH] = { .name = "path", .type = BLOBMSG_TYPE_STRING },
};

static void handle_event(__unused struct ubus_context *ctx, __unused struct ubus_event_handler *ev,
                __unused const char *type, struct blob_attr *msg)
{
	struct blob_attr *tb[OBJ_ATTR_MAX];
	blobmsg_parse(obj_attrs, OBJ_ATTR_MAX, tb, blob_data(msg), blob_len(msg));

	if (!tb[OBJ_ATTR_ID] || !tb[OBJ_ATTR_PATH])
		return;

	if (strcmp(blobmsg_get_string(tb[OBJ_ATTR_PATH]), "network.interface"))
		return;

	ubus_network_interface = blobmsg_get_u32(tb[OBJ_ATTR_ID]);
	iface_flush();
	sync_netifd(true);
}
static struct ubus_event_handler event_handler = { .cb = handle_event };
static const char *hnetd_pd_socket = NULL;

int platform_init(struct pa_data *data, const char *pd_socket)
{
	if (!(ubus = ubus_connect(NULL))) {
		L_ERR("Failed to connect to ubus: %s", strerror(errno));
		return -1;
	}

	netifd.cb = handle_update;
	ubus_register_subscriber(ubus, &netifd);

	ubus_add_uloop(ubus);
	ubus_register_event_handler(ubus, &event_handler, "ubus.object.add");
	if (!ubus_lookup_id(ubus, "network.interface", &ubus_network_interface))
		sync_netifd(true);

	hnetd_pd_socket = pd_socket;
	pa_data = data;
	return 0;
}

// Constructor for openwrt-specific interface part
void platform_iface_new(struct iface *c, const char *handle)
{
	assert(c->platform == NULL);

	size_t handlenamelen = strlen(handle) + 1;
	struct platform_iface *iface = calloc(1, sizeof(*iface) + handlenamelen);
	memcpy(iface->handle, handle, handlenamelen);
	iface->iface = c;
	iface->update.cb = platform_commit;

	c->platform = iface;

	// Have to rerun dump here as to sync up on nested interfaces
	sync_netifd(false);

	// reqiest
	INIT_LIST_HEAD(&iface->req.list);
}

// Destructor for openwrt-specific interface part
void platform_iface_free(struct iface *c)
{
	struct platform_iface *iface = c->platform;
	if (iface) {
		uloop_timeout_cancel(&iface->update);
		ubus_abort_request(ubus, &iface->req);
		free(iface);
		c->platform = NULL;
	}
}

void platform_set_internal(struct iface *c,
		__unused bool internal)
{
	struct platform_iface *iface = c->platform;
	assert(iface);
	uloop_timeout_set(&iface->update, 100);
}

void platform_set_address(struct iface *c,
		__unused struct iface_addr *addr, __unused bool enable)
{
	platform_set_internal(c, false);
}

void platform_set_route(struct iface *c,
		__unused struct iface_route *route, __unused bool enable)
{
	platform_set_internal(c, false);
}


void platform_set_owner(struct iface *c,
		__unused bool enable)
{
	platform_set_internal(c, false);
}


void platform_set_dhcpv6_send(struct iface *c,
		__unused const void *dhcpv6_data, __unused size_t len,
		__unused const void *dhcp_data, __unused size_t len4)
{
	platform_set_internal(c, false);
}


void platform_filter_prefix(struct iface *c,
		__unused const struct prefix *p, __unused bool enable)
{
	platform_set_internal(c, false);
}


void platform_set_prefix_route(const struct prefix *p, bool enable)
{
	iface_set_unreachable_route(p, enable);
}


// Handle netifd ubus event for interfaces updates
static void handle_complete(struct ubus_request *req, int ret)
{
	struct platform_iface *iface = container_of(req, struct platform_iface, req);
	L_INFO("platform: async notify_proto for %s: %s", iface->handle, ubus_strerror(ret));
}


// Commit platform changes to netifd
static void platform_commit(struct uloop_timeout *t)
{
	struct platform_iface *iface = container_of(t, struct platform_iface, update);
	struct iface *c = iface->iface;

	struct blob_buf b = {NULL, NULL, 0, NULL};
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "action", 0);
	blobmsg_add_u8(&b, "link-up", 1);
	blobmsg_add_string(&b, "interface", iface->handle);

	L_DEBUG("platform: *** begin interface update %s (%s)", iface->handle, c->ifname);

	void *k, *l, *m;
	struct iface_addr *a;
	struct iface_route *r;

	k = blobmsg_open_array(&b, "ipaddr");
	vlist_for_each_element(&c->assigned, a, node) {
		if (!IN6_IS_ADDR_V4MAPPED(&a->prefix.prefix))
			continue;

		l = blobmsg_open_table(&b, NULL);

		char *buf = blobmsg_alloc_string_buffer(&b, "ipaddr", INET_ADDRSTRLEN);
		inet_ntop(AF_INET, &a->prefix.prefix.s6_addr[12], buf, INET_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);

		L_DEBUG("	%s/%u", buf, prefix_af_length(&a->prefix));

		buf = blobmsg_alloc_string_buffer(&b, "mask", 4);
		snprintf(buf, 4, "%u", prefix_af_length(&a->prefix));
		blobmsg_add_string_buffer(&b);

		blobmsg_close_table(&b, l);
	}
	blobmsg_close_array(&b, k);

	hnetd_time_t now = hnetd_time();
	k = blobmsg_open_array(&b, "ip6addr");
	vlist_for_each_element(&c->assigned, a, node) {
		hnetd_time_t preferred = (a->preferred_until - now) / HNETD_TIME_PER_SECOND;
		hnetd_time_t valid = (a->valid_until - now) / HNETD_TIME_PER_SECOND;
		if (IN6_IS_ADDR_V4MAPPED(&a->prefix.prefix) || valid <= 0)
			continue;

		if (preferred < 0)
			preferred = 0;
		else if (preferred > UINT32_MAX)
			preferred = UINT32_MAX;

		if (valid > UINT32_MAX)
			valid = UINT32_MAX;

		l = blobmsg_open_table(&b, NULL);

		char *buf = blobmsg_alloc_string_buffer(&b, "ipaddr", INET6_ADDRSTRLEN);
		inet_ntop(AF_INET6, &a->prefix.prefix, buf, INET6_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);

		L_DEBUG("	%s/%u (%lld/%lld)", buf, prefix_af_length(&a->prefix),
				(long long)preferred, (long long)valid);

		buf = blobmsg_alloc_string_buffer(&b, "mask", 4);
		snprintf(buf, 4, "%u", prefix_af_length(&a->prefix));
		blobmsg_add_string_buffer(&b);

		blobmsg_add_u32(&b, "preferred", preferred);
		blobmsg_add_u32(&b, "valid", valid);
		blobmsg_add_u8(&b, "offlink", true);

		uint8_t *oend = &a->dhcpv6_data[a->dhcpv6_len], *odata;
		uint16_t olen, otype;
		dhcpv6_for_each_option(a->dhcpv6_data, oend, otype, olen, odata) {
#ifdef EXT_PREFIX_CLASS
			if (otype == DHCPV6_OPT_PREFIX_CLASS && olen == 2) {
				uint16_t class = (uint16_t)odata[0] << 8 | (uint16_t)odata[1];
				char *buf = blobmsg_alloc_string_buffer(&b, "class", 6);
				snprintf(buf, 6, "%u", class);
				blobmsg_add_string_buffer(&b);
			}
#endif
		}

		blobmsg_close_table(&b, l);
	}
	blobmsg_close_array(&b, k);

	k = blobmsg_open_array(&b, "routes");
	vlist_for_each_element(&c->routes, r, node) {
		if (!IN6_IS_ADDR_V4MAPPED(&r->to.prefix))
			continue;

		l = blobmsg_open_table(&b, NULL);

		char *buf = blobmsg_alloc_string_buffer(&b, "target", INET_ADDRSTRLEN);
		inet_ntop(AF_INET, &r->to.prefix.s6_addr[12], buf, INET_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);

		char *buf2 = blobmsg_alloc_string_buffer(&b, "netmask", 4);
		snprintf(buf2, 4, "%u", prefix_af_length(&r->to));
		blobmsg_add_string_buffer(&b);

		char *buf3 = blobmsg_alloc_string_buffer(&b, "gateway", INET_ADDRSTRLEN);
		inet_ntop(AF_INET, &r->via.s6_addr[12], buf3, INET_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);

		blobmsg_add_u32(&b, "metric", r->metric);

		L_DEBUG("	to %s/%s via %s", buf, buf2, buf3);

		blobmsg_close_table(&b, l);
	}
	blobmsg_close_array(&b, k);

	k = blobmsg_open_array(&b, "routes6");
	vlist_for_each_element(&c->routes, r, node) {
		if (IN6_IS_ADDR_V4MAPPED(&r->to.prefix))
			continue;

		l = blobmsg_open_table(&b, NULL);

		char *buf = blobmsg_alloc_string_buffer(&b, "target", INET6_ADDRSTRLEN);
		inet_ntop(AF_INET6, &r->to.prefix, buf, INET6_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);

		char *buf2 = blobmsg_alloc_string_buffer(&b, "netmask", 4);
		snprintf(buf2, 4, "%u", prefix_af_length(&r->to));
		blobmsg_add_string_buffer(&b);

		char *buf3 = blobmsg_alloc_string_buffer(&b, "gateway", INET6_ADDRSTRLEN);
		inet_ntop(AF_INET6, &r->via, buf3, INET6_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);

		char *buf4 = blobmsg_alloc_string_buffer(&b, "source", PREFIX_MAXBUFFLEN);
		prefix_ntop(buf4, PREFIX_MAXBUFFLEN, &r->from, true);
		blobmsg_add_string_buffer(&b);

		blobmsg_add_u32(&b, "metric", r->metric);

		L_DEBUG("	from %s to %s/%s via %s", buf4, buf, buf2, buf3);

		blobmsg_close_table(&b, l);
	}
	vlist_for_each_element(&c->assigned, a, node) {
		hnetd_time_t preferred = (a->preferred_until - now) / HNETD_TIME_PER_SECOND;
		hnetd_time_t valid = (a->valid_until - now) / HNETD_TIME_PER_SECOND;
		if (IN6_IS_ADDR_V4MAPPED(&a->prefix.prefix) || valid <= 0 ||
				(preferred <= 0 && valid <= 7200))
			continue;

		l = blobmsg_open_table(&b, NULL);

		char *buf = blobmsg_alloc_string_buffer(&b, "target", INET6_ADDRSTRLEN);
		inet_ntop(AF_INET6, &a->prefix.prefix, buf, INET6_ADDRSTRLEN);
		blobmsg_add_string_buffer(&b);

		char *buf2 = blobmsg_alloc_string_buffer(&b, "netmask", 4);
		snprintf(buf2, 4, "%u", prefix_af_length(&a->prefix));
		blobmsg_add_string_buffer(&b);

		L_DEBUG("	on-link %s/%s", buf, buf2);

		blobmsg_close_table(&b, l);
	}
	blobmsg_close_array(&b, k);


	// DNS options
	const size_t dns_max = 4;
	size_t dns_cnt = 0, dns4_cnt = 0, domain_cnt = 0;
	struct in6_addr dns[dns_max];
	struct in_addr dns4[dns_max];
	char domains[dns_max][256];

	// Add per interface DHCPv6 options
	uint8_t *oend = ((uint8_t*)c->dhcpv6_data_out) + c->dhcpv6_len_out, *odata;
	uint16_t olen, otype;
	dhcpv6_for_each_option(c->dhcpv6_data_out, oend, otype, olen, odata) {
		if (otype == DHCPV6_OPT_DNS_SERVERS) {
			size_t cnt = olen / sizeof(*dns);
			if (cnt + dns_cnt > dns_max)
				cnt = dns_max - dns_cnt;

			memcpy(&dns[dns_cnt], odata, cnt * sizeof(*dns));
			dns_cnt += cnt;
		} else if (otype == DHCPV6_OPT_DNS_DOMAIN) {
			uint8_t *oend = &odata[olen];
			while (odata < oend && domain_cnt < dns_max) {
				int l = dn_expand(odata, oend, odata, domains[domain_cnt], sizeof(*domains));
				if (l > 0) {
					++domain_cnt;
					odata += l;
				} else {
					break;
				}
			}
		}
	}

	// Add per interface DHCP options
	uint8_t *o4end = ((uint8_t*)c->dhcp_data_out) + c->dhcp_len_out;
	struct dhcpv4_option *opt;
	dhcpv4_for_each_option(c->dhcp_data_out, o4end, opt) {
		if (opt->type == DHCPV4_OPT_DNSSERVER) {
			size_t cnt = opt->len / sizeof(*dns4);
			if (cnt + dns4_cnt > dns_max)
				cnt = dns_max - dns_cnt;

			memcpy(&dns4[dns4_cnt], opt->data, cnt * sizeof(*dns4));
			dns4_cnt += cnt;
		}
	}

	if (dns_cnt || dns4_cnt) {
		k = blobmsg_open_array(&b, "dns");

		for (size_t i = 0; i < dns_cnt; ++i) {
			char *buf = blobmsg_alloc_string_buffer(&b, NULL, INET6_ADDRSTRLEN);
			inet_ntop(AF_INET6, &dns[i], buf, INET6_ADDRSTRLEN);
			blobmsg_add_string_buffer(&b);
			L_DEBUG("	DNS: %s", buf);
		}

		for (size_t i = 0; i < dns4_cnt; ++i) {
			char *buf = blobmsg_alloc_string_buffer(&b, NULL, INET_ADDRSTRLEN);
			inet_ntop(AF_INET, &dns4[i], buf, INET_ADDRSTRLEN);
			blobmsg_add_string_buffer(&b);
			L_DEBUG("	DNS: %s", buf);
		}

		blobmsg_close_array(&b, k);
	}

	k = blobmsg_open_table(&b, "data");

	const char *service = (c->internal && c->linkowner) ? "server" : "disabled";
	blobmsg_add_string(&b, "ra", service);
	blobmsg_add_string(&b, "dhcpv4", service);
	blobmsg_add_string(&b, "dhcpv6", service);
	blobmsg_add_u32(&b, "ra_management", 1);


	if (c->internal && c->linkowner)
		blobmsg_add_string(&b, "pd_manager", hnetd_pd_socket);

	const char *zone = (c->internal ||
			((c->flags & IFACE_FLAG_ACCEPT_CERID) && !IN6_IS_ADDR_UNSPECIFIED(&c->cer))) ? "lan" : "wan";
	blobmsg_add_string(&b, "zone", zone);

	L_DEBUG("	RA/DHCP/DHCPv6: %s, Zone: %s", service, zone);

	if (domain_cnt && c->internal && c->linkowner) {
		l = blobmsg_open_array(&b, "domain");

		for (size_t i = 0; i < domain_cnt; ++i)
			blobmsg_add_string(&b, NULL, domains[i]);

		blobmsg_close_array(&b, l);
	}

	if (c->flags & IFACE_FLAG_GUEST) {
		if (dns_cnt || dns4_cnt) {
			l = blobmsg_open_array(&b, "dns");

			for (size_t i = 0; i < dns_cnt; ++i) {
				char *buf = blobmsg_alloc_string_buffer(&b, NULL, INET6_ADDRSTRLEN);
				inet_ntop(AF_INET6, &dns[i], buf, INET6_ADDRSTRLEN);
				blobmsg_add_string_buffer(&b);
				L_DEBUG("	DNS: %s", buf);
			}

			for (size_t i = 0; i < dns4_cnt; ++i) {
				char *buf = blobmsg_alloc_string_buffer(&b, NULL, INET_ADDRSTRLEN);
				inet_ntop(AF_INET, &dns4[i], buf, INET_ADDRSTRLEN);
				blobmsg_add_string_buffer(&b);
				L_DEBUG("	DNS: %s", buf);
			}

			blobmsg_close_array(&b, l);
		}

		l = blobmsg_open_array(&b, "firewall");

		m = blobmsg_open_table(&b, NULL);

		blobmsg_add_string(&b, "type", "rule");
		blobmsg_add_string(&b, "proto", "all");
		blobmsg_add_string(&b, "src", zone);
		blobmsg_add_string(&b, "direction", "in");
		blobmsg_add_string(&b, "target", "REJECT");
		blobmsg_add_string(&b, "family", "any");

		blobmsg_close_table(&b, m);

		struct pa_dp *dp;
		pa_for_each_dp(dp, pa_data) {
			for (int i = 0; i <= 1; ++i) {
				m = blobmsg_open_table(&b, NULL);

				blobmsg_add_string(&b, "type", "rule");
				blobmsg_add_string(&b, "proto", "all");
				blobmsg_add_string(&b, "src", (i) ? zone : "*");
				blobmsg_add_string(&b, "dest", (i) ? "*" : zone);
				blobmsg_add_string(&b, "direction", (i) ? "in" : "out");
				blobmsg_add_string(&b, "target", "REJECT");

				const char *family = IN6_IS_ADDR_V4MAPPED(&dp->prefix.prefix) ? "inet" : "inet6";
				blobmsg_add_string(&b, "family", family);

				char *buf = blobmsg_alloc_string_buffer(&b, (i) ? "dest_ip" : "src_ip", PREFIX_MAXBUFFLEN);
				prefix_ntop(buf, PREFIX_MAXBUFFLEN, &dp->prefix, true);
				blobmsg_add_string_buffer(&b);

				blobmsg_close_table(&b, m);
			}
		}
		blobmsg_close_array(&b, l);
	}

	blobmsg_close_table(&b, k);

	L_DEBUG("platform: *** end interface update %s (%s)", iface->handle, c->ifname);

	int ret;
	ubus_abort_request(ubus, &iface->req);
	if (!(ret = ubus_invoke_async(ubus, ubus_network_interface, "notify_proto", b.head, &iface->req))) {
		iface->req.complete_cb = handle_complete;
		ubus_complete_request_async(ubus, &iface->req);
	} else {
		L_INFO("platform: async notify_proto for %s (%s) failed: %s", iface->handle, c->ifname, ubus_strerror(ret));
		platform_set_internal(c, false);
	}

	blob_buf_free(&b);
}


enum {
	PREFIX_ATTR_ADDRESS,
	PREFIX_ATTR_MASK,
	PREFIX_ATTR_VALID,
	PREFIX_ATTR_PREFERRED,
	PREFIX_ATTR_EXCLUDED,
	PREFIX_ATTR_CLASS,
	PREFIX_ATTR_MAX,
};


static const struct blobmsg_policy prefix_attrs[PREFIX_ATTR_MAX] = {
	[PREFIX_ATTR_ADDRESS] = { .name = "address", .type = BLOBMSG_TYPE_STRING },
	[PREFIX_ATTR_MASK] = { .name = "mask", .type = BLOBMSG_TYPE_INT32 },
	[PREFIX_ATTR_PREFERRED] = { .name = "preferred", .type = BLOBMSG_TYPE_INT32 },
	[PREFIX_ATTR_EXCLUDED] = { .name = "excluded", .type = BLOBMSG_TYPE_STRING },
	[PREFIX_ATTR_VALID] = { .name = "valid", .type = BLOBMSG_TYPE_INT32 },
	[PREFIX_ATTR_CLASS] = { .name = "class", .type = BLOBMSG_TYPE_STRING },
};



enum {
	IFACE_ATTR_HANDLE,
	IFACE_ATTR_IFNAME,
	IFACE_ATTR_PROTO,
	IFACE_ATTR_PREFIX,
	IFACE_ATTR_ROUTE,
	IFACE_ATTR_DELEGATION,
	IFACE_ATTR_DNS,
	IFACE_ATTR_UP,
	IFACE_ATTR_DATA,
	IFACE_ATTR_MAX,
};

enum {
	ROUTE_ATTR_TARGET,
	ROUTE_ATTR_MASK,
	ROUTE_ATTR_MAX
};

enum {
	DATA_ATTR_ACCEPT_CERID,
	DATA_ATTR_CER,
	DATA_ATTR_GUEST,
	DATA_ATTR_MAX
};

static const struct blobmsg_policy iface_attrs[IFACE_ATTR_MAX] = {
	[IFACE_ATTR_HANDLE] = { .name = "interface", .type = BLOBMSG_TYPE_STRING },
	[IFACE_ATTR_IFNAME] = { .name = "l3_device", .type = BLOBMSG_TYPE_STRING },
	[IFACE_ATTR_PROTO] = { .name = "proto", .type = BLOBMSG_TYPE_STRING },
	[IFACE_ATTR_PREFIX] = { .name = "ipv6-prefix", .type = BLOBMSG_TYPE_ARRAY },
	[IFACE_ATTR_ROUTE] = { .name = "route", .type = BLOBMSG_TYPE_ARRAY },
	[IFACE_ATTR_DELEGATION] = { .name = "delegation", .type = BLOBMSG_TYPE_BOOL },
	[IFACE_ATTR_DNS] = { .name = "dns-server", .type = BLOBMSG_TYPE_ARRAY },
	[IFACE_ATTR_UP] = { .name = "up", .type = BLOBMSG_TYPE_BOOL },
	[IFACE_ATTR_DATA] = { .name = "data", .type = BLOBMSG_TYPE_TABLE },
};

static const struct blobmsg_policy route_attrs[ROUTE_ATTR_MAX] = {
	[ROUTE_ATTR_TARGET] = { .name = "target", .type = BLOBMSG_TYPE_STRING },
	[ROUTE_ATTR_MASK] = { .name = "mask", .type = BLOBMSG_TYPE_INT32 },
};

static const struct blobmsg_policy data_attrs[DATA_ATTR_MAX] = {
	[DATA_ATTR_ACCEPT_CERID] = { .name = "accept_cerid", .type = BLOBMSG_TYPE_BOOL },
	[DATA_ATTR_CER] = { .name = "cer", .type = BLOBMSG_TYPE_STRING },
	[DATA_ATTR_GUEST] = { .name = "guest", .type = BLOBMSG_TYPE_BOOL },
};


// Decode and analyze all delegated prefixes and commit them to iface
static void update_interface(struct iface *c,
		struct blob_attr *tb[IFACE_ATTR_MAX], bool v4uplink, bool v6uplink)
{
	hnetd_time_t now = hnetd_time();
	struct blob_attr *k;
	unsigned rem;

	if (v6uplink) {
		blobmsg_for_each_attr(k, tb[IFACE_ATTR_PREFIX], rem) {
			struct blob_attr *tb[PREFIX_ATTR_MAX], *a;
			blobmsg_parse(prefix_attrs, PREFIX_ATTR_MAX, tb,
					blobmsg_data(k), blobmsg_data_len(k));

			hnetd_time_t preferred = HNETD_TIME_MAX;
			hnetd_time_t valid = HNETD_TIME_MAX;
			struct prefix p = {IN6ADDR_ANY_INIT, 0};
			struct prefix ex = {IN6ADDR_ANY_INIT, 0};

			if (!(a = tb[PREFIX_ATTR_ADDRESS]) ||
					inet_pton(AF_INET6, blobmsg_get_string(a), &p.prefix) < 1)
				continue;

			if (!(a = tb[PREFIX_ATTR_MASK]))
				continue;

			p.plen = blobmsg_get_u32(a);

			if ((a = tb[PREFIX_ATTR_PREFERRED]))
				preferred = now + (blobmsg_get_u32(a) * HNETD_TIME_PER_SECOND);

			if ((a = tb[PREFIX_ATTR_VALID]))
				valid = now + (blobmsg_get_u32(a) * HNETD_TIME_PER_SECOND);

			if ((a = tb[PREFIX_ATTR_EXCLUDED]))
				prefix_pton(blobmsg_get_string(a), &ex);

			void *data = NULL;
			size_t len = 0;

	#ifdef EXT_PREFIX_CLASS
			struct dhcpv6_prefix_class pclass = {
				.type = htons(DHCPV6_OPT_PREFIX_CLASS),
				.len = htons(2),
				.class = htons(atoi(blobmsg_get_string(a)))
			};

			if ((a = tb[PREFIX_ATTR_CLASS])) {
				data = &pclass;
				len = sizeof(pclass);
			}
	#endif

			iface_add_delegated(c, &p, &ex, valid, preferred, data, len);
		}
	}

	const size_t dns_max = 4;
	size_t dns_cnt = 0, dns4_cnt = 0;
	struct {
		uint16_t type;
		uint16_t len;
		struct in6_addr addr[dns_max];
	} dns;
	struct __attribute__((packed)) {
		uint8_t type;
		uint8_t len;
		struct in_addr addr[dns_max];
	} dns4;

	blobmsg_for_each_attr(k, tb[IFACE_ATTR_DNS], rem) {
		if (dns_cnt >= dns_max || blobmsg_type(k) != BLOBMSG_TYPE_STRING ||
				inet_pton(AF_INET6, blobmsg_data(k), &dns.addr[dns_cnt]) < 1) {
			if (dns4_cnt >= dns_max ||
					inet_pton(AF_INET, blobmsg_data(k), &dns4.addr[dns4_cnt]) < 1)
				continue;
			++dns4_cnt;
			continue;
		}

		++dns_cnt;
	}

	if (v6uplink && dns_cnt) {
		dns.type = htons(DHCPV6_OPT_DNS_SERVERS);
		dns.len = htons(dns_cnt * sizeof(struct in6_addr));
		iface_add_dhcpv6_received(c, &dns, ((uint8_t*)&dns.addr[dns_cnt]) - ((uint8_t*)&dns));
	}

	if (v4uplink) {
		if (dns4_cnt) {
			dns4.type = DHCPV4_OPT_DNSSERVER;
			dns4.len = dns4_cnt * sizeof(struct in_addr);
			iface_add_dhcp_received(c, &dns4, ((uint8_t*)&dns4.addr[dns4_cnt]) - ((uint8_t*)&dns4));
		}

		iface_set_ipv4_uplink(c);
	}
}


// Decode and analyze netifd interface status blob
static void platform_update(void *data, size_t len)
{
	struct blob_attr *tb[IFACE_ATTR_MAX], *a;
	blobmsg_parse(iface_attrs, IFACE_ATTR_MAX, tb, data, len);

	if (!(a = tb[IFACE_ATTR_IFNAME]))
		return;

	const char *ifname = blobmsg_get_string(a);
	struct iface *c = iface_get(ifname);
	bool up = (a = tb[IFACE_ATTR_UP]) && blobmsg_get_bool(a);
	bool v4uplink = false, v6uplink = false;

	struct blob_attr *route;
	unsigned rem;
	blobmsg_for_each_attr(route, tb[IFACE_ATTR_ROUTE], rem) {
		struct blob_attr *rtb[ROUTE_ATTR_MAX];
		blobmsg_parse(route_attrs, ROUTE_ATTR_MAX, rtb, blobmsg_data(route), blobmsg_len(route));
		if (!rtb[ROUTE_ATTR_MASK] || blobmsg_get_u32(rtb[ROUTE_ATTR_MASK]))
			continue;

		const char *target = (rtb[ROUTE_ATTR_TARGET]) ? blobmsg_get_string(rtb[ROUTE_ATTR_TARGET]) : NULL;
		if (target && !strcmp(target, "::"))
			v6uplink = true;
		else if (target && !strcmp(target, "0.0.0.0"))
			v4uplink = true;
	}

	enum iface_flags flags = 0;
	struct in6_addr cer = IN6ADDR_ANY_INIT;
	if (tb[IFACE_ATTR_DATA]) {
		struct blob_attr *dtb[DATA_ATTR_MAX];
		blobmsg_parse(data_attrs, DATA_ATTR_MAX, dtb,
				blobmsg_data(tb[IFACE_ATTR_DATA]), blobmsg_len(tb[IFACE_ATTR_DATA]));

		if (dtb[DATA_ATTR_ACCEPT_CERID] && blobmsg_get_bool(dtb[DATA_ATTR_ACCEPT_CERID]))
			flags |= IFACE_FLAG_ACCEPT_CERID;

		if (dtb[DATA_ATTR_GUEST] && blobmsg_get_bool(dtb[DATA_ATTR_GUEST]))
			flags |= IFACE_FLAG_GUEST;

		if (dtb[DATA_ATTR_CER])
			inet_pton(AF_INET6, blobmsg_get_string(dtb[DATA_ATTR_CER]), &cer);
	}

	const char *proto = "";
	if ((a = tb[IFACE_ATTR_PROTO]))
		proto = blobmsg_get_string(a);

	if (!c && up && !strcmp(proto, "hnet") && (a = tb[IFACE_ATTR_HANDLE]))
		c = iface_create(ifname, blobmsg_get_string(a), flags);

	if (c)
		c->cer = cer;

	L_INFO("platform: interface update for %s detected", ifname);

	if (c && c->platform) {
		// This is a known managed interface
		if (!strcmp(proto, "hnet")) {
			if (!up)
				iface_remove(c);
		} else {
			update_interface(c, tb, v4uplink, v6uplink);
		}
	} else {
		// We have only unmanaged interfaces at this point
		// If netifd delegates this prefix, ignore it
		if ((a = tb[IFACE_ATTR_DELEGATION]) && blobmsg_get_bool(a)) {
			v4uplink = false;
			v6uplink = false;
		}

		bool empty = !up || (!v6uplink && !v4uplink);

		// If we don't know this interface yet but it has a PD for us create it
		if (!c && !empty)
			c = iface_create(ifname, NULL, 0);

		if (c && up)
			update_interface(c, tb, v4uplink, v6uplink);

		// Likewise, if all prefixes are gone, delete the interface
		if (c && empty)
			iface_remove(c);
	}
}


// Handle netifd ubus event for interfaces updates
static int handle_update(__unused struct ubus_context *ctx, __unused struct ubus_object *obj,
		__unused struct ubus_request_data *req, __unused const char *method,
		struct blob_attr *msg)
{
	struct blob_attr *tb[IFACE_ATTR_MAX];
	blobmsg_parse(iface_attrs, IFACE_ATTR_MAX, tb, blob_data(msg), blob_len(msg));

	if (!tb[IFACE_ATTR_PROTO] || strcmp(blobmsg_get_string(tb[IFACE_ATTR_PROTO]), "hnet") ||
			!tb[IFACE_ATTR_IFNAME] || !iface_get(blobmsg_get_string(tb[IFACE_ATTR_IFNAME])))
		sync_netifd(false);

	return 0;
}


enum {
	DUMP_ATTR_INTERFACE,
	DUMP_ATTR_MAX
};

static const struct blobmsg_policy dump_attrs[DUMP_ATTR_MAX] = {
	[DUMP_ATTR_INTERFACE] = { .name = "interface", .type = BLOBMSG_TYPE_ARRAY },
};

// Handle netifd ubus reply for interface dump
static void handle_dump(__unused struct ubus_request *req,
		__unused int type, struct blob_attr *msg)
{
	struct blob_attr *tb[DUMP_ATTR_MAX];
	blobmsg_parse(dump_attrs, DUMP_ATTR_MAX, tb, blob_data(msg), blob_len(msg));

	if (!tb[DUMP_ATTR_INTERFACE])
		return;

	struct blob_attr *c;
	unsigned rem;

	iface_update();

	blobmsg_for_each_attr(c, tb[DUMP_ATTR_INTERFACE], rem)
		platform_update(blobmsg_data(c), blobmsg_data_len(c));

	iface_commit();
}

