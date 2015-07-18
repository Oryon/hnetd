/**
 * IETF 93 Hackathon
 */

#ifndef HNCP_SLICING_H_
#define HNCP_SLICING_H_

#include "hncp.h"
#include "dncp.h"
#include "dncp_i.h"
#include "prefix_utils.h"
#include "hncp_proto.h"
#include <uci.h>
#define HNCP_T_SLICE_MEMBERSHIP 	200

typedef struct __packed hncp_slice_membership_tlv {
	uint32_t endpoint_id;
	uint32_t slice_id;
} hncp_slice_membership_data_s, *hncp_slice_membership_data_p;

int hncp_slicing_init(hncp hncp, const char *scriptpath);

void slicing_tlv_changed_callback(dncp_subscriber s, dncp_node n, struct tlv_attr *tlv, bool add);

//0 = remove slice
int set_endpoint_slice(hncp h, uint32_t endpoint_id, uint32_t slice);

int do_add_rules(hncp h, uint32_t ep_id);

void update_uci(char* iface, bool internet, int nb_inet_prefixes,struct prefix* inet_prefixes, int nb_accessible_prefixes,struct prefix* accessible_prefixes);

#endif /* HNCP_SLICING_H_ */
