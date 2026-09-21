/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cstring>

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

#ifdef WIFI_RMT_HAS_REFLECTION
namespace eppp_rpc {

/**
 * wifi_config_t is a union — convert `interface`, then only the active arm.
 */
template<>
inline void to_wire_bytes<esp_wifi_remote_config>(const esp_wifi_remote_config &host, void *out)
{
    std::memcpy(out, &host, sizeof(host));
    auto *base = static_cast<std::uint8_t *>(out);
    to_wire_bytes(host.interface, base + offsetof(esp_wifi_remote_config, interface));
    void *conf = base + offsetof(esp_wifi_remote_config, conf);
    if (host.interface == WIFI_IF_AP) {
        rpc_fix_endian_members<wifi_ap_config_t>(conf, true);
    } else if (host.interface == WIFI_IF_STA) {
        rpc_fix_endian_members<wifi_sta_config_t>(conf, true);
    }
}

template<>
inline esp_wifi_remote_config from_wire_bytes<esp_wifi_remote_config>(const void *in)
{
    esp_wifi_remote_config host{};
    std::memcpy(&host, in, sizeof(host));
    auto *base = reinterpret_cast<std::uint8_t *>(&host);
    host.interface = from_wire_bytes<wifi_interface_t>(
                         base + offsetof(esp_wifi_remote_config, interface));
    void *conf = base + offsetof(esp_wifi_remote_config, conf);
    if (host.interface == WIFI_IF_AP) {
        rpc_fix_endian_members<wifi_ap_config_t>(conf, false);
    } else if (host.interface == WIFI_IF_STA) {
        rpc_fix_endian_members<wifi_sta_config_t>(conf, false);
    }
    return host;
}

}  // namespace eppp_rpc
#endif
