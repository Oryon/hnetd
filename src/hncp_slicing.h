/**
 * IETF 93 Hackathon
 */

#ifndef HNCP_SLICING_H_
#define HNCP_SLICING_H_

#include "hncp.h"

struct hncp_slice_membership_data;
typedef struct hncp_slice_membership_data hncp_slice_membership_data_s;
typedef struct hncp_slice_membership_data *hncp_slice_membership_data_p;

struct hncp_slice_membership_tlv {
	uint32_t endpoint_id;
	uint32_t slice_id;
};



int hncp_slicing_init(hncp hncp, const char *scriptpath);

#endif /* HNCP_SLICING_H_ */
