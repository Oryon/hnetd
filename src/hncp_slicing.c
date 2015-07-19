/**
 * IETF 93 Hackathon
 */

#include "hncp_slicing.h"


static int do_add_rules(dncp d, uint32_t ep_id, struct tlv_attr *tlv);
struct slice_content;
typedef struct slice_content slice_content_s, *slice_content_p;

struct slice_content {
	struct prefix p;
	dncp_node node;
	uint32_t ep_id;
	slice_content_p next;
};

typedef struct slice_subscriber {
	dncp d;
	dncp_subscriber_s dncp_subscriber;
} slice_subscriber_s, *slice_subscriber_p;

static slice_subscriber_s subscriber = {0};

int hncp_slicing_init(dncp dncp, const char __unused *scriptpath)
{
	L_ERR("Init called");
	subscriber.dncp_subscriber.tlv_change_cb = &slicing_tlv_changed_callback;
	subscriber.d = dncp;
	dncp_subscribe(dncp, &subscriber.dncp_subscriber);
	return 0;
}

/*
 * Used to change endpoint slice assignment
 *
 * Is slice is 0, removes the assignment if any, otherwise adds or changes the assignment
 */
int hncp_slicing_set_slice(dncp d, char *ifname, uint32_t slice)
{
	L_ERR("Set slice %d to if %s", slice, ifname);
	uint32_t endpoint_id = dncp_ep_get_id(dncp_find_ep_by_name(d, ifname));
	bool nochange = false;
	//Find our TLV if it exists, and always remove it first
	struct tlv_attr *a;
	dncp_node_for_each_tlv_with_type(dncp_get_own_node(d), a, HNCP_T_SLICE_MEMBERSHIP) {
		hncp_slice_membership_data_p tlv = tlv_data(a);
		if (tlv->endpoint_id == endpoint_id) {
			L_ERR("Existing TLV found for this interface, slice %d", ntohl(tlv->slice_id));
			if (ntohl(tlv->slice_id) != slice) {
				dncp_tlv dt = dncp_find_tlv(d, HNCP_T_SLICE_MEMBERSHIP, tlv_data(a), sizeof(hncp_slice_membership_data_s));
				dncp_remove_tlv(d, dt);
			} else {
				nochange = true;
			}
		}
	}
	if (slice != 0 && !nochange) {
		L_ERR("Adding new TLV");
		hncp_slice_membership_data_s tlv;
		tlv.endpoint_id = endpoint_id;
		tlv.slice_id = htonl(slice);
		dncp_add_tlv(d, HNCP_T_SLICE_MEMBERSHIP, &tlv, sizeof(hncp_slice_membership_data_s), 0);
	}
	return 0;
}

/*
 * Monitors changes to SLICE_MEMBERSHIP TLVs and ASSIGNED_PREFIX TLVs
 *
 * Calls do_add_rules when a change is needed (and only when a change is needed)
 * Note that this is called for both addition and deletion of a TLV, but in our
 * case the behavior is exactly the same in both situations?
 *
 * Issue: adding the rules is a heavy operation, we might want to wait till the state
 * is stable for each endpoint
 */
void slicing_tlv_changed_callback(dncp_subscriber __unused s, dncp_node n, struct tlv_attr *tlv, bool __unused add) {
	L_ERR("Callback called back");
	dncp d = subscriber.d;  //TODO something better, eg. save our slice_subscriber in hncp ext data
	if (tlv_id(tlv) == HNCP_T_SLICE_MEMBERSHIP) {
		L_ERR("=== Membership TLV changed");
		// We always have to update something if one of our TLVs changes
		hncp_slice_membership_data_p data = tlv_data(tlv);
		if (dncp_node_is_self(n)) {
			L_ERR("==== My TLV changed");
			do_add_rules(d, data->endpoint_id, tlv);
			return;
		}
		// We update the rules of all endpoints that are in the slice that changed
		struct tlv_attr *a;
		dncp_node_for_each_tlv_with_type(dncp_get_own_node(d), a, HNCP_T_SLICE_MEMBERSHIP) {
			hncp_slice_membership_data_p my_data = tlv_data(a);
			if (my_data->slice_id == data->slice_id) {
				L_ERR("==== Other TLV changed, with a slice of my interface");
				do_add_rules(d, my_data->endpoint_id, tlv);
			}
		}
	} else if (tlv_id(tlv) == HNCP_T_ASSIGNED_PREFIX) {
		L_ERR("=== Assigned TLV changed");
		hncp_t_assigned_prefix_header data = tlv_data(tlv);
		L_ERR("=== EP that changed %d", data->ep_id);
		// Find the slice affected by this change
		uint32_t slice = 0;
		struct tlv_attr *a;
		dncp_node_for_each_tlv_with_type(n, a, HNCP_T_SLICE_MEMBERSHIP) {
			hncp_slice_membership_data_p other_data = tlv_data(a);
			if (data->ep_id == other_data->endpoint_id) {
				L_ERR("==== Found other membership for ep %d, slice %d", other_data->endpoint_id, ntohl(other_data->slice_id));
				slice = ntohl(other_data->slice_id);
				break;
			}
		}
		//Now find the ep(s) in this slice and update them
		dncp_node_for_each_tlv_with_type(dncp_get_own_node(d), a, HNCP_T_SLICE_MEMBERSHIP) {
			hncp_slice_membership_data_p my_data = tlv_data(a);
			if (my_data->slice_id == slice) {
				L_ERR("==== Found my membership for slice %d ep %d",ntohl(my_data->slice_id), my_data->endpoint_id);
				do_add_rules(d, my_data->endpoint_id, a);
			}
		}
	}
}


