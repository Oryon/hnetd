#ifndef _IFACE_H
#define _IFACE_H

#include "hnetd.h"
#include "prefix_utils.h"

#include <libubox/list.h>
#include <libubox/vlist.h>
#include <netinet/in.h>
#include <time.h>


// API for PA / HCP & friends

struct iface_user {
	// We will just add this struct to our linked-list so please keep it around by yourself ;)
	struct list_head head;

	/* Callback for internal interfaces */
	void (*cb_intiface)(struct iface_user *u, const char *ifname, bool enabled);

	/* Callback for external interfaces */
	void (*cb_extdata)(struct iface_user *u, const char *ifname,
			const void *dhcpv6_data, size_t dhcpv6_len);

	/* Callback for delegated prefixes (a negative validity time indicates removal) */
	void (*cb_prefix)(struct iface_user *u, const char *ifname,
			const struct prefix *prefix, const struct prefix *excluded,
			hnetd_time_t valid_until, hnetd_time_t preferred_until,
			const void *dhcpv6_data, size_t dhcpv6_len);
};

// Register user for interface events (callbacks with NULL-values are ignored)
void iface_register_user(struct iface_user *user);

// Unregister user for interface events, do NOT call this from the callback itself!
void iface_unregister_user(struct iface_user *user);

// Update DHCPv6 out data
void iface_set_dhcpv6_send(const char *ifname, const void *dhcpv6_data, size_t dhcpv6_len);


// Begin route update cycle
void iface_update_routes(void);

// Add new routes
void iface_add_route(const char *ifname, const struct prefix *from, const struct prefix *to, const struct in6_addr *via);

// Flush and commit routes to synthesize events
void iface_commit_routes(void);


// Internal API to platform

struct iface_addr {
	struct vlist_node node;
	hnetd_time_t valid_until;
	hnetd_time_t preferred_until;
	struct prefix excluded;
	struct prefix prefix;
	size_t dhcpv6_len;
	uint8_t dhcpv6_data[];
};

struct iface_route {
	struct vlist_node node;
	struct prefix from;
	struct prefix to;
	struct in6_addr via;
};

struct iface {
	struct list_head head;

	// Platform specific handle
	void *platform;

	// Interface status
	bool linkowner;
	bool internal;
	bool v4leased;

	// LL-address
	struct in6_addr eui64_addr;

	// Prefix storage
	struct vlist_tree assigned;
	struct vlist_tree delegated;
	struct vlist_tree routes;

	// Other data
	void *dhcpv6_data_in;
	void *dhcpv6_data_out;
	size_t dhcpv6_len_in;
	size_t dhcpv6_len_out;

	// Interface name
	char ifname[];
};


#include "pa.h"

// Generic initializer to be called by main()
int iface_init(pa_t pa);

// Get an interface by name
struct iface* iface_get(const char *ifname);

// Create / get an interface (external or internal), handle set = managed
struct iface* iface_create(const char *ifname, const char *handle);

// Remove a known interface
void iface_remove(struct iface *iface);


// Begin PD update cycle
void iface_update_delegated(struct iface *c);

// Add currently available prefixes from PD
void iface_add_delegated(struct iface *c,
		const struct prefix *p, const struct prefix *excluded,
		hnetd_time_t valid_until, hnetd_time_t preferred_until,
		const void *dhcpv6_data, size_t dhcpv6_len);

// Flush and commit PD to synthesize events to users and rerun border discovery
void iface_commit_delegated(struct iface *c);


// Set DHCPv4 leased flag and rerun border discovery
void iface_set_v4leased(struct iface *c, bool v4leased);


// Set DHCPv6 data received
void iface_set_dhcpv6_received(struct iface *c, const void *dhcpv6_data, size_t dhcpv6_len);

// Flush all interfaces
void iface_flush(void);

#endif
