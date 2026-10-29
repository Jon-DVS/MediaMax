#include "mediamax/util/Base64.h"

namespace mediamax::util
{

namespace
{

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeNibble(char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + 52;
    }
    if (c == '+')
    {
        return 62;
    }
    if (c == '/')
    {
        return 63;
    }
    return -1;
}

} // namespace

std::string base64Encode(const std::uint8_t* data, std::size_t size)
{
    std::string out;
    out.reserve(((size + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= size)
    {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out += kAlphabet[(n >> 18) & 0x3Fu];
        out += kAlphabet[(n >> 12) & 0x3Fu];
        out += kAlphabet[(n >> 6) & 0x3Fu];
        out += kAlphabet[n & 0x3Fu];
        i += 3;
    }

    const std::size_t rem = size - i;
    if (rem == 1)
    {
        const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        out += kAlphabet[(n >> 18) & 0x3Fu];
        out += kAlphabet[(n >> 12) & 0x3Fu];
        out += "==";
    }
    else if (rem == 2)
    {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out += kAlphabet[(n >> 18) & 0x3Fu];
        out += kAlphabet[(n >> 12) & 0x3Fu];
        out += kAlphabet[(n >> 6) & 0x3Fu];
        out += '=';
    }
    return out;
}

bool base64Decode(const std::string& in, std::vector<std::uint8_t>& out)
{
    out.clear();
    if (in.size() % 4 != 0)
    {
        return false;
    }
    // Decode into a scratch buffer so a late failure leaves `out` empty.
    std::vector<std::uint8_t> bytes;
    bytes.reserve(in.size() / 4 * 3);

    for (std::size_t block = 0; block < in.size(); block += 4)
    {
        // Padding is only valid as the final one or two characters of the
        // final block (RFC 4648); data after padding is malformed.
        const bool lastBlock = block + 4 == in.size();
        std::uint32_t n = 0;
        int groups = 0; // 6-bit groups decoded in this block
        bool seenPad = false;
        for (int i = 0; i < 4; ++i)
        {
            const char c = in[block + static_cast<std::size_t>(i)];
            if (c == '=')
            {
                if (!lastBlock || groups < 2)
                {
                    return false;
                }
                seenPad = true;
                continue;
            }
            if (seenPad)
            {
                return false;
            }
            const int v = decodeNibble(c);
            if (v < 0)
            {
                return false;
            }
            n = (n << 6) | static_cast<std::uint32_t>(v);
            ++groups;
        }
        // groups is 2, 3 or 4: the block carries groups-1 bytes, the top
        // (groups*6) bits of n (the remaining low bits are padding and are
        // ignored, matching lenient standard decoders).
        const int bits = groups * 6;
        for (int b = 0; b < groups - 1; ++b)
        {
            bytes.push_back(static_cast<std::uint8_t>((n >> (bits - 8 - 8 * b)) & 0xFFu));
        }
    }
    out = std::move(bytes);
    return true;
}

} // namespace mediamax::util
