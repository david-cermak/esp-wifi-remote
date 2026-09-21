/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <meta>
#include <type_traits>

namespace eppp_rpc {

namespace detail {

template<typename M>
void fix_scalar_field(std::uint8_t *field, bool host_to_wire)
{
    if constexpr (sizeof(M) == 2) {
        if (host_to_wire) {
            M host_val{};
            std::memcpy(&host_val, field, sizeof(M));
            le16 w = to_le16(static_cast<std::uint16_t>(host_val));
            std::memcpy(field, &w, sizeof(w));
        } else {
            le16 w{};
            std::memcpy(&w, field, sizeof(w));
            M host_val = static_cast<M>(from_le16(w));
            std::memcpy(field, &host_val, sizeof(M));
        }
    } else if constexpr (sizeof(M) == 4) {
        if (host_to_wire) {
            M host_val{};
            std::memcpy(&host_val, field, sizeof(M));
            le32 w = to_le32(static_cast<std::uint32_t>(host_val));
            std::memcpy(field, &w, sizeof(w));
        } else {
            le32 w{};
            std::memcpy(&w, field, sizeof(w));
            M host_val = static_cast<M>(from_le32(w));
            std::memcpy(field, &host_val, sizeof(M));
        }
    }
}

}  // namespace detail

/**
 * Walk non-static data members and fix 16/32-bit integral/enum endianness
 * in a byte buffer (or object representation) of type T.
 *
 * @param base         Pointer to T's object representation
 * @param host_to_wire true: host → LE; false: LE → host
 *
 * Skips: bit-fields (storage left as memcpy'd), pointers, 1-byte fields,
 *        unions (opaque — specialize the parent if an active arm is known).
 */
template<typename T>
void rpc_fix_endian_members(void *base, bool host_to_wire)
{
    constexpr auto ctx = std::meta::access_context::current();
    template for (constexpr auto member : [: std::meta::reflect_constant_array(
                      std::meta::nonstatic_data_members_of(^^T, ctx)) :]) {
        using M = [: std::meta::type_of(member) :];
        constexpr auto off = std::meta::offset_of(member).bytes;
        auto *field = static_cast<std::uint8_t *>(base) + off;

        if constexpr (std::meta::is_bit_field(member)) {
            // Bit-field packing is ABI-specific; leave the storage word alone.
        } else if constexpr (std::is_pointer_v<M> || std::is_member_pointer_v<M>) {
            // Host pointers are meaningless on the other side; leave as copied.
        } else if constexpr (std::is_enum_v<M>) {
            using U = std::underlying_type_t<M>;
            if constexpr (sizeof(U) == 2 || sizeof(U) == 4) {
                detail::fix_scalar_field<U>(field, host_to_wire);
            }
        } else if constexpr (std::is_integral_v<M>) {
            if constexpr (sizeof(M) == 2 || sizeof(M) == 4) {
                detail::fix_scalar_field<M>(field, host_to_wire);
            }
        } else if constexpr (std::meta::is_array_type(^^M)) {
            using E = [: std::meta::remove_all_extents(^^M) :];
            constexpr std::size_t n = sizeof(M) / sizeof(E);
            if constexpr (sizeof(E) == 1) {
                // bytes / chars — endian-neutral
            } else if constexpr (std::is_enum_v<E>) {
                using U = std::underlying_type_t<E>;
                if constexpr (sizeof(U) == 2 || sizeof(U) == 4) {
                    for (std::size_t i = 0; i < n; ++i) {
                        detail::fix_scalar_field<U>(field + i * sizeof(E), host_to_wire);
                    }
                }
            } else if constexpr (std::is_integral_v<E>) {
                if constexpr (sizeof(E) == 2 || sizeof(E) == 4) {
                    for (std::size_t i = 0; i < n; ++i) {
                        detail::fix_scalar_field<E>(field + i * sizeof(E), host_to_wire);
                    }
                }
            } else if constexpr (std::meta::is_class_type(^^E) && !std::meta::is_union_type(^^E)) {
                for (std::size_t i = 0; i < n; ++i) {
                    rpc_fix_endian_members<E>(field + i * sizeof(E), host_to_wire);
                }
            }
        } else if constexpr (std::meta::is_union_type(^^M)) {
            // Overlapping arms — do not walk; caller specializes if needed.
        } else if constexpr (std::meta::is_class_type(^^M)) {
            rpc_fix_endian_members<M>(field, host_to_wire);
        }
    }
}

template<typename T>
void rpc_struct_to_wire(const T &host, void *out)
{
    std::memcpy(out, &host, sizeof(T));
    rpc_fix_endian_members<T>(out, true);
}

template<typename T>
T rpc_struct_from_wire(const void *in)
{
    T host{};
    std::memcpy(&host, in, sizeof(T));
    rpc_fix_endian_members<T>(&host, false);
    return host;
}

}  // namespace eppp_rpc
