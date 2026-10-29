// Base64: RFC 4648 vectors, round-trips and malformed-input rejection.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mediamax/util/Base64.h"

using mediamax::util::base64Decode;
using mediamax::util::base64Encode;

TEST_SUITE("Base64")
{

TEST_CASE("known vectors (RFC 4648)")
{
    const std::vector<std::pair<std::string, std::string>> vectors = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (const auto& [plain, encoded] : vectors)
    {
        const std::vector<std::uint8_t> bytes(plain.begin(), plain.end());
        CHECK(base64Encode(bytes) == encoded);
        std::vector<std::uint8_t> decoded;
        CHECK(base64Decode(encoded, decoded));
        CHECK(decoded == bytes);
    }
}

TEST_CASE("round-trips for every small size and a pseudo-random buffer")
{
    // 0..29 bytes covers every padding case (0/1/2 trailing bytes) across
    // many block counts.
    for (std::size_t n = 0; n < 30; ++n)
    {
        std::vector<std::uint8_t> bytes(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            bytes[i] = static_cast<std::uint8_t>(i * 37 + n);
        }
        const std::string encoded = base64Encode(bytes);
        std::vector<std::uint8_t> decoded;
        REQUIRE(base64Decode(encoded, decoded));
        CHECK(decoded == bytes);
    }

    // A larger, denser buffer.
    std::vector<std::uint8_t> bytes(1000);
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = static_cast<std::uint8_t>((i * 131 + (i >> 4) * 17) & 0xFF);
    }
    const std::string encoded = base64Encode(bytes);
    CHECK(encoded.size() == ((bytes.size() + 2) / 3) * 4);
    std::vector<std::uint8_t> decoded;
    REQUIRE(base64Decode(encoded, decoded));
    CHECK(decoded == bytes);
}

TEST_CASE("decoding rejects malformed input")
{
    const std::vector<std::string> bad = {
        "Zm9",            // length not a multiple of 4
        "Zg",             // too short
        "Z!9v",           // character outside the alphabet
        "Zm9vYg=Z",       // data after padding
        "Zm9vYg==Zm9v",   // padding in a non-final block
        "====",           // all padding
        "A===",           // one data group cannot precede two pad chars
        "Zm9vYg= ",       // whitespace is not accepted
    };
    for (const auto& in : bad)
    {
        std::vector<std::uint8_t> out;
        CHECK_FALSE(base64Decode(in, out));
        CHECK(out.empty());
    }
}

TEST_CASE("empty input decodes to empty output")
{
    std::vector<std::uint8_t> out;
    REQUIRE(base64Decode("", out));
    CHECK(out.empty());
}

} // TEST_SUITE("Base64")
