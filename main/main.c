/*
 * main.c  –  ESP32-S3-ETH  •  Philips Hue Bridge Proxy
 *
 * Architecture
 * ─────────────────────────────────────────────────────────────────────────────
 *  nRF54LM20 EVK ──[Wi-Fi AP]── ESP32-S3-ETH ──[Ethernet/W5500]── Hue Bridge
 *
 *  The ESP32 exposes a Wi-Fi AP (192.168.4.1:80).  The nRF connects to it and
 *  sends standard Hue REST requests.  Every request is forwarded over Ethernet
 *  to the bridge and the response relayed back verbatim.
 *
 *  Hardware: Waveshare ESP32-S3-POE-ETH  (ESP32-S3R8, ESP-IDF ≥ 5.3)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_eth.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "dhcpserver/dhcpserver.h"
#include "ethernet_init.h"
#include "config.h"

static const char *TAG = "HUE_PROXY";

/* Maximum Hue API body / response size we handle (bytes).
 * /api/0/config returns ~10 KB of JSON so keep the response buffer large. */
#define MAX_HUE_BODY      512
#define MAX_HUE_RESPONSE  16384

/* Ethernet link state – updated by the event handler */
static volatile bool s_eth_link_up = false;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Network event handlers
 * ═══════════════════════════════════════════════════════════════════════════*/
static void eth_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "[ETH] Driver started");
        break;
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[ETH] Link up");
        s_eth_link_up = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "[ETH] Link down");
        s_eth_link_up = false;
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "[ETH] IP: " IPSTR "  GW: " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
    } else if (id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        ip_event_assigned_ip_to_client_t *ev = (ip_event_assigned_ip_to_client_t *)data;
        ESP_LOGI(TAG, "[WiFi] nRF client assigned IP: " IPSTR, IP2STR(&ev->ip));
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  W5500 Ethernet initialisation
 * ═══════════════════════════════════════════════════════════════════════════*/
static void eth_init(void)
{
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles = NULL;

    /* Initialize W5500 driver via ethernet_init component.
     * GPIO pins and SPI config come from sdkconfig (Kconfig). */
    ESP_ERROR_CHECK(ethernet_init_all(&eth_handles, &eth_port_cnt));

    /* Use a custom netif config: static IP + DHCP server (like a Wi-Fi AP but
     * on Ethernet). This lets the Hue Bridge get an IP automatically with a
     * direct cable – no router needed. */
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER |
                                         ESP_NETIF_FLAG_AUTOUP);
    base_cfg.ip_info = &((esp_netif_ip_info_t){
        .ip      = { .addr = ESP_IP4TOADDR(192,168,2,1) },
        .gw      = { .addr = ESP_IP4TOADDR(192,168,2,1) },
        .netmask = { .addr = ESP_IP4TOADDR(255,255,255,0) },
    });
    base_cfg.route_prio = 10;

    esp_netif_config_t netif_cfg = {
        .base  = &base_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif,
                                     esp_eth_new_netif_glue(eth_handles[0])));

    /* Configure DHCP server: hand out .2 to the Hue Bridge */
    esp_netif_dhcps_stop(eth_netif);
    dhcps_lease_t lease = {
        .enable      = true,
        .start_ip    = { .addr = ESP_IP4TOADDR(192,168,2,2) },
        .end_ip      = { .addr = ESP_IP4TOADDR(192,168,2,10) },
    };
    ESP_ERROR_CHECK(esp_netif_dhcps_option(eth_netif,
                                            ESP_NETIF_OP_SET,
                                            ESP_NETIF_REQUESTED_IP_ADDRESS,
                                            &lease, sizeof(lease)));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(eth_netif));

    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));
    ESP_LOGI(TAG, "[ETH] W5500 started – DHCP server on 192.168.2.1, "
                  "bridge will get 192.168.2.2");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Wi-Fi Access Point initialisation
 * ═══════════════════════════════════════════════════════════════════════════*/
static void wifi_ap_init(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.ap.ssid,     AP_SSID,     sizeof(wifi_cfg.ap.ssid)     - 1);
    strncpy((char *)wifi_cfg.ap.password, AP_PASSWORD, sizeof(wifi_cfg.ap.password) - 1);
    wifi_cfg.ap.ssid_len       = (uint8_t)strlen(AP_SSID);
    wifi_cfg.ap.max_connection = 4;
    wifi_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "[WiFi] AP started  SSID='%s'  IP=192.168.4.1", AP_SSID);
    ESP_LOGI(TAG, "[WiFi] nRF should connect to '%s' and target http://192.168.4.1", AP_SSID);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP proxy implementation
 * ═══════════════════════════════════════════════════════════════════════════*/

/* Accumulates response data from esp_http_client via its event callback. */
typedef struct {
    char *buf;
    int   len;
} resp_accum_t;

static esp_err_t http_client_event_cb(esp_http_client_event_t *evt)
{
    resp_accum_t *r = (resp_accum_t *)evt->user_data;
    if (r && evt->event_id == HTTP_EVENT_ON_DATA) {
        int space = MAX_HUE_RESPONSE - r->len;
        if (space > 0) {
            int n = (evt->data_len < space) ? evt->data_len : space;
            memcpy(r->buf + r->len, evt->data, n);
            r->len += n;
        }
    }
    return ESP_OK;
}

/* Translate httpd method id to esp_http_client method id */
static esp_http_client_method_t map_method(int m)
{
    switch (m) {
    case HTTP_PUT:    return HTTP_METHOD_PUT;
    case HTTP_POST:   return HTTP_METHOD_POST;
    case HTTP_DELETE: return HTTP_METHOD_DELETE;
    default:          return HTTP_METHOD_GET;
    }
}

