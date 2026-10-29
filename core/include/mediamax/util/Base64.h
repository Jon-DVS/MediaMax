#pragma once

// Base64 (RFC 4648, standard alphabet) encode/decode.
//
// The control protocol is line-oriented JSON, so binary payloads (media
// thumbnails, see media/MediaThumbnail.h and the media.thumbnail command)
// travel as base64 strings.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mediamax::util
{

// Encodes raw bytes as standard base64 ("=" padded).
std::string base64Encode(const std::uint8_t* data, std::size_t size);
inline std::string base64Encode(const std::vector<std::uint8_t>& data)
{
    return data.empty() ? std::string() : base64Encode(data.data(), data.size());
}

// Decodes standard base64 into `out`. Returns false (leaving `out` empty)
// when the input is not valid: a length not divisible by 4, a character
// outside the alphabet, or padding ('=') anywhere but the final one or two
// characters of the final block. No whitespace is accepted.
bool base64Decode(const std::string& in, std::vector<std::uint8_t>& out);

} // namespace mediamax::util
