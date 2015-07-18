/**
 * IETF 93 Hackathon
 */

#include "hncp_wifi.h"
#include "hncp_i.h"

#define HNCP_SSIDS 2; //Number of supported SSID provided to the script

struct hncp_t_wifi_ssid_struct {
	uint32_t slice;
	uint8_t ssid[32];
	uint8_t password[32];
} __packed;
typedef struct hncp_t_wifi_ssid_struct hncp_t_wifi_ssid_s, *hncp_t_wifi_ssid;

typedef struct hncp_ssid_struct {
	struct list_head le;
	int32_t id; // A pushed ID or -1
	uint32_t slice;
	uint8_t ssid[32];
	uint8_t password[32];
} hncp_ssid_s, *hncp_ssid;

struct hncp_wifi_struct {
	struct list_head ssids;
	const char *script;
	dncp dncp;
	dncp_subscriber_s subscriber;
};

static void wifi_ssid_notify(hncp_wifi wifi,
		dncp_node n, struct tlv_attr *tlv, bool add)
{

}

int hncp_wifi_modssid(hncp_wifi wifi, uint32_t slice,
		const char *ssid, const char *password, bool del)
{
	struct hncp_t_wifi_ssid_struct tlv = {
			.slice = htonl(slice)
	};
	strcpy(tlv.ssid, ssid);
	strcpy(tlv.password, password);
	dncp_tlv dtlv = dncp_find_tlv(wifi->dncp, HNCP_T_SSID, &tlv, sizeof(tlv));
	if(del) {
		if(dtlv) {
			dncp_remove_tlv(wifi->dncp, dtlv);
			return 0;
		}
		return 1;
	} else {
		if(!dtlv)
			return !!dncp_add_tlv(wifi->dncp, HNCP_T_SSID, &tlv, sizeof(tlv), 0);
		return 1;
	}
	return 0; //for warning
}

static void wifi_tlv_cb(dncp_subscriber s,
		dncp_node n, struct tlv_attr *tlv, bool add)
{
	hncp_wifi wifi = container_of(s, hncp_wifi_s, subscriber);
	if(tlv_id(tlv) == HNCP_T_SSID) //TODO: Add sanity check
		wifi_ssid_notify(wifi, n, tlv, add);
}

hncp_wifi hncp_wifi_init(hncp hncp, const char *scriptpath)
{
	hncp_wifi wifi;
	if(!(wifi = calloc(1, sizeof(*wifi))))
		return NULL;

	INIT_LIST_HEAD(&wifi->ssids);
	wifi->script = scriptpath;
	wifi->dncp = hncp->dncp;
	wifi->subscriber.tlv_change_cb = wifi_tlv_cb;
	dncp_subscribe(wifi->dncp, &wifi->subscriber);
	return wifi;
}

