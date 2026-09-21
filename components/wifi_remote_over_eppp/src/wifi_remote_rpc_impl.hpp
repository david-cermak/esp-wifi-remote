/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <cstring>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <type_traits>
#ifdef CONFIG_WIFI_RMT_OVER_EPPP_UNSECURE
#include <unistd.h>
#include <sys/socket.h>
#endif

namespace eppp_rpc {

static constexpr int rpc_port = 3333;

/**
 * @brief Explicit little-endian wire integers (same idea as zigbee-remote).
 *
 * Native multi-byte integers are never placed on the wire as-is. Both ends are
 * ESP (LE) today, but the framing must not rely on that coincidence.
 */
struct le16 {
    uint8_t bytes[2];
};

struct le32 {
    uint8_t bytes[4];
};

constexpr le16 to_le16(uint16_t value)
{
    return {{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)}};
}

constexpr uint16_t from_le16(le16 value)
{
    return static_cast<uint16_t>(value.bytes[0]) |
           (static_cast<uint16_t>(value.bytes[1]) << 8);
}

constexpr le32 to_le32(uint32_t value)
{
    return {{static_cast<uint8_t>(value),
             static_cast<uint8_t>(value >> 8),
             static_cast<uint8_t>(value >> 16),
             static_cast<uint8_t>(value >> 24)}};
}

constexpr uint32_t from_le32(le32 value)
{
    return static_cast<uint32_t>(value.bytes[0]) |
           (static_cast<uint32_t>(value.bytes[1]) << 8) |
           (static_cast<uint32_t>(value.bytes[2]) << 16) |
           (static_cast<uint32_t>(value.bytes[3]) << 24);
}

constexpr le32 to_le32_s(int32_t value)
{
    return to_le32(static_cast<uint32_t>(value));
}

constexpr int32_t from_le32_s(le32 value)
{
    return static_cast<int32_t>(from_le32(value));
}

}  // namespace eppp_rpc

#ifdef WIFI_RMT_HAS_REFLECTION
#include "wifi_remote_rpc_meta.hpp"
#endif

namespace eppp_rpc {

/**
 * @brief Currently supported RPC commands/events
 */
enum class api_id : uint32_t {
    ERROR,
    UNDEF,
    INIT,
    DEINIT,
    SET_MODE,
    SET_CONFIG,
    START,
    STOP,
    CONNECT,
    DISCONNECT,
    GET_MAC,
    SET_STORAGE,
    WIFI_EVENT,
    IP_EVENT,
    SET_PS,
};

enum class role {
    SERVER,
    CLIENT,
};

/** Host-side header (native integers). */
struct RpcHeader {
    api_id id;
    uint32_t size;
};

/** On-wire header: little-endian id and size. */
struct WireHeader {
    le32 id;
    le32 size;
} __attribute__((packed));

static_assert(sizeof(WireHeader) == 8);

/**
 * Encode a host value into wire bytes.
 * Integrals/enums → explicit LE.
 * Aggregates (with reflection) → member walk fixing i16/i32/enums; unions opaque.
 */
template<typename T>
void to_wire_bytes(const T &host, void *out)
{
    if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        to_wire_bytes(static_cast<U>(host), out);
    } else if constexpr (std::is_same_v<T, le16> || std::is_same_v<T, le32>) {
        std::memcpy(out, &host, sizeof(T));
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (sizeof(T) == 1) {
            std::memcpy(out, &host, 1);
        } else if constexpr (sizeof(T) == 2) {
            le16 w = to_le16(static_cast<uint16_t>(host));
            std::memcpy(out, &w, sizeof(w));
        } else if constexpr (sizeof(T) == 4) {
            le32 w = to_le32(static_cast<uint32_t>(host));
            std::memcpy(out, &w, sizeof(w));
        } else {
            static_assert(sizeof(T) != sizeof(T), "unsupported integral wire size");
        }
#ifdef WIFI_RMT_HAS_REFLECTION
    } else if constexpr (std::meta::is_union_type(^^T)) {
        std::memcpy(out, &host, sizeof(T));
    } else if constexpr (std::meta::is_class_type(^^T)) {
        rpc_struct_to_wire(host, out);
#endif
    } else {
        std::memcpy(out, &host, sizeof(T));
    }
}

template<typename T>
T from_wire_bytes(const void *in)
{
    if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        return static_cast<T>(from_wire_bytes<U>(in));
    } else if constexpr (std::is_same_v<T, le16> || std::is_same_v<T, le32>) {
        T out{};
        std::memcpy(&out, in, sizeof(T));
        return out;
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (sizeof(T) == 1) {
            T out{};
            std::memcpy(&out, in, 1);
            return out;
        } else if constexpr (sizeof(T) == 2) {
            le16 w{};
            std::memcpy(&w, in, sizeof(w));
            return static_cast<T>(from_le16(w));
        } else if constexpr (sizeof(T) == 4) {
            le32 w{};
            std::memcpy(&w, in, sizeof(w));
            return static_cast<T>(from_le32(w));
        } else {
            static_assert(sizeof(T) != sizeof(T), "unsupported integral wire size");
        }
#ifdef WIFI_RMT_HAS_REFLECTION
    } else if constexpr (std::meta::is_union_type(^^T)) {
        T out{};
        std::memcpy(&out, in, sizeof(T));
        return out;
    } else if constexpr (std::meta::is_class_type(^^T)) {
        return rpc_struct_from_wire<T>(in);
#endif
    } else {
        T out{};
        std::memcpy(&out, in, sizeof(T));
        return out;
    }
}

