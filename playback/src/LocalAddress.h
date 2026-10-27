#pragma once

// This machine's primary IPv4 address, for display on the status overlay
// (and, later, in the control GUI's system list).

#include <string>

namespace mediamax
{

// Resolves the local host name to its first IPv4 address (gethostname +
// getaddrinfo). Returns "127.0.0.1" when no IPv4 can be determined.
std::string localIpAddress();

// This machine's computer name (gethostname), for the system info the
// control GUI shows per system (status "hostname"). Empty when it cannot
// be determined.
std::string localHostName();

} // namespace mediamax
