# Implementation plan: reflection-based RPC

Background and rationale: [RPC_with_reflection.md](RPC_with_reflection.md).

This plan improves the EPPP WiFi-remote control RPC using C++26 static reflection. The wire format stays `{RpcHeader | POD blob}`. Reflection is used for logging, memcpy-safety checks, pointer strip/rebind, and generating host/slave glue from one API catalog.

## Goals

- One catalog of RPCs instead of three parallel lists (`api_id`, client stub, server `switch`).
- Compile-time rejection of payloads that cannot be memcpy’d (pointers, function tables).
- Structured verbose logs of payloads (field names), without `iostream`.
- Cover the “simple” majority of `esp_wifi_remote_api.h` without per-API boilerplate.
- Keep `extern "C" esp_wifi_remote_*` as the public ABI.

## Non-goals

- New self-describing / TLV wire protocol.
- Auto-wrapping callbacks, `void *` + length blobs, or unbounded arrays.
- Changing TLS, EPPP data path, or FreeRTOS sync.
- Supporting `-freflection` on older IDF toolchains that lack it.

## Constraints

- Latest IDF Clang toolchain with `-freflection` (as in `ext/reflection`). Gate the flag with `check_cxx_compiler_flag`; do not break the component on older compilers — keep the current handwritten path as fallback until the catalog path is complete and the min IDF is bumped.
- No `iostream` / `sstream` in this component. Logging goes through `esp_log`.
- Host and slave IDF headers must stay size-compatible (already assumed). `get_payload` keeps the `size` check.
- `wifi_config_t` is a union; the discriminator is `wifi_interface_t`, already in `esp_wifi_remote_config`.
- Protocol change, if any, is additive (`api_id` values for new APIs). Existing IDs stay stable.

## Current bugs to fix on the way

- Client implements `esp_wifi_remote_stop()`; slave `handle_commands()` has no `api_id::STOP` case.

---

## Phase 0 — Toolchain and STOP

**Outcome:** Component builds with `-freflection` on latest IDF; `STOP` works.

### Tasks

1. In `CMakeLists.txt`, after `idf_component_register`:
   - `check_cxx_compiler_flag(-freflection ...)`
   - `target_compile_options(${COMPONENT_LIB} PRIVATE -freflection)` when supported
   - `target_compile_definitions(... WIFI_RMT_HAS_REFLECTION=1)`
2. Add `src/wifi_remote_rpc_meta.hpp` (empty facade for now) included from `wifi_remote_rpc_impl.hpp` only when `WIFI_RMT_HAS_REFLECTION`.
3. Add `case api_id::STOP` on the slave (same pattern as `START` / `CONNECT`). Host-side netif `started` flag should clear on stop if it is set on start.
4. Smoke-build `tests/sample` and `examples/server` with latest IDF.

### Files

- `CMakeLists.txt`
- `src/wifi_remote_rpc_server.cpp`
- `src/wifi_remote_rpc_meta.hpp` (new)

### Done when

- Latest IDF build passes with `-freflection`.
- `esp_wifi_remote_stop()` round-trips and returns the slave `esp_wifi_stop()` result.

---

## Phase 1 — Payload dump and enumerator names

**Outcome:** Verbose logs show `SET_CONFIG` and struct fields, not only a hex dump.

### Tasks

1. Implement in `wifi_remote_rpc_meta.hpp`:
   - `const char *rpc_api_name(api_id)` via `std::meta::enumerators_of(^^api_id)`
   - `template <typename T> void rpc_dump(const char *tag, const T &obj)`
     - Walk `nonstatic_data_members_of`
     - Scalars, enums, bit-fields, `uint8_t[N]` (MAC vs SSID/password like `ext/reflection`)
     - Nested structs recurse
     - Pointers: print `(ptr)` / skipped, do not dereference
     - Unions: dump `sizeof` only, unless a specialization is provided
   - Cap output (~512–1024 bytes) on a stack buffer; `ESP_LOGD`/`ESP_LOGV` only
2. Specialize dump of `esp_wifi_remote_config` to print `interface` and the matching `ap` or `sta` arm.
3. Call `rpc_dump` from `RpcEngine::send<T>` and `get_payload<T>` next to the existing hex dump. Replace integer `(int)id` logs with `rpc_api_name`.
4. Without reflection, keep current hex dumps.

### Files

- `src/wifi_remote_rpc_meta.hpp`
- `src/wifi_remote_rpc_impl.hpp`

### Done when

