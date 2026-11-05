#include "release_sig.h"

#include <cstdint>
#include <cstring>
#include "vendor/monocypher/monocypher-ed25519.h"
#include "vendor/sha256/sha256.h"

namespace relsig {

// The release keys: SHA256SUMS signed by any of them is accepted. Keep this list and
// .github/release_signers (what promote checks the signature with) the same.
static const char* const kReleaseKeys[] = {
	// The owner's YubiKey (FIDO2, made with -O application=ssh:bhoptools-release -O verify-required).
	"sk-ssh-ed25519@openssh.com AAAAGnNrLXNzaC1lZDI1NTE5QG9wZW5zc2guY29tAAAAIO5VVrqS8SoOXuUdbUgw3pGQlXmAt2EFaS7sSQr2k4CDAAAAFXNzaDpiaG9wdG9vbHMtcmVsZWFzZQ== bhoptools-release",
	// Temporary software backup (passphrase-protected, kept offline); replace with the backup YubiKey's key.
	"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAbUi/sEB4HW1rXUrQ78ugg4MIbsvTbTI4dTnlN8UflB bhoptools-release-backup",
};

namespace {

typedef unsigned char u8;

std::string Hex(const u8* p, size_t n)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < n; i++) {
		out += digits[p[i] >> 4];
		out += digits[p[i] & 15];
	}
	return out;
}

std::string Sha256(const std::string& data)
{
	SHA256_CTX ctx;
	BYTE hash[SHA256_BLOCK_SIZE];
	sha256_init(&ctx);
	sha256_update(&ctx, reinterpret_cast<const BYTE*>(data.data()), data.size());
	sha256_final(&ctx, hash);
	return std::string(reinterpret_cast<const char*>(hash), sizeof(hash));
}

std::string Sha512(const std::string& data)
{
	u8 hash[64];
	crypto_sha512(hash, reinterpret_cast<const u8*>(data.data()), data.size());
	return std::string(reinterpret_cast<const char*>(hash), sizeof(hash));
}

// Whitespace is skipped; anything else that isn't base64 fails.
bool Base64(const std::string& in, std::string& out)
{
	out.clear();
	unsigned acc = 0;
	int bits = 0;
	bool padded = false;
	for (const char ch : in) {
		const unsigned char c = static_cast<unsigned char>(ch);
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
		if (c == '=') { padded = true; continue; }
		if (padded) return false;
		unsigned v;
		if (c >= 'A' && c <= 'Z') v = c - 'A';
		else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
		else if (c >= '0' && c <= '9') v = c - '0' + 52;
		else if (c == '+') v = 62;
		else if (c == '/') v = 63;
		else return false;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += static_cast<char>((acc >> bits) & 0xff);
		}
	}
	return true;
}

// The SSH wire format: big-endian uint32, and strings with a uint32 length in front.
struct Reader {
	const u8* p;
	size_t n;

	explicit Reader(const std::string& s) : p(reinterpret_cast<const u8*>(s.data())), n(s.size()) {}

	bool U8(u8& v)
	{
		if (n < 1) return false;
		v = *p++;
		n--;
		return true;
	}

	bool U32(uint32_t& v)
	{
		if (n < 4) return false;
		v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
		p += 4;
		n -= 4;
		return true;
	}

	bool Bytes(size_t len, std::string& s)
	{
		if (len > n) return false;
		s.assign(reinterpret_cast<const char*>(p), len);
		p += len;
		n -= len;
		return true;
	}

	bool String(std::string& s)
	{
		uint32_t len;
		return U32(len) && Bytes(len, s);
	}
};

void PutU32(std::string& out, uint32_t v)
{
	out += static_cast<char>(v >> 24);
	out += static_cast<char>(v >> 16);
	out += static_cast<char>(v >> 8);
	out += static_cast<char>(v);
}

void PutString(std::string& out, const std::string& s)
{
	PutU32(out, static_cast<uint32_t>(s.size()));
	out += s;
}

const char kEd25519[] = "ssh-ed25519";
const char kSkEd25519[] = "sk-ssh-ed25519@openssh.com";

// The key blob from a public key line ("type base64 comment").
bool KeyBlob(const char* line, std::string& blob)
{
	const char* space = strchr(line, ' ');
	if (!space) return false;
	const char* end = strchr(space + 1, ' ');
	const std::string b64 = end ? std::string(space + 1, end) : std::string(space + 1);
	return Base64(b64, blob);
}

}

std::string Sha256Hex(const std::string& data)
{
	const std::string hash = Sha256(data);
	return Hex(reinterpret_cast<const u8*>(hash.data()), hash.size());
}