static int do_add_rules(dncp dncp_inst, uint32_t ep_id, struct tlv_attr *tlv) {
//First we get the slice number corresponding to this ep_id
	uint32_t slice_id = 0;
	L_ERR("Begin do_add_rules with endpoint %d",ep_id);
	//Now find the name of the interface
	dncp_ep dep = NULL;
	dep = dncp_find_ep_by_id(dncp_inst, ep_id);
	if (dep == NULL){
		L_ERR("This ep corresponds to nothing. Die !");
		L_ERR("end of do_add_rules");
		return -1;
	}
	slice_id = ntohl(((hncp_slice_membership_data_p) tlv->data)->slice_id);
	//Did we found our slice number or is it zero?
	if (slice_id == 0) {
		L_ERR("Null slice, flush everything");
		flush_slicing_config(dep->ifname);
		L_ERR("end of do_add_rules");
		return 0;
	}
	//Now I have the slice number, let us find all the (node,ep) on this slice
	slice_content_p s_cont = NULL;
	dncp_node n = NULL;
	dncp_for_each_node(dncp_inst,n)
	{
		struct tlv_attr* membership_tlv = NULL;
		dncp_node_for_each_tlv_with_type(n,membership_tlv,HNCP_T_SLICE_MEMBERSHIP)
		{
			hncp_slice_membership_data_p data =
					(hncp_slice_membership_data_p) membership_tlv->data;
			uint32_t sid = ntohl(data->slice_id);
			uint32_t ep = data->endpoint_id;
			if (sid == slice_id) {
				//It is the good slice, add an entry in s_cont
				slice_content_p newEntry = calloc(1, sizeof(slice_content_s));

				//!!!UGLY !!!
				L_ERR("Node %d ep %d in slice %d",&n->node_id);

				newEntry->ep_id = ep;
				newEntry->node = n;
				slice_content_p* endOfList = &s_cont;
				while (*endOfList != NULL)
					endOfList = &((*endOfList)->next);
				*endOfList = newEntry;
				struct tlv_attr* pa_tlv = NULL;
				//Now find the right prefix
				dncp_node_for_each_tlv_with_type(n,pa_tlv,HNCP_T_ASSIGNED_PREFIX)
				{
					hncp_t_assigned_prefix_header pa_data =
							(hncp_t_assigned_prefix_header) pa_tlv->data;
					if (pa_data->ep_id == ep_id) {
						//Now assign the right prefix
						L_ERR("Found the prefix for that link : %x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x/%d",pa_data->prefix_data[0],
								pa_data->prefix_data[1],
								pa_data->prefix_data[2],
								pa_data->prefix_data[3],
								pa_data->prefix_data[4],
								pa_data->prefix_data[5],
								pa_data->prefix_data[6],
								pa_data->prefix_data[7],
								pa_data->prefix_data[8],
								pa_data->prefix_data[9],
								pa_data->prefix_data[10],
								pa_data->prefix_data[11],
								pa_data->prefix_data[12],
								pa_data->prefix_data[13],
								pa_data->prefix_data[14],
								pa_data->prefix_data[15],
								pa_data->prefix_length_bits);
						newEntry->p.plen = pa_data->prefix_length_bits;
						memcpy(&newEntry->p.prefix, pa_data->prefix_data, 16);
						//We consider that plen = 0 means no prefix assigned (ignore entry)
					}
				}
			}
		}
	}

//Build the array of accessible prefixes
	struct prefix* accessibles = NULL;
	int current_len = 0;
	while (s_cont != NULL) {
		if (s_cont->p.plen != 0) {
			accessibles = realloc(accessibles,
					(current_len + 1) * sizeof(struct prefix));
			memcpy(&accessibles[current_len], &s_cont->p,
					sizeof(struct prefix));
			current_len++;
		}
		slice_content_p next = s_cont->next;
		free(s_cont);
		s_cont = next;
	}
	struct prefix* dp_prefixes = NULL;
	int current_num_dp = 0;
	dncp_tlv dp_tlv = NULL;
	dncp_for_each_tlv(dncp_inst,dp_tlv)
	{
		struct tlv_attr* attrRoot = dncp_tlv_get_attr(dp_tlv);
		if (tlv_id(attrRoot) == HNCP_T_EXTERNAL_CONNECTION) {
			struct tlv_attr* attr = NULL;
			tlv_for_each_attr(attr,attrRoot)
				if (tlv_id(attr) == HNCP_T_DELEGATED_PREFIX) {
					dp_prefixes = realloc(dp_prefixes,
							(current_num_dp + 1) * sizeof(struct prefix));
					hncp_t_delegated_prefix_header dp_data =
							(hncp_t_delegated_prefix_header) attr->data;
					L_ERR("Found DP %x%x%x%x%x%x%x%x%x%x%x%x%x%x%x%x/%d",
							dp_data->prefix_data[0], dp_data->prefix_data[1],
							dp_data->prefix_data[2], dp_data->prefix_data[3],
							dp_data->prefix_data[4], dp_data->prefix_data[5],
							dp_data->prefix_data[6], dp_data->prefix_data[7],
							dp_data->prefix_data[8], dp_data->prefix_data[9],
							dp_data->prefix_data[10], dp_data->prefix_data[11],
							dp_data->prefix_data[12], dp_data->prefix_data[13],
							dp_data->prefix_data[14], dp_data->prefix_data[15],
							dp_data->prefix_length_bits);
					memcpy(&dp_prefixes[current_num_dp].prefix,
							dp_data->prefix_data, 16);
					dp_prefixes[current_num_dp].plen =
							dp_data->prefix_length_bits;
					current_num_dp++;
				}
		}
	}
	update_slicing_config(dep->ifname, true, current_num_dp, dp_prefixes,
			current_len, accessibles);
	L_ERR("end of do_add_rules");
	free(dp_prefixes);
	free(accessibles);
	return 0;
}
