#include "mediamax/util/Sha256.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <utility>
#include <vector>

namespace mediamax::util
{

namespace
{

// The FIPS 180-4 round constants: the first 32 bits of the fractional
// parts of the cube roots of the first 64 primes.
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr const char kHex[] = "0123456789abcdef";

inline std::uint32_t rotr(std::uint32_t x, std::uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

// The compression function: folds one 512-bit block into the 256-bit state.
// A free function so hex() can finalize a shadow state without touching
// the running hash.
void compress(std::uint32_t state[8], const std::uint8_t* block)
{
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
    {
        w[i] = (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
               (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i)
    {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i)
    {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + S1 + ch + kK[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

std::string toHex(const std::uint8_t* digest, std::size_t size)
{
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i)
    {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0f]);
    }
    return out;
}

} // namespace

void Sha256::update(const std::uint8_t* data, std::size_t size)
{
    m_HexValid = false;
    m_BitLength += static_cast<std::uint64_t>(size) * 8u;
    while (size > 0)
    {
        const std::size_t space = 64 - m_BlockFill;
        const std::size_t n = size < space ? size : space;
        std::copy_n(data, n, m_Block + m_BlockFill);
        m_BlockFill += n;
        data += n;
        size -= n;
        if (m_BlockFill == 64)
        {
            compress(m_State, m_Block);
            m_BlockFill = 0;
        }
    }
}

std::string Sha256::hex() const
{
    if (m_HexValid)
    {
        return toHex(m_Digest, 32);
    }

    // Finalize a shadow of the state (the live hash keeps running): the
    // partial block, the 0x80 pad byte, zero fill and the 64-bit big-endian
    // bit length, compressed to the end.
    std::uint32_t state[8];
    std::copy_n(m_State, 8, state);
    const std::uint64_t bitLength = m_BitLength;

    std::uint8_t tail[64] = {};
    std::copy_n(m_Block, m_BlockFill, tail);
    std::size_t fill = m_BlockFill;

    tail[fill++] = 0x80;
    if (fill > 56)
    {
        // No room for the 8-byte length in this block: compress it padded,
        // then the length alone in the next block.
        compress(state, tail);
        fill = 0;
    }
    while (fill < 56)
    {
        tail[fill++] = 0;
    }
    for (int i = 7; i >= 0; --i)
    {
        tail[fill++] = static_cast<std::uint8_t>(bitLength >> (i * 8));
    }
    compress(state, tail);

    for (int i = 0; i < 8; ++i)
    {
        m_Digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24);
        m_Digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        m_Digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        m_Digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
    }
    m_HexValid = true;
    return toHex(m_Digest, 32);
}

std::string sha256Hex(const std::uint8_t* data, std::size_t size)
{
    Sha256 hash;
    hash.update(data, size);
    return hash.hex();
}

bool sha256File(const std::filesystem::path& path, std::string& outHex, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "cannot open " + path.string() + " for reading";
        return false;
    }
    // Streamed in bounded blocks: a multi-gigabyte media file is hashed
    // without ever holding more than one block in memory.
    Sha256 hash;
    std::vector<std::uint8_t> block(1u << 20);
    for (;;)
    {
        in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));
        const std::streamsize got = in.gcount();
        if (got > 0)
        {
            hash.update(block.data(), static_cast<std::size_t>(got));
        }
        if (got < static_cast<std::streamsize>(block.size()))
        {
            break; // EOF (or a short read that bad() explains below)
        }
    }
    if (in.bad())
    {
        error = "read failed: " + path.string();
        return false;
    }
    outHex = hash.hex();
    return true;
}

} // namespace mediamax::util
