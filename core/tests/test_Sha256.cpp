// SHA-256: the FIPS 180-4 test vectors, streaming vs one-shot, the running
// digest semantics, and the file hashing used by the media transfer.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "mediamax/util/Sha256.h"

using mediamax::util::Sha256;
using mediamax::util::sha256File;
using mediamax::util::sha256Hex;

namespace
{

// Splits the input into `chunk`-sized pieces and feeds them one at a time:
// the media transfer hashes its running chunk stream exactly this way.
std::string streamingHash(const std::string& in, std::size_t chunk)
{
    Sha256 hash;
    for (std::size_t offset = 0; offset < in.size() || (offset == 0 && in.empty());
         offset += chunk)
    {
        const std::size_t n = in.size() - offset < chunk ? in.size() - offset : chunk;
        hash.update(reinterpret_cast<const std::uint8_t*>(in.data()) + offset, n);
    }
    return hash.hex();
}

} // namespace

TEST_SUITE("Sha256")
{

TEST_CASE("FIPS 180-4 test vectors")
{
    // Empty message.
    CHECK(sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // "abc".
    CHECK(sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Two-block message (56 chars).
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // One million 'a' (the long-message vector: 1,563 blocks).
    CHECK(sha256Hex(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("streaming in chunks matches the one-shot digest")
{
    // Sizes around the 64-byte block boundary exercise the padding paths:
    // pad fits the block (55), the 0x80 byte alone fills it (55), the pad
    // overflows into a fresh block (56, 63-65), and exact multiples.
    for (std::size_t n : {0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1000})
    {
        const std::string in(n, 'x');
        for (std::size_t chunk : {1, 3, 13, 63, 64, 65, 4096})
        {
            CHECK(streamingHash(in, chunk) == sha256Hex(in));
        }
    }
}

TEST_CASE("hex() is stable while idle and follows new data")
{
    Sha256 hash;
    hash.update("abc");
    const std::string first = hash.hex();
    // Idempotent without new data.
    CHECK(hash.hex() == first);
    // A new update continues the running hash (it does not restart).
    hash.update("def");
    CHECK(hash.hex() == sha256Hex("abcdef"));
}

TEST_CASE("sha256File streams the file content")
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "mediamax-sha256-test.bin";
    // 3 blocks and change, so the streaming loop takes several passes.
    std::string content(200000, 'M');
    content += "media tail";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << content;
    }

    std::string hex, error;
    REQUIRE(sha256File(file, hex, error));
    CHECK(error.empty());
    CHECK(hex == sha256Hex(content));

    // A missing file is a clean failure.
    std::string missing;
    CHECK_FALSE(sha256File(file / "nope", missing, error));
    CHECK_FALSE(error.empty());

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST_CASE("sha256File hashes an empty file like the empty message")
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "mediamax-sha256-empty.bin";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
    }
    std::string hex, error;
    REQUIRE(sha256File(file, hex, error));
    CHECK(hex == sha256Hex(""));
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

} // TEST_SUITE("Sha256")
