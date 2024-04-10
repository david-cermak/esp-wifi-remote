/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <cstring>
#include <cerrno>

namespace eppp_rpc {

const int rpc_port = 3333;

enum class api_id : uint32_t {
    ERROR,
    UNDEF,
    INIT,
    SET_MODE,
    SET_CONFIG,
    START,
    STOP,
    CONNECT,
    GET_MAC,
    WIFI_EVENT,
    IP_EVENT,
};

enum class role {
    SERVER,
    CLIENT,
};

struct RpcHeader {
    api_id id;
    uint32_t size;
} __attribute((__packed__));

template<typename T>
struct RpcData {
    RpcHeader head;
    T value_{};
    explicit RpcData(api_id id) : head{id, sizeof(T)} {}

    uint8_t *value()
    {
        return (uint8_t *) &value_;
    }

    uint8_t *marshall(T *t, size_t &size)
    {
        size = head.size + sizeof(RpcHeader);
        memcpy(value(), t, sizeof(T));
        return (uint8_t *) this;
    }
} __attribute((__packed__));

class RpcInstance;

class RpcEngine {
public:
    explicit RpcEngine(role r) : tls_(nullptr), role_(r) {}

    esp_err_t init()
    {
        if (tls_ != nullptr) {
            return ESP_OK;
        }
        if (role_ == role::CLIENT) {
            instance = init_client();
        }
        if (role_ == role::SERVER) {
            instance = init_server();
        }
        return instance == nullptr ? ESP_FAIL : ESP_OK;
    }

    template<typename T>
    esp_err_t send(api_id id, T *t)
    {
        RpcData<T> req(id);
        size_t size;
        auto buf = req.marshall(t, size);
        ESP_LOGD("rpc", "Sending API id:%d", (int) id);
        ESP_LOG_BUFFER_HEXDUMP("rpc", buf, size, ESP_LOG_VERBOSE);
        int len = esp_tls_conn_write(tls_, buf, size);
        if (len <= 0) {
            ESP_LOGE("rpc", "Failed to write data to the connection");
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    esp_err_t send(api_id id) // specialization for (void)
    {
        RpcHeader head = {.id = id, .size = 0};
        int len = esp_tls_conn_write(tls_, &head, sizeof(head));
        if (len <= 0) {
            ESP_LOGE("rpc", "Failed to write data to the connection");
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    RpcHeader get_header()
    {
        RpcHeader header{};
        int len = esp_tls_conn_read(tls_, (char *) &header, sizeof(header));
        if (len <= 0) {
            if (len < 0 && errno != EAGAIN) {
                ESP_LOGE("rpc", "Failed to read header data from the connection %d %s", errno, strerror(errno));
                return {.id = api_id::ERROR, .size = 0};
            }
            return {.id = api_id::UNDEF, .size = 0};
        }
        return header;
    }

    template<typename T>
    T get_payload(api_id id, RpcHeader &head)
    {
        RpcData<T> resp(id);
        if (head.id != id || head.size != resp.head.size) {
            ESP_LOGE("rpc", "unexpected header %d %d or sizes %" PRIu32 " %" PRIu32, (int)head.id, (int)id, head.size, resp.head.size);
            return {};
        }
        int len = esp_tls_conn_read(tls_, (char *) resp.value(), resp.head.size);
        if (len <= 0) {
            ESP_LOGE("rpc", "Failed to read data from the connection");
            return {};
        }
        return resp.value_;
    }

private:
    RpcInstance *init_server();
    RpcInstance *init_client();
    esp_tls_t *tls_;
    role role_;
    RpcInstance *instance{nullptr};
};

};