/**
 * @brief Singleton holding the static data for either the client or server side
 */
class RpcInstance;

/**
 * @brief Engine that implements a simple RPC mechanism
 */
class RpcEngine {
public:
    constexpr explicit RpcEngine(role r) : tls_(nullptr), role_(r)
#ifdef CONFIG_WIFI_RMT_OVER_EPPP_UNSECURE
        , plain_sock_(-1)
#endif
    {}

    esp_err_t init()
    {
#ifdef CONFIG_WIFI_RMT_OVER_EPPP_UNSECURE
        if (plain_sock_ >= 0) {
            return ESP_OK;
        }
#else
        if (tls_ != nullptr) {
            return ESP_OK;
        }
#endif
        if (role_ == role::CLIENT) {
            instance = init_client();
        }
        if (role_ == role::SERVER) {
            instance = init_server();
        }
        return instance == nullptr ? ESP_FAIL : ESP_OK;
    }

    void deinit()
    {
#ifdef CONFIG_WIFI_RMT_OVER_EPPP_UNSECURE
        if (plain_sock_ >= 0) {
            close(plain_sock_);
            plain_sock_ = -1;
        }
#endif
        if (tls_ != nullptr) {
            if (role_ == role::CLIENT) {
                esp_tls_conn_destroy(tls_);
            } else if (role_ == role::SERVER) {
                esp_tls_server_session_delete(tls_);
            }
            tls_ = nullptr;
        }
    }

    template<typename T>
    esp_err_t send(api_id id, T *t)
    {
        WireHeader head{
            .id = to_le32(static_cast<uint32_t>(id)),
            .size = to_le32(static_cast<uint32_t>(sizeof(T))),
        };
        alignas(T) uint8_t payload[sizeof(T)];
        to_wire_bytes(*t, payload);

        ESP_LOGD("rpc", "Sending API id:%d", (int) id);
        ESP_LOG_BUFFER_HEXDUMP("rpc", &head, sizeof(head), ESP_LOG_VERBOSE);
        ESP_LOG_BUFFER_HEXDUMP("rpc", payload, sizeof(payload), ESP_LOG_VERBOSE);

#ifdef CONFIG_WIFI_RMT_OVER_EPPP_UNSECURE
        int len = write(plain_sock_, &head, sizeof(head));
        if (len == (int) sizeof(head)) {
            len = write(plain_sock_, payload, sizeof(payload));
        }
#else
        int len = esp_tls_conn_write(tls_, &head, sizeof(head));
        if (len == (int) sizeof(head)) {
            len = esp_tls_conn_write(tls_, payload, sizeof(payload));
        }
#endif
        if (len <= 0) {
            ESP_LOGE("rpc", "Failed to write data to the connection");
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    esp_err_t send(api_id id) // overload for (void)
    {
        WireHeader head{
            .id = to_le32(static_cast<uint32_t>(id)),
            .size = to_le32(0),
        };
#ifdef CONFIG_WIFI_RMT_OVER_EPPP_UNSECURE
        int len = write(plain_sock_, &head, sizeof(head));
#else
        int len = esp_tls_conn_write(tls_, &head, sizeof(head));
#endif
        if (len <= 0) {
            ESP_LOGE("rpc", "Failed to write data to the connection");
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    int get_socket_fd()
    {
#ifdef CONFIG_WIFI_RMT_OVER_EPPP_UNSECURE
        return plain_sock_;
#else
        int sock;
        if (esp_tls_get_conn_sockfd(tls_, &sock) != ESP_OK) {
            return -1;
        }
        return sock;
#endif
    }

    RpcHeader get_header()
    {
        WireHeader wire{};
#ifdef CONFIG_WIFI_RMT_OVER_EPPP_UNSECURE
        int len = read(plain_sock_, (char *) &wire, sizeof(wire));
#else
        int len = esp_tls_conn_read(tls_, (char *) &wire, sizeof(wire));
#endif
        if (len <= 0) {
            if (len < 0 && errno != EAGAIN) {
                ESP_LOGE("rpc", "Failed to read header data from the connection %d %s", errno, strerror(errno));
                return {.id = api_id::ERROR, .size = 0};
            }
            return {.id = api_id::UNDEF, .size = 0};
        }
        return {
            .id = static_cast<api_id>(from_le32(wire.id)),
            .size = from_le32(wire.size),
        };
    }

    template<typename T>
    T get_payload(api_id id, RpcHeader &head)
    {
        if (head.id != id || head.size != sizeof(T)) {
            ESP_LOGE("rpc", "unexpected header %d %d or sizes %" PRIu32 " %zu",
                     (int)head.id, (int)id, head.size, sizeof(T));
            return {};
        }
        alignas(T) uint8_t payload[sizeof(T)];
#ifdef CONFIG_WIFI_RMT_OVER_EPPP_UNSECURE
        int len = read(plain_sock_, (char *) payload, sizeof(payload));
#else
        int len = esp_tls_conn_read(tls_, (char *) payload, sizeof(payload));
#endif
        if (len <= 0) {
            ESP_LOGE("rpc", "Failed to read data from the connection");
            return {};
        }
        return from_wire_bytes<T>(payload);
    }

private:
    RpcInstance *init_server();
    RpcInstance *init_client();
    esp_tls_t *tls_;
    role role_;
    RpcInstance *instance{nullptr};
#ifdef CONFIG_WIFI_RMT_OVER_EPPP_UNSECURE
    int plain_sock_;
#endif
};

};
