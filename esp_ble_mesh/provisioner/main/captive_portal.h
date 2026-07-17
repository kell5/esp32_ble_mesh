#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts an HTTP server pre-loaded with captive-portal keepalive handlers and a
 * catch-all DNS server. The handlers answer OS connectivity probes so phones
 * treat the SoftAP as "internet available" and do not auto-disconnect (nor
 * force a captive-portal sign-in that could block the provisioning app). The
 * returned handle is meant to be passed to
 * network_prov_scheme_softap_set_httpd_handle() so protocomm shares this server.
 */
esp_err_t farmely_captive_portal_start(httpd_handle_t *out_server);

/* Stops the DNS + HTTP servers started by farmely_captive_portal_start(). */
void farmely_captive_portal_stop(httpd_handle_t server);

/* Makes the SoftAP DHCP server advertise the SoftAP IP (192.168.4.1) as the
 * DNS server so clients send their connectivity-check lookups to us. Call this
 * from the WIFI_EVENT_AP_START handler. */
void farmely_captive_portal_configure_dhcp_dns(void);

#ifdef __cplusplus
}
#endif
