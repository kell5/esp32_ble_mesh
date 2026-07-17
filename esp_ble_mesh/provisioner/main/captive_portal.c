#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/inet.h"

#include "dns_server.h"
#include "captive_portal.h"

#define TAG "FARMELY_CAPTIVE"
#define AP_IP "192.168.4.1"

/* DHCP option flag from lwip apps/dhcpserver/dhcpserver.h (OFFER_DNS = 0x02). */
#define FARMELY_DHCPS_OFFER_DNS 0x02

static dns_server_handle_t s_dns;

/* Android/Honor/Huawei connectivity checks expect HTTP 204. Returning it marks
 * the network "validated / has internet", so the OS keeps the SoftAP connected
 * and does not pop a captive-portal sign-in (which can block the app's traffic
 * to 192.168.4.1). */
static esp_err_t probe_204_handler(httpd_req_t *req)
{
    /* Probes are one-shot: close the connection so OS connectivity checks
     * do not pile up keep-alive sockets and starve protocomm requests. */
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Apple hotspot detection considers the network online only for this body. */
static esp_err_t apple_success_handler(httpd_req_t *req)
{
    static const char body[] =
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Windows NCSI probe. */
static esp_err_t ncsi_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Microsoft NCSI", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Any other path also validates (some vendors use non-standard probe URLs). */
static esp_err_t catchall_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    ESP_LOGI(TAG, "catch-all 204 for %s %s",
             req->method == HTTP_GET ? "GET" : "non-GET", req->uri);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t s_uris[] = {
    { .uri = "/generate_204", .method = HTTP_GET, .handler = probe_204_handler },
    { .uri = "/gen_204", .method = HTTP_GET, .handler = probe_204_handler },
    { .uri = "/generate204", .method = HTTP_GET, .handler = probe_204_handler },
    { .uri = "/", .method = HTTP_GET, .handler = probe_204_handler },
    { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = apple_success_handler },
    { .uri = "/library/test/success.html", .method = HTTP_GET, .handler = apple_success_handler },
    { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = ncsi_handler },
    { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = ncsi_handler },
};

esp_err_t farmely_captive_portal_start(httpd_handle_t *out_server)
{
    if (!out_server) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 24;
    config.max_open_sockets = 12;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0; i < sizeof(s_uris) / sizeof(s_uris[0]); i++) {
        httpd_register_uri_handler(server, &s_uris[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, catchall_404_handler);

    /* Resolve every A query to the SoftAP IP so probe hostnames reach us. */
    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns_cfg);
    if (!s_dns) {
        ESP_LOGW(TAG, "DNS server failed to start (probes may miss)");
    }

    *out_server = server;
    ESP_LOGI(TAG, "captive-portal keepalive started (HTTP 204 + DNS catch-all)");
    return ESP_OK;
}

void farmely_captive_portal_stop(httpd_handle_t server)
{
    if (s_dns) {
        stop_dns_server(s_dns);
        s_dns = NULL;
    }
    if (server) {
        httpd_stop(server);
    }
}

void farmely_captive_portal_configure_dhcp_dns(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) {
        ESP_LOGW(TAG, "AP netif not found; DHCP DNS not configured");
        return;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ipaddr_addr(AP_IP);

    uint8_t offer_dns = FARMELY_DHCPS_OFFER_DNS;
    esp_err_t e_stop = esp_netif_dhcps_stop(ap);
    esp_err_t e_dns = esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_err_t e_opt = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET,
                          ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns,
                          sizeof(offer_dns));
    esp_err_t e_start = esp_netif_dhcps_start(ap);
    ESP_LOGI(TAG, "SoftAP DHCP DNS=%s stop=%s dns=%s opt=%s start=%s", AP_IP,
             esp_err_to_name(e_stop), esp_err_to_name(e_dns),
             esp_err_to_name(e_opt), esp_err_to_name(e_start));
}
