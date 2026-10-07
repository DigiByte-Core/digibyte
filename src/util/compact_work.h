// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_UTIL_COMPACT_WORK_H
#define DIGIBYTE_UTIL_COMPACT_WORK_H

#include <arith_uint256.h>
#include <crypto/common.h>
#include <uint256.h>

#include <cstdint>
#include <type_traits>

/** Store work exactly; callers keep doing arithmetic with arith_uint256. */
class CompactWork
{
    struct Small {
        uint16_t kind;
        uint16_t high;
        uint32_t middle;
        uint64_t low;
    };

    struct Large {
        uint16_t kind;
        uint16_t unused_high;
        uint32_t unused_middle;
        arith_uint256* value;
    };

    union Storage {
        Small small;
        Large large;

        constexpr Storage() noexcept : small{} {}
    } m_storage;

    static_assert(std::is_standard_layout_v<Small> && std::is_standard_layout_v<Large>);
    static_assert(std::is_trivially_copyable_v<Storage>);
    static_assert(sizeof(Storage) <= 16);

    bool IsLarge() const noexcept
    {
        // Both union members start with the same tag. Reading this common
        // initial field is valid whichever member is active.
        return m_storage.small.kind != 0;
    }

    void Clear() noexcept
    {
        if (IsLarge()) delete m_storage.large.value;
        m_storage.small = Small{};
    }

public:
    CompactWork() noexcept = default;
    explicit CompactWork(arith_uint256 value) { Set(value); }
    CompactWork(const CompactWork& other) { Set(other.Get()); }

    CompactWork(CompactWork&& other) noexcept : m_storage{other.m_storage}
    {
        other.m_storage.small = Small{};
    }

    ~CompactWork() { Clear(); }

    CompactWork& operator=(const CompactWork& other)
    {
        if (this != &other) Set(other.Get());
        return *this;
    }

    CompactWork& operator=(CompactWork&& other) noexcept
    {
        if (this != &other) {
            Clear();
            m_storage = other.m_storage;
            other.m_storage.small = Small{};
        }
        return *this;
    }

    arith_uint256 Get() const noexcept
    {
        if (IsLarge()) return *m_storage.large.value;

        uint256 bytes;
        WriteLE64(bytes.begin(), m_storage.small.low);
        WriteLE32(bytes.begin() + 8, m_storage.small.middle);
        WriteLE16(bytes.begin() + 12, m_storage.small.high);
        return UintToArith256(bytes);
    }

    void Set(arith_uint256 value)
    {
        if (value.bits() <= 112) {
            const uint256 bytes = ArithToUint256(value);
            const Small replacement{0, ReadLE16(bytes.begin() + 12), ReadLE32(bytes.begin() + 8), ReadLE64(bytes.begin())};
            Clear();
            m_storage.small = replacement;
        } else if (IsLarge()) {
            *m_storage.large.value = value;
        } else {
            // Allocate before replacing the old value so a failed allocation
            // leaves the object unchanged.
            arith_uint256* replacement = new arith_uint256{value};
            m_storage.large = Large{1, 0, 0, replacement};
        }
    }
};

#endif // DIGIBYTE_UTIL_COMPACT_WORK_H
