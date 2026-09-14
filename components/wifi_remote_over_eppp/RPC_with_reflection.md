# RPC and C++26 static reflection

This note reviews the EPPP WiFi-remote RPC (`wifi_remote_rpc_impl.hpp` plus client/server) and maps it onto C++26 static reflection as demonstrated in `ext/reflection`. Conclusion first: **yes, we can benefit**, but not by replacing the wire protocol with a fully automatic serializer. The high-value uses are compile-time safety, payload logging, and generating the per-API glue that is currently handwritten on both sides.

The toolchain already supports this (`-freflection`, `<meta>`). The example pretty-prints `wifi_sta_config_t` — the same IDF C struct that `SET_CONFIG` already ships as a blob.

## Current RPC

Control path is a request/response binary protocol over TLS (or a plain TCP socket). Layout:

```
[ RpcHeader { api_id id; uint32_t size; } | payload of `size` bytes ]
```

`RpcEngine::send<T>` / `get_payload<T>` treat `T` as a packed POD: `memcpy` of `sizeof(T)`. Void commands send a header with `size == 0`. Async slave→host notifications reuse the same framing (`WIFI_EVENT`, `IP_EVENT`).

Each API is wired in **three places** that must stay in sync:

| Piece | Where |
| --- | --- |
| `api_id` enumerator | `wifi_remote_rpc_impl.hpp` |
| Host stub `esp_wifi_remote_*` | `wifi_remote_rpc_client.cpp` |
| Slave `switch (header.id)` + native `esp_wifi_*` call | `wifi_remote_rpc_server.cpp` |

Multi-argument APIs also need a hand-written bag struct in `wifi_remote_rpc_params.h` (`esp_wifi_remote_config`, `esp_wifi_remote_mac_t`, …).

Covered today: `INIT`, `DEINIT`, `SET_MODE`, `SET_CONFIG`, `START`, `STOP`, `CONNECT`, `DISCONNECT`, `GET_MAC`, `SET_STORAGE`, plus the two events. That is a small slice of `esp_wifi_remote_api.h` (~90 entry points).

Transport, EPPP data path, and FreeRTOS sync are unrelated to reflection and should stay as they are.

### Properties that matter for reflection

1. **Memcpy is the serializer.** Same-family GCC, little-endian, matching `sizeof(T)` on both ends. Fast and simple. It also means pointers and function tables on the wire are host addresses, useless on the slave.
2. **`wifi_init_config_t` is already a special case.** After unmarshalling, the server overwrites `osi_funcs` and `wpa_crypto_funcs` with the slave’s own tables. Those members are a pointer and a struct of function pointers. Reflection can *see* that; memcpy cannot.
3. **`wifi_config_t` is a union** (`ap` / `sta` / `nan`). The discriminator is the separate `interface` argument, not a field inside the union. The example reflects `wifi_config.sta` (a struct), not the union.
4. **Out-parameters and variable-length data** are not modelled. `GET_MAC` uses an ad-hoc response struct. APIs such as `esp_wifi_scan_start` (`uint8_t *ssid`, `uint8_t *bssid`) or `scan_get_ap_records` cannot be memcpy’d safely.
5. **Logging is a hex dump.** `ESP_LOG_BUFFER_HEXDUMP` of the packed buffer. The reflection example’s `to_string` is strictly better for humans, once the formatter is not `iostream`.

Incidental gap (not reflection-related): the client implements `esp_wifi_remote_stop()`, but `handle_commands()` has no `api_id::STOP` case, so that call fails on the slave.

## What the example actually proves

`ext/reflection/main/station_example_main.cpp` does one thing well: walk **non-static data members of an unmodified IDF C struct at compile time** and print them.

Relevant surface (P2996, enabled with `-freflection`):

- `^^T` — reflect a type or entity
- `[: r :]` — splice a reflection back into the program
- `template for` — expansion statement (compile-time loop that can contain `if constexpr`)
- `std::meta::nonstatic_data_members_of(^^T, ctx)`
- `std::meta::identifier_of`, `type_of`, `is_array_type`
- `obj.[:m:]` — member access via splice (works for bit-fields too)

That is enough to recurse through `wifi_sta_config_t`, `wifi_ap_config_t`, `esp_netif_ip_info_t`, `wifi_init_config_t`, etc. **without** annotating IDF headers.

It does **not** by itself: invent a wire format, know which union arm is live, know which pointer is an in-buffer vs an out-buffer, or generate `esp_wifi_remote_*` C symbols. Those need a thin layer of descriptors on top.

Do not copy the example’s `<iostream>` / `ostringstream` into this component. Use `esp_log` and a small stack buffer.

## Where reflection helps

Ranked by payoff vs. risk.

### 1. Structured payload logs (easy, proven)

Same algorithm as `to_string` in the example, hooked from `RpcEngine::send` / `get_payload` at verbose level.

