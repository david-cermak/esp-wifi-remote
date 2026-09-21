/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "wifi_remote_rpc_impl.hpp"

struct esp_wifi_remote_config {
    wifi_interface_t interface;
    wifi_config_t conf;
};

/** Wire response for GET_MAC: err is little-endian; MAC is raw bytes. */
struct esp_wifi_remote_mac_t {
    eppp_rpc::le32 err;
    uint8_t mac[6];
};

struct esp_wifi_remote_eppp_ip_event {
    eppp_rpc::le32 id;
    esp_netif_ip_info_t wifi_ip;
    esp_netif_ip_info_t ppp_ip;
    esp_netif_dns_info_t dns;
};