- Connecting a station at `ESP_LOG_VERBOSE` prints `wifi_sta_config_t` fields (ssid, bssid, authmode, …) on host and slave.
- No `iostream` in the component; flash cost of dump code is acceptable at verbose-only (check `.map` if needed).

---

## Phase 2 — Compile-time memcpy safety

**Outcome:** `send<T>` / `get_payload<T>` will not compile for types that cannot be copied as a blob.

### Tasks

1. `consteval bool rpc_memcpy_safe(std::meta::info type)`:
   - false: pointer, member pointer, function, union (unless opted in)
   - false: class/struct that has any unsafe member (recurse)
   - true: scalar, enum, bit-field, array of safe types, safe class/struct
2. Trait `template <typename T> concept RpcBlob = ...` wrapping that check.
3. `static_assert(RpcBlob<T>)` in `RpcEngine::send<T>` and `get_payload<T>`.
4. Opt-in specializations (still memcpy, but acknowledged):
   - `wifi_config_t` / `esp_wifi_remote_config` — union, same size both ends
   - `wifi_init_config_t` — **not** safe yet; either exclude from the generic path or wait for phase 3
5. Confirm current APIs still compile. `INIT` should fail the assert until phase 3; keep a named `send_init` / `get_payload_unchecked` escape hatch used only there.

### Files

- `src/wifi_remote_rpc_meta.hpp`
- `src/wifi_remote_rpc_impl.hpp`
- `src/wifi_remote_rpc_server.cpp` (`INIT` hatch)

### Done when

- Adding `send<wifi_scan_config_t>` fails to compile (contains `uint8_t *`).
- Existing non-INIT APIs still build.

---

## Phase 3 — Strip / rebind local resources

**Outcome:** `INIT` uses the generic engine; no handwritten pointer fixes.

### Tasks

1. `rpc_zero_nonblobs(T &)`: set pointer members (and nested function-pointer structs) to `{}` / `nullptr` before marshalling.
2. `rpc_rebind_wifi_init(wifi_init_config_t &)` on the slave:
   - `osi_funcs = &g_wifi_osi_funcs`
   - `wpa_crypto_funcs = g_wifi_default_wpa_crypto_funcs`
   - Prefer generating “rebind every pointer-like member we know how to fill” from a small map (`^^wifi_init_config_t::osi_funcs` → `&g_wifi_osi_funcs`, …) so a new IDF function-pointer field is a map update, not a missed assignment.
3. Host `esp_wifi_remote_init`: copy config, `rpc_zero_nonblobs`, then `send`.
4. Slave `INIT` case: `get_payload`, `rpc_rebind_wifi_init`, `esp_wifi_init`.
5. Mark `wifi_init_config_t` as allowed through the hatch, or as `RpcBlob` after stripping (strip is a runtime transform; the type itself stays unsafe — keep the hatch).

### Files

- `src/wifi_remote_rpc_meta.hpp`
- `src/wifi_remote_rpc_client.cpp`
- `src/wifi_remote_rpc_server.cpp`

### Done when

- The two magic assignments are gone from the `INIT` case.
- Station init still works (slave uses its own OSI/crypto tables).

---

## Phase 4 — RPC catalog and generated dispatch

**Outcome:** Simple APIs are declared once; slave `switch` and most host glue disappear.

### Design

New `src/wifi_remote_rpc_catalog.hpp`:

```cpp
enum class dir { in, out };

template <api_id Id, auto NativeFn, dir... Dirs>
struct rpc_api {
    static constexpr api_id id = Id;
    static constexpr auto native = NativeFn;
};

using rpc_catalog = std::tuple<
    rpc_api<api_id::SET_MODE,    ^^esp_wifi_set_mode,    dir::in>,
    rpc_api<api_id::START,       ^^esp_wifi_start>,
    rpc_api<api_id::GET_MAC,     ^^esp_wifi_get_mac,     dir::in, dir::out>,
    rpc_api<api_id::SET_CONFIG,  ^^esp_wifi_set_config,  dir::in, dir::in>,
    // ...
>;
```

Rules:

- Parameter count must equal `sizeof...(Dirs)` (or 0). `static_assert` in the template.
- `dir::in` by-value or `const T*` / non-const `T*` that we *declare* as in: copy pointee into the request blob.
- `dir::out`: live in the response blob; slave writes, host copies out.
- Return value is always the first field of the response (`esp_err_t` almost everywhere; `int64_t` for `get_tsf_time`).
- Request/response types: `std::tuple` of decayed in/out types, or `define_aggregate` if we want named fields for dumps. Tuple is enough for the wire; wrap in a packed struct for `RpcData<T>`.
- Header-only commands: empty `Dirs`, `size == 0` request, `esp_err_t` response (current `START`/`STOP`/… already return `esp_err_t`).