```cpp
template <typename T>
void rpc_log_payload(const char *tag, const T &obj)
{
    constexpr auto ctx = std::meta::access_context::current();
    ESP_LOGD(tag, "%s", std::meta::identifier_of(^^T).data());
    template for (constexpr auto m :
        [: std::meta::reflect_constant_array(
            std::meta::nonstatic_data_members_of(^^T, ctx)) :])
    {
        using member_type = [: std::meta::type_of(m) :];
        // arrays / uint8_t / enums / nested structs — same split as the example
        ESP_LOGD(tag, "  %s = ...", std::meta::identifier_of(m).data());
        (void)obj.[:m:];
    }
}
```

Also reflect `api_id` enumerators (`std::meta::enumerators_of(^^api_id)`) so logs say `SET_CONFIG` instead of `6`.

This is the only item that is a drop-in of the working example. Unions should log the active arm (`sta` vs `ap`) using the `wifi_interface_t` already in `esp_wifi_remote_config`, not every overlapping member.

### 2. Compile-time “is this type memcpy-safe?” (easy, high leverage)

Today a pointer in `T` compiles and ships a host address. Reflection can reject that at compile time, recursively:

- pointer or member-pointer → not safe (unless a specialization says otherwise)
- function type / struct of function pointers (`wpa_crypto_funcs_t`) → not safe
- union → not safe unless a discriminator is supplied
- nested class/struct → recurse
- array of the above → recurse
- scalar, enum, bit-field, trivial array of bytes → safe

`send<T>` / `get_payload<T>` would `static_assert` that trait. `wifi_init_config_t` and `wifi_scan_config_t` then fail the assert instead of failing in the field, and we keep explicit specializations for them.

This is the single best “correctness” win: it scales as we add the remaining ~80 APIs.

### 3. Strip / restore non-serializable members (medium, fixes INIT properly)

For types that are *almost* POD, generate the pre/post memcpy steps that are now handwritten:

**Host:** zero every pointer (and every nested function-pointer struct) before `marshall`.
**Slave:** after `get_payload`, splice the slave-local defaults back (`&g_wifi_osi_funcs`, `g_wifi_default_wpa_crypto_funcs`).

That replaces the two magic assignments in the `INIT` case with a generic “rebind local resources” pass, and it stays correct if IDF adds another function-pointer field to `wifi_init_config_t`.

Memcpy of the remaining scalars/bit-fields stays the wire format. No need for a TLV protocol just because we can walk members.

### 4. One catalog → client stubs + server dispatch (medium–high, the real boilerplate killer)

The pain is not serialization of `wifi_mode_t`. It is repeating `api_id`, send, wait, switch-case, native call for every new API. Reflection of **functions** (not only structs) is what collapses that.

Sketch of a single source of truth:

```cpp
template <api_id Id, auto NativeFn>
struct rpc_api {
    static constexpr api_id id = Id;
    static constexpr auto native = NativeFn;   // ^^esp_wifi_set_mode
};

using catalog = std::tuple<
    rpc_api<api_id::SET_MODE,    ^^esp_wifi_set_mode>,
    rpc_api<api_id::START,       ^^esp_wifi_start>,
    rpc_api<api_id::CONNECT,     ^^esp_wifi_connect>,
    rpc_api<api_id::SET_STORAGE, ^^esp_wifi_set_storage>
    // ...
>;
```

From `^^esp_wifi_set_mode` we can take `std::meta::parameters_of` / return type and:

- **Build the request struct** (today’s `esp_wifi_remote_config`) with `std::meta::define_aggregate`, or a tuple of decayed parameter types. Pointer-to-struct in-parameters become the pointee by value.
- **Generate the slave case:** `get_payload<Req>()`, splice `[: NativeFn :](args...)`, `send` the return value (and out-params).
- **Generate the host body** of `esp_wifi_remote_*`: pack args, `send(Id, &req)`, `get_resp<Ret>(Id)`. The `extern "C"` symbol still needs a named function for the linker; a small macro or an explicit list of wrappers is enough.

`template for` over the catalog replaces the giant `switch`. Adding `SET_PS` becomes one tuple line plus an `extern "C"` trampoline, not three edited files.

Heuristic for in vs out (good enough for most of `esp_wifi_*`, not perfect):

| Parameter | Treat as |
| --- | --- |
| `T` by value, `const T*`, `const T[]` | in → copy into request |
| non-const `T*` / `T[]` | out → allocate in response |
| `void`, no args | header-only command |
| `void *` + `len`, callbacks | **not** auto-generated; explicit specialization |

`esp_wifi_set_config(wifi_interface_t, wifi_config_t *)` is an in-parameter that is a non-const pointer (classic ESP-IDF style). The heuristic would mis-classify it. Mark those APIs in the catalog (`in_ptr` / `out_ptr`) rather than trying to be fully automatic.

### 5. Memberwise wire format (possible, mostly not worth it)