/* Return a full HTTP status line for the most common Hue API codes */
static const char *http_status_str(int code)
{
    switch (code) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 204: return "204 No Content";
    case 207: return "207 Multi-Status";   /* Hue group/scene responses */
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 500: return "500 Internal Server Error";
    default:  return "200 OK";
    }
}

/* Catch-all proxy handler – registered for GET / PUT / POST / DELETE */
static esp_err_t proxy_handler(httpd_req_t *req)
{
    /* ── Guard: Ethernet must have link ──────────────────────────────────── */
    if (!s_eth_link_up) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req,
            "Ethernet link is down – plug the RJ45 cable into the Hue Bridge",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* ── Build target URL ────────────────────────────────────────────────── */
    char url[600];  /* 7 (http://) + 15 (IP) + 512 (max URI) + slack */
    snprintf(url, sizeof(url), "http://%s%s", HUE_BRIDGE_IP, req->uri);

    /* ── Read request body (reject oversized payloads) ───────────────────── */
    if (req->content_len > MAX_HUE_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Request body too large (max 512 bytes)");
        return ESP_OK;
    }

    char *body     = NULL;
    int   body_len = 0;
    if (req->content_len > 0) {
        body = malloc((size_t)req->content_len + 1);
        if (!body) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Out of memory");
            return ESP_OK;
        }
        body_len = httpd_req_recv(req, body, req->content_len);
        if (body_len < 0) body_len = 0;
        body[body_len] = '\0';
    }

    /* ── Allocate response accumulation buffer ────────────────────────────── */
    char *resp_buf = calloc(1, MAX_HUE_RESPONSE + 1);
    if (!resp_buf) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_OK;
    }
    resp_accum_t accum = { .buf = resp_buf, .len = 0 };

    /* ── Configure and run the outgoing HTTP client ──────────────────────── */
    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = map_method(req->method),
        .timeout_ms    = 5000,
        .event_handler = http_client_event_cb,
        .user_data     = &accum,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    /* Force connection close so server signals end-of-response by TCP close
     * rather than chunked terminator – avoids ESP_ERR_HTTP_INCOMPLETE_DATA */
    esp_http_client_set_header(client, "Connection", "close");

    /* Forward Hue authentication headers */
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "hue-application-key",
                                    hdr, sizeof(hdr)) == ESP_OK) {
        esp_http_client_set_header(client, "hue-application-key", hdr);
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization",
                                    hdr, sizeof(hdr)) == ESP_OK) {
        esp_http_client_set_header(client, "Authorization", hdr);
    }

    /* Attach body for PUT / POST */
    if (body_len > 0) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, body_len);
    }

    /* Perform the request (blocking, event handler accumulates response) */
    esp_err_t err       = esp_http_client_perform(client);
    int       hue_code  = esp_http_client_get_status_code(client);

    /* ── Relay response back to the nRF client ────────────────────────────── */
    /* Accept incomplete chunked data as success if we got a valid status code
     * and some data – the Hue Bridge sometimes closes before the final chunk */
    bool ok = (hue_code > 0) &&
              (err == ESP_OK || err == ESP_ERR_HTTP_INCOMPLETE_DATA);

    if (ok) {
        httpd_resp_set_status(req, http_status_str(hue_code));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, accum.buf, (ssize_t)accum.len);

        ESP_LOGI(TAG, "[PROXY] %-6s %s  →  HTTP %d  (%d B)",
                 (req->method == HTTP_GET  ? "GET"    :
                  req->method == HTTP_PUT  ? "PUT"    :
                  req->method == HTTP_POST ? "POST"   : "DELETE"),
                 req->uri, hue_code, accum.len);
    } else {
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Cannot reach Hue Bridge", HTTPD_RESP_USE_STRLEN);
        ESP_LOGW(TAG, "[PROXY] Failed: %s  err=%s", url, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(resp_buf);
    free(body);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP server initialisation
 * ═══════════════════════════════════════════════════════════════════════════*/
static void http_server_init(void)
{
    httpd_config_t cfg     = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn       = httpd_uri_match_wildcard;
    cfg.max_uri_handlers   = 8;
    cfg.stack_size         = 8192;
    cfg.server_port        = HUE_PROXY_PORT;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    /* Register one wildcard handler per HTTP method the Hue API uses */
    static const httpd_method_t methods[] = {
        HTTP_GET, HTTP_PUT, HTTP_POST, HTTP_DELETE
    };
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        httpd_uri_t uri = {
            .uri     = "/*",
            .method  = methods[i],
            .handler = proxy_handler,
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    }

    ESP_LOGI(TAG, "[HTTP] Proxy listening on port %d", HUE_PROXY_PORT);
    ESP_LOGI(TAG, "[HUE]  Bridge target: http://%s", HUE_BRIDGE_IP);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  app_main
 * ═══════════════════════════════════════════════════════════════════════════*/
void app_main(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Waveshare ESP32-S3-ETH  •  Hue Proxy   ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════╝");

    /* NVS init (required by Wi-Fi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Core infrastructure */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /* Network event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               ip_event_handler, NULL));

    /* Bring up hardware */
    eth_init();
    wifi_ap_init();
    http_server_init();

    ESP_LOGI(TAG, "Ready:");
    ESP_LOGI(TAG, "  nRF  → PUT http://192.168.4.1/api/<key>/lights/<id>/state");
    ESP_LOGI(TAG, "  fwd  → PUT http://%s/api/<key>/lights/<id>/state", HUE_BRIDGE_IP);
}
