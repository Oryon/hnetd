/**
 * IETF 93 Hackathon
 */

#ifndef HNCP_WIFI_H_
#define HNCP_WIFI_H_

#include "hnetd.h"
#include "hncp.h"

#define HNCP_T_SSID 195

typedef struct hncp_wifi_struct hncp_wifi_s, *hncp_wifi;

hncp_wifi hncp_wifi_init(hncp hncp, char *scriptpath);

int hncp_wifi_modssid(hncp_wifi wifi, uint32_t slice,
		const char *ssid, const char *password, bool del);

#endif /* HNCP_WIFI_H_ */
