#pragma once

// SHA-256 (FIPS 180-4), implemented in the core so the media distribution
// feature (docs/PROTOCOL.md media.put and the show file's media checksums)
// can verify transferred files without any platform crypto dependency.
//
// The digest is reported as lowercase hex (64 characters) - the form the
// protocol and the show file carry.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace mediamax::util
{

// Incremental SHA-256: feed the data in any number of `update` calls (the
// media transfer feeds one network chunk at a time, the file hashing runs
// in bounded blocks), then take the final digest with `hex()`. Cheap to
// copy/move (plain state).
class Sha256
{
public:
    // Feeds `size` bytes into the running digest.
    void update(const std::uint8_t* data, std::size_t size);
    void update(const std::string& data)
    {
        update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    }

    // Returns the 64-character lowercase hex digest of everything fed so
    // far. The digest is computed over a shadow of the state, so the hash
    // keeps running (later updates + hex() recompute); repeated calls
    // without new data return the same string.
    std::string hex() const;

private:
    void transform(const std::uint8_t* block);

    std::uint32_t m_State[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint64_t m_BitLength = 0;
    std::uint8_t m_Block[64] = {};
    std::size_t m_BlockFill = 0;
    mutable std::uint8_t m_Digest[32] = {};
    mutable bool m_HexValid = false;
};

// One-shot SHA-256: the 64-character lowercase hex digest of the data.
std::string sha256Hex(const std::uint8_t* data, std::size_t size);
inline std::string sha256Hex(const std::string& data)
{
    return sha256Hex(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

// The SHA-256 of a whole file, streamed in bounded blocks (media files can
// be multi-gigabyte; nothing is read into memory at once). Returns false
// with `error` when the file cannot be read.
bool sha256File(const std::filesystem::path& path, std::string& outHex, std::string& error);

} // namespace mediamax::util
