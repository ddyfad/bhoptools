// Signed releases: each public release carries SHA256SUMS, signed with `ssh-keygen -Y sign` by a
// pinned release key (SHA256SUMS.sig). The updater installs nothing the signed file doesn't list.
// See STAGING.md, "Promoting".
#pragma once

#include <cstddef>
#include <string>

#define RELEASE_SIG_NAMESPACE "bhoptools-release"
// What SHA256SUMS starts with, followed by the release's tag.
#define RELEASE_SUMS_HEADER "# bhoptools release "

namespace relsig {

// SSHSIG flags from the security key: user present, user verified (PIN).
const unsigned kFlagUserPresent = 0x01;
const unsigned kFlagUserVerified = 0x04;

// Lower-case hex.
std::string Sha256Hex(const std::string& data);

// Checks an armored SSHSIG (`ssh-keygen -Y sign`) over `message` in namespace `ns`, made by one of
// `keys` (public key lines, "type base64 [comment]"). A security key's signature must have every
// bit of `skFlags` set. False with the reason in `err` otherwise.
bool VerifySshsig(const std::string& armored, const std::string& message, const char* ns,
	const char* const* keys, size_t keyCount, unsigned skFlags, std::string& err);

// Reads a SHA256SUMS that's already been checked: it must be the one for `tag`, and `hash` gets
// the sha256 it lists for `asset`.
bool ParseSums(const std::string& sums, const std::string& tag, const char* asset, std::string& hash, std::string& err);

// Checks SHA256SUMS against SHA256SUMS.sig and the pinned release keys, then ParseSums.
bool CheckSums(const std::string& sums, const std::string& sig, const std::string& tag,
	const char* asset, std::string& hash, std::string& err);

}
