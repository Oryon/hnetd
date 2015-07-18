/**
 * IETF 93 Hackathon
 */

#include "hncp_slicing.h"

int hncp_slicing_init(hncp hncp, const char *scriptpath)
{
	dncp_subscriber sub = {};
	return 0;
}

void tlv_changed_callback(dncp_subscriber s, dncp_node n, struct tlv_attr *tlv, bool add) {

}

//0 = remove slice
int set_endpoint_slice(uint32_t endpoint_id, uint32_t slice) {

}


int do_add_rules(hncp h, uint32_t ep_id) {

}