A recursive member walk could emit a self-describing or field-by-field stream: skip padding, skip pointers, maybe survive modest IDF header drift between host and slave.

For this component that is the wrong default:

- Host and slave are both ESP, same endianness, and we already reject size mismatches in `get_payload`.
- Memberwise encoding is larger in flash, slower, and still cannot invent a meaning for `uint8_t *ssid`.
- Cross-IDF layout drift is better solved by keeping slave/host IDF versions aligned (already a product constraint) or by an explicit versioned IDL — not by reflecting whatever header happened to be included.

Use member walks for **validation, logging, and stripping**, not as the on-wire codec.

## Where reflection does not help

- **TLS / socket / `select` / eventfd / FreeRTOS queues.** No types to reflect that would remove code.
- **EPPP data path and host-side netif channels.** Independent of RPC payloads.
- **Callbacks** (`set_promiscuous_rx_cb`, CSI, 802.11 tx done, vendor IE). Cannot be remoted by walking a function type.
- **Variable-length blobs** (`scan_get_ap_records`, `80211_tx`, vendor IEs, FTM reports). Need an explicit length on the wire; reflection of `T*` does not give a bound.
- **C ABI of `esp_wifi_remote_*`.** Must remain `extern "C"` for `esp_wifi_remote`. Reflection generates bodies, not a new public API.
- **Union tagging.** `wifi_config_t` still needs `wifi_interface_t` (already in `esp_wifi_remote_config`). Reflecting the union without the discriminator would dump overlapping `ap`/`sta`/`nan` garbage — do not do that.
- **Replacing `memcpy` for the APIs we already have.** `wifi_mode_t`, `wifi_storage_t`, `esp_err_t`, MAC arrays are already the right wire types.

## Recommended path

Keep the existing header+blob protocol and `RpcEngine`. Layer reflection beside it.

| Phase | What | Why |
| --- | --- | --- |
| 0 | Compile this component with `-freflection` (as in `ext/reflection/main/CMakeLists.txt`) | Unlocks `<meta>` |
| 1 | `rpc_dump<T>` + enumerator names, verbose only, no iostream | Immediate debug win; same code as the example |
| 2 | `rpc_memcpy_safe<T>` `static_assert` on `send`/`get_payload` | Stops unsafe APIs from landing |
| 3 | Generic strip/rebind of pointers for `wifi_init_config_t` | Deletes the INIT special case as a one-off |
| 4 | `catalog` tuple + generated slave dispatch for the “simple” APIs (by-value / header-only / POD in, `esp_err_t` out) | Unblocks filling out `esp_wifi_remote_api.h` |
| 5 | Hand-written specializations only for scan lists, 802.11 buffers, callbacks | Reflection does not remove judgment there |

Phase 4 is where the design of the RPC should change: **one catalog**, not three parallel lists. Phases 1–3 do not change the protocol and can ship independently.

## Suggested shape (phase 4)

```cpp
// wifi_remote_rpc_catalog.hpp
enum class dir { in, out, in_opt };

template <api_id Id, auto Fn, dir... Dirs>
struct rpc_api;

using catalog = std::tuple<
    rpc_api<api_id::SET_MODE,    ^^esp_wifi_set_mode>,
    rpc_api<api_id::START,       ^^esp_wifi_start>,
    rpc_api<api_id::GET_MAC,     ^^esp_wifi_get_mac, dir::in, dir::out>,
    rpc_api<api_id::SET_CONFIG,  ^^esp_wifi_set_config, dir::in, dir::in>
>;
```

Server:

```cpp
esp_err_t handle_commands() {
    auto header = rpc.get_header();
    template for (constexpr auto desc : /* splice catalog */) {
        if (header.id == [:desc:].id) {
            return invoke_slave<[:desc:]>(rpc, header);
        }
    }
    return ESP_FAIL;
}
```

`invoke_slave` uses `parameters_of(Fn)` to pick `get_payload` vs header-only, calls `[:Fn:](...)`, sends the return (and out-params). Host trampolines stay thin `extern "C"` functions so the published API does not move.

`STOP` becomes a one-liner in the catalog — which also closes the current client/server mismatch.

## Bottom line

C++26 static reflection is a good fit for this RPC because the payloads **are** the IDF C structs the example already reflects, and because the maintenance cost is **N copies of glue**, not the memcpy itself.

Use it to:

- dump payloads with field names (the example, almost as-is),
- prove at compile time that a payload may be memcpy’d,
- strip/rebind function pointers as IDF structs evolve,
- generate client/server glue from one catalog of `^^esp_wifi_*` functions.

Do not use it to:

- invent a new self-describing wire protocol,
- auto-wrap every `esp_wifi_*` including callbacks and length-prefixed buffers,
- replace sockets, TLS, or EPPP.

The first useful patch is phase 1+2: `-freflection`, a firmware-friendly `rpc_dump`, and `rpc_memcpy_safe<T>` on the existing `RpcEngine` templates.
