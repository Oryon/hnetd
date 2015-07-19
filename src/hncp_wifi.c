/**
 * IETF 93 Hackathon
 */

#include <libubox/list.h>
#include <sys/wait.h>

#include "hncp_wifi.h"
#include "hncp_i.h"
#include "hnetd.h"

#define HNCP_SSIDS 2 //Number of supported SSID provided to the script

typedef struct hncp_ssid_struct {
	uint32_t id;
	uint32_t slice;
	char ssid[32];
	char password[32];
} hncp_ssid_s, *hncp_ssid;

struct hncp_wifi_struct {
	char *script;
	dncp dncp;
	dncp_subscriber_s subscriber;
	hncp_ssid_s ssids[HNCP_SSIDS];
};

static hncp_ssid wifi_find_ssid(hncp_wifi wifi, uint8_t *ssid,
		uint8_t *password, uint32_t slice)
{
	int i;
	for(i=0; i<HNCP_SSIDS; i++) {
		if(!strcmp((char *)wifi->ssids[i].ssid, (char *)ssid) &&
				!strcmp((char *)wifi->ssids[i].password, (char *)password) &&
				wifi->ssids[i].slice == slice)
			return &wifi->ssids[i];
	}
	return NULL;
}

static hncp_ssid wifi_find_free_ssid(hncp_wifi wifi)
{
	int i;
	for(i=0; i<HNCP_SSIDS; i++) {
		if(!strlen(wifi->ssids[i].ssid)) {
			wifi->ssids[i].id = i;
			return &wifi->ssids[i];
		}
	}
	return NULL;
}

static void wifi_ssid_notify(hncp_wifi wifi,
		__unused dncp_node n, struct tlv_attr *tlv, bool add)
{
	hncp_t_wifi_ssid tlv_data = (hncp_t_wifi_ssid) tlv->data;
	hncp_ssid ssid = wifi_find_ssid(wifi, tlv_data->ssid, tlv_data->password, ntohl(tlv_data->slice));
	L_ERR("wifi_ssid_notify ");
	if(add && !ssid) {
		ssid = wifi_find_free_ssid(wifi);
		if(!ssid) {
			L_ERR("Cannot handle that many ssids...");
			return;
		}
		strcpy(ssid->password, (char *)tlv_data->password);
		strcpy(ssid->ssid,  (char *)tlv_data->ssid);
		ssid->slice = ntohl(tlv_data->slice);

		char id[10], sl[10];
		sprintf(id, "%d", (int)ssid->id);
		sprintf(sl, "%d", (int)ssid->slice);
		char *argv[] = {wifi->script, "addssid", id, ssid->ssid, ssid->password, sl, NULL};
		pid_t pid = hncp_run(argv);
		int status;
		waitpid(pid, &status, 0);
		L_INFO("Auto-Wifi script (pid %d) returned %d", pid, status);
	} else if(!add && ssid) {
		char id[10], sl[10];
		sprintf(id, "%d", (int)ssid->id);
		sprintf(sl, "%d", (int)ssid->slice);
		char *argv[] = {wifi->script, "delssid", id, ssid->ssid, ssid->password, sl, NULL};
		pid_t pid = hncp_run(argv);
		int status;
		waitpid(pid, &status, 0);
		L_INFO("Auto-Wifi script (pid %d) returned %d", pid, status);

		ssid->ssid[0] = 0;
		ssid->ssid[1] = 1;
	}
}

int hncp_wifi_modssid(hncp_wifi wifi, uint32_t slice,
		const char *ssid, const char *password, bool del)
{
	L_INFO("Auto-Wifi mod-ssid %s ssid %s password %s on slice %d",
			del?"del":"add", ssid, password, (int) slice);
	struct hncp_t_wifi_ssid_struct tlv = {
			.slice = htonl(slice)
	};
	strcpy((char *)tlv.ssid, ssid);
	strcpy((char *)tlv.password, password);
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

hncp_wifi hncp_wifi_init(hncp hncp, char *scriptpath)
{
	hncp_wifi wifi;
	if(!(wifi = calloc(1, sizeof(*wifi))))
		return NULL;

	L_INFO("Initialize Auto-Wifi component with script %s", scriptpath);
	wifi->script = scriptpath;
	wifi->dncp = hncp->dncp;
	wifi->subscriber.tlv_change_cb = wifi_tlv_cb;
	dncp_subscribe(wifi->dncp, &wifi->subscriber);
	return wifi;
}