bool VerifySshsig(const std::string& armored, const std::string& message, const char* ns,
	const char* const* keys, size_t keyCount, unsigned skFlags, std::string& err)
{
	static const char kBegin[] = "-----BEGIN SSH SIGNATURE-----";
	static const char kEnd[] = "-----END SSH SIGNATURE-----";
	const size_t begin = armored.find(kBegin);
	const size_t end = begin == std::string::npos ? begin : armored.find(kEnd, begin);
	std::string blob;
	if (end == std::string::npos
			|| !Base64(armored.substr(begin + sizeof(kBegin) - 1, end - begin - (sizeof(kBegin) - 1)), blob)) {
		err = "the signature isn't an SSH signature";
		return false;
	}

	// PROTOCOL.sshsig
	Reader r(blob);
	std::string magic, publicKey, sigNamespace, reserved, hashAlg, signature;
	uint32_t version;
	if (!r.Bytes(6, magic) || magic != "SSHSIG" || !r.U32(version) || version != 1
			|| !r.String(publicKey) || !r.String(sigNamespace) || !r.String(reserved)
			|| !r.String(hashAlg) || !r.String(signature) || r.n != 0) {
		err = "the signature is malformed";
		return false;
	}
	bool pinned = false;
	for (size_t i = 0; i < keyCount && !pinned; i++) {
		std::string key;
		pinned = KeyBlob(keys[i], key) && key == publicKey;
	}
	if (!pinned) {
		err = "the signature isn't from a release key";
		return false;
	}
	if (sigNamespace != ns) {
		err = "the signature is for namespace \"" + sigNamespace + "\", not \"" + ns + "\"";
		return false;
	}
	std::string messageHash;
	if (hashAlg == "sha512") messageHash = Sha512(message);
	else if (hashAlg == "sha256") messageHash = Sha256(message);
	else {
		err = "the signature uses hash \"" + hashAlg + "\"";
		return false;
	}

	Reader k(publicKey);
	std::string keyType, pk, application;
	if (!k.String(keyType) || !k.String(pk) || pk.size() != 32
			|| (keyType == kSkEd25519 && !k.String(application))
			|| (keyType != kEd25519 && keyType != kSkEd25519) || k.n != 0) {
		err = "the release key isn't an ed25519 key";
		return false;
	}

	// The reserved field signed is always empty, whatever the signature carries.
	std::string toSign = "SSHSIG";
	PutString(toSign, ns);
	PutString(toSign, "");
	PutString(toSign, hashAlg);
	PutString(toSign, messageHash);

	Reader s(signature);
	std::string sigType, ed;
	if (!s.String(sigType) || sigType != keyType || !s.String(ed) || ed.size() != 64) {
		err = "the signature is malformed";
		return false;
	}
	std::string signedData;
	if (keyType == kEd25519) {
		if (s.n != 0) {
			err = "the signature is malformed";
			return false;
		}
		signedData = toSign;
	} else {
		// PROTOCOL.u2f: the authenticator signs sha256(application) || flags || counter || sha256(data).
		u8 flags;
		uint32_t counter;
		if (!s.U8(flags) || !s.U32(counter) || s.n != 0) {
			err = "the signature is malformed";
			return false;
		}
		if ((flags & skFlags) != skFlags) {
			err = (skFlags & kFlagUserVerified) && !(flags & kFlagUserVerified)
				? "the security key didn't verify its PIN for the signature"
				: "the security key wasn't touched for the signature";
			return false;
		}
		signedData = Sha256(application);
		signedData += static_cast<char>(flags);
		PutU32(signedData, counter);
		signedData += Sha256(toSign);
	}
	if (crypto_ed25519_check(reinterpret_cast<const u8*>(ed.data()), reinterpret_cast<const u8*>(pk.data()),
			reinterpret_cast<const u8*>(signedData.data()), signedData.size()) != 0) {
		err = "the signature doesn't match";
		return false;
	}
	return true;
}

// The signed file names its release, so an older release's SHA256SUMS can't be put on a new tag.
bool ParseSums(const std::string& sums, const std::string& tag, const char* asset, std::string& hash, std::string& err)
{
	std::string version;
	bool versionSeen = false;
	hash.clear();
	for (size_t at = 0; at < sums.size();) {
		size_t eol = sums.find('\n', at);
		if (eol == std::string::npos) eol = sums.size();
		std::string line = sums.substr(at, eol - at);
		at = eol + 1;
		if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
		if (line.compare(0, sizeof(RELEASE_SUMS_HEADER) - 1, RELEASE_SUMS_HEADER) == 0) {
			if (versionSeen) {
				err = "SHA256SUMS names more than one release";
				return false;
			}
			versionSeen = true;
			version = line.substr(sizeof(RELEASE_SUMS_HEADER) - 1);
			continue;
		}
		// sha256sum's "<hash>  <name>", or "<hash> *<name>" in binary mode.
		if (line.size() > 66 && line[64] == ' ' && (line[65] == ' ' || line[65] == '*')
				&& line.compare(66, std::string::npos, asset) == 0) {
			if (!hash.empty()) {
				err = std::string("SHA256SUMS lists ") + asset + " twice";
				return false;
			}
			hash = line.substr(0, 64);
			for (char& c : hash)
				if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
			if (hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
				err = std::string("SHA256SUMS has a bad hash for ") + asset;
				return false;
			}
		}
	}
	if (!versionSeen || version != tag) {
		err = "SHA256SUMS is for release \"" + version + "\", not " + tag;
		return false;
	}
	if (hash.empty()) {
		err = std::string("SHA256SUMS doesn't list ") + asset;
		return false;
	}
	return true;
}

bool CheckSums(const std::string& sums, const std::string& sig, const std::string& tag,
	const char* asset, std::string& hash, std::string& err)
{
	if (!VerifySshsig(sig, sums, RELEASE_SIG_NAMESPACE, kReleaseKeys, sizeof(kReleaseKeys) / sizeof(kReleaseKeys[0]),
			kFlagUserPresent | kFlagUserVerified, err)) {
		err = "SHA256SUMS.sig: " + err;
		return false;
	}
	return ParseSums(sums, tag, asset, hash, err);
}

}
