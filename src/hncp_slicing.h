/**
 * IETF 93 Hackathon
 */

#ifndef HNCP_SLICING_H_
#define HNCP_SLICING_H_

#include "hncp.h"

struct hncp_slice_membership_data;
typedef struct hncp_slice_membership_data hncp_slice_membership_data_s;
typedef struct hncp_slice_membership_data *hncp_slice_membership_data_p;

#define HNCP_T_SLICE_MEMBERSHIP 	200

struct __packed hncp_slice_membership_tlv {
	uint32_t endpoint_id;
	uint32_t slice_id;
};

int hncp_slicing_init(hncp hncp, const char *scriptpath);

void tlv_changed_callback(dncp_subscriber s, dncp_node n, struct tlv_attr *tlv, bool add);

//0 = remove slice
int set_endpoint_slice(uint32_t endpoint_id, uint32_t slice);


bool is_endpoint_local_if(uint32_t ep);



#endif /* HNCP_SLICING_H_ */
