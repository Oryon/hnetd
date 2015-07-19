/**
 * IETF 93 Hackathon
 */

#ifndef HNCP_SLICING_H_
#define HNCP_SLICING_H_

#include "hncp.h"
#include "dncp.h"
#include "dncp_i.h"
#include "hncp_proto.h"
#include "prefix_utils.h"
#include "platform.h"
#define HNCP_T_SLICE_MEMBERSHIP 	200

extern bool slicing_enabled;

typedef struct __packed hncp_slice_membership_tlv {
	uint32_t endpoint_id;
	uint32_t slice_id;
} hncp_slice_membership_data_s, *hncp_slice_membership_data_p;

int hncp_slicing_init(dncp dncp, const char *scriptpath);

void slicing_tlv_changed_callback(dncp_subscriber s, dncp_node n, struct tlv_attr *tlv, bool add);

//0 = remove slice
int hncp_slicing_set_slice(dncp d, char *ifname, uint32_t slice);


#endif /* HNCP_SLICING_H_ */