### Tasks

1. Implement `invoke_slave<Desc>(RpcEngine &, RpcHeader)`:
   - unpack request, splice `[:Desc::native:](args...)`, send response.
2. Replace `handle_commands()` `switch` with `template for` over `rpc_catalog`. Keep a fallback only for events (`WIFI_EVENT`, `IP_EVENT`) — those stay in `marshall_events` / `perform`, not the catalog.
3. Host: `template <typename Desc, typename... Args> auto rpc_call(Args&&...)` that packs, `send`, `get_resp`. Each `extern "C" esp_wifi_remote_*` becomes a one-line trampoline.
4. Migrate **existing** APIs onto the catalog first (including `STOP`, `INIT` via the phase-3 hatch if INIT cannot be fully generic). Behaviour must not change.
5. Generate `api_id` values from the catalog if practical (`std::meta::enumerators` still used for names). Until then, keep the enum and `static_assert` that every catalog `Id` exists and is unique.

### Files

- `src/wifi_remote_rpc_catalog.hpp` (new)
- `src/wifi_remote_rpc_invoke.hpp` (new, host+slave helpers)
- `src/wifi_remote_rpc_server.cpp` — `handle_commands` shrinks
- `src/wifi_remote_rpc_client.cpp` — stubs shrink
- `src/wifi_remote_rpc_params.h` — keep only types the catalog cannot express yet (`esp_wifi_remote_eppp_ip_event`); `esp_wifi_remote_config` / `esp_wifi_remote_mac_t` should become catalog-generated

### Done when

- Existing APIs work with no per-API `switch` cases.
- Adding a by-value API is: one `api_id`, one catalog line, one trampoline.
- `tests/sample` + `examples/server` still pass.

---

## Phase 5 — Fill the simple API surface

**Outcome:** Most of `esp_wifi_remote_api.h` works over EPPP.

Add in batches. Each batch: catalog lines + trampolines + a host/slave test that calls the new APIs.

### Batch A — header-only / scalar in, `esp_err_t` out

`restore`, `clear_fast_connect`, `scan_stop`, `clear_ap_list`, `deauth_sta`, `set_ps` / `get_ps`, `set_promiscuous` / `get_promiscuous`, `set_max_tx_power` / `get_max_tx_power`, `set_event_mask` / `get_event_mask`, `ftm_end_session`, `force_wakeup_acquire` / `release`, `set_dynamic_cs`, `set_csi`, `statis_dump`, `set_rssi_threshold`, `disable_pmf_config`

### Batch B — POD in/out structs (memcpy-safe)

`set_scan_parameters` / `get_scan_parameters`, `set_country` / `get_country`, `set_mac`, `get_mode`, `set_protocol` / `get_protocol`, `set_bandwidth` / `get_bandwidth`, `set_channel` / `get_channel` / `get_home_channel`, `set_promiscuous_filter` / `get_*`, `get_config`, `set_csi_config` / `get_csi_config`, `set_inactive_time` / `get_inactive_time`, `get_tsf_time`, `sta_get_aid`, `sta_get_rssi`, `sta_get_negotiated_phymode`, `set_band` / `get_band`, `set_band_mode` / `get_band_mode`, `set_protocols` / `get_protocols`, `set_bandwidths` / `get_bandwidths`, `config_11b_rate`, `config_80211_tx_rate`, `config_80211_tx`, `ap_get_sta_aid`, `ftm_initiate_session`, `ftm_resp_set_offset`, `connectionless_module_set_wake_interval`

`get_config` uses `dir::in, dir::out` with `wifi_config_t` (union opt-in from phase 2).

### Batch C — bounded arrays

`scan_get_ap_record`, `sta_get_ap_info`, `ap_get_sta_list` (`wifi_sta_list_t` is bounded), `get_country_code` (`char[3]` style), `set_country_code`

`scan_get_ap_num` is a single out `uint16_t`.

### Done when

- Weak `esp_wifi_remote_*` symbols for batches A–C are all implemented by this component.
- Station + AP smoke: set/get mode, config, mac, channel, country, ps, start/stop/connect.

---

## Phase 6 — Special APIs (hand-written, not catalog-generic)

These stay explicit. Use `RpcEngine` primitives; do not force them into `dir::in/out`.

