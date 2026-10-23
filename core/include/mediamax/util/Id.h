#pragma once

// Lightweight identifier types used throughout the engine.
//
// Id values are opaque 32-bit integers allocated by an IdAllocator. The value
// 0 is reserved to mean "no id" / invalid.

#include <cstdint>

namespace mediamax
{

using Id = std::uint32_t;

inline constexpr Id kInvalidId = 0;

class IdAllocator
{
public:
    IdAllocator() = default;
    explicit IdAllocator(std::uint32_t startValue) : m_Next(startValue) {}

    Id next()
    {
        if (m_Next >= kMaxId)
        {
            m_Next = 1; // wrap around; collisions are extremely unlikely and ids are
                        // only meaningful within a single running instance
        }
        return m_Next++;
    }

    std::uint32_t nextValue() const { return m_Next; }

private:
    static constexpr std::uint32_t kMaxId = 0xFFFFFFFFu;
    std::uint32_t m_Next = 1;
};

} // namespace mediamax