| API | Approach |
| --- | --- |
| `scan_start` | Copy `wifi_scan_config_t` values; inline `ssid`/`bssid` as `uint8_t ssid[32]`, `uint8_t bssid[6]` + presence flags in a wire struct |
| `scan_get_ap_records` | Request `max_number`; response `count` + `wifi_ap_record_t[N]` with compile-time cap (Kconfig, e.g. 16) or heap buffer + size in header (header `size` already supports this) |
| `ftm_get_report` | Same pattern as scan records |
| `80211_tx` / `set_vendor_ie` | Length-prefixed blob after a small header; cap length |
| `action_tx_req` / `remain_on_channel` | Inspect IDF structs for internal pointers; specialize |
| Callbacks (`set_promiscuous_rx_cb`, CSI, vendor IE cb, 802.11 tx done) | **Do not remote.** Document as unsupported over EPPP (run on the slave only), or return `ESP_ERR_NOT_SUPPORTED` |

Phase 6 can proceed in parallel with late phase 5 once the catalog is stable.

---

## Suggested file layout (end state)

```
src/
  wifi_remote_rpc_impl.hpp       # transport: header, send, recv, TLS/socket
  wifi_remote_rpc_meta.hpp       # dump, memcpy_safe, zero/rebind
  wifi_remote_rpc_catalog.hpp    # api_id catalog, dir tags
  wifi_remote_rpc_invoke.hpp     # invoke_slave, rpc_call
  wifi_remote_rpc_params.h       # events + remaining special wire structs
  wifi_remote_rpc_client.cpp     # trampolines + session
  wifi_remote_rpc_server.cpp     # select loop, events, handle_commands
  eppp_init.c
```

Transport stays in `RpcEngine`. Reflection does not leak into EPPP or netif code.

## Testing

| Level | What |
| --- | --- |
| Build | `tests/sample` host + `examples/server` slave, latest IDF, `-freflection` |
| Existing | Station connect path (init, set_mode, set_config, start, connect, get_mac, IP event) |
| New | After each phase-5 batch, a small host test that set/get round-trips the new APIs |
| Negative | (dev) try `send<wifi_scan_config_t>` and confirm phase-2 `static_assert` |
| Size | Compare `.map` / `idf_size` before/after dump + catalog; dump must stay behind verbose |

No protocol compatibility tests against old slaves for new `api_id`s. Old IDs must keep working.

## Order and independence

```
Phase 0 (flag + STOP)
  → Phase 1 (dump)          // optional for later phases, ship anytime
  → Phase 2 (memcpy_safe)
      → Phase 3 (INIT strip/rebind)
          → Phase 4 (catalog, migrate existing APIs)
              → Phase 5 (batches A–C)
              → Phase 6 (scan/blobs, in parallel after 4)
```

Phases 1–3 do not change the protocol. Phase 4 is the design change. Phase 5 is the product expansion.

## Risks

- **Compile time / RAM of `template for` over ~80 APIs.** If slave `.text` or build time blows up, split the catalog (core vs extended) with Kconfig.
- **`dir::in` on non-const pointers** is easy to get wrong. Catalog review is mandatory; the compiler will not catch semantic in vs out.
- **`wifi_init_config_t` layout** differs by `CONFIG_WIFI_RMT_*` / slave Kconfig. Keep the size check; document that host and slave WiFi buffer configs must match.
- **Reflection as experimental Clang.** Pin the IDF version in `idf_component.yml` once this lands; feature-test the fallback until then.

## Product

The finished component is a full EPPP backend for `esp_wifi_remote`: a host MCU calls the usual `esp_wifi_remote_*` API and the slave runs native `esp_wifi_*`, with commands and events on the existing TLS (or plain TCP) control path. Almost every memcpy-safe WiFi API is remoted from a single catalog; adding one is a catalog line and a C trampoline, not a three-file edit. Unsafe payloads are rejected at compile time, `INIT` rebinds slave-local function tables automatically, and verbose logs print struct fields instead of hex. Scan lists and length-prefixed frames are supported as explicit special cases; callbacks stay on the slave (or return `ESP_ERR_NOT_SUPPORTED`). The public ABI, wire header, and EPPP data path do not change — applications keep linking `esp_wifi_remote` the same way, with broader API coverage and safer glue.


### Implementing reflection based RPC for Wi-Fi connectivity

This talk introduces a practical use of C++26 static reflection to implement RPC between host and slave nodes.
The reflection is used to
* compile-time catalogue public API and structures of complex Wi-Fi configuration.
* safe wire serialization of requests/responses
* verbose/semantic logging of Wi-Fi configuration
