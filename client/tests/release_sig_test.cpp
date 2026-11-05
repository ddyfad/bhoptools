// SHA-256, SHA-512 and ed25519 known answers, SSHSIG checks against OpenSSH's own test signatures
// (regress/unittests/sshsig/testdata), and SHA256SUMS parsing.
#include "release_sig.h"
#include "vendor/monocypher/monocypher-ed25519.h"

#include <cstdio>
#include <string>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string Unhex(const char* hex)
{
	std::string out;
	for (; hex[0] && hex[1]; hex += 2) {
		unsigned v = 0;
		sscanf(hex, "%2x", &v);
		out += static_cast<char>(v);
	}
	return out;
}

static std::string Sha512Hex(const std::string& data)
{
	unsigned char hash[64];
	crypto_sha512(hash, reinterpret_cast<const unsigned char*>(data.data()), data.size());
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for (unsigned char c : hash) {
		out += digits[c >> 4];
		out += digits[c & 15];
	}
	return out;
}

static void TestHashes()
{
	CHECK(relsig::Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(relsig::Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(relsig::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
		== "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(relsig::Sha256Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	CHECK(Sha512Hex("abc") == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
		"2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

// RFC 8032 7.1, tests 1 and 2.
static void TestEd25519()
{
	const std::string pk1 = Unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
	std::string sig1 = Unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
		"5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
	const std::string pk2 = Unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
	const std::string sig2 = Unhex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
		"085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
	const std::string msg2 = Unhex("72");
	auto check = [](const std::string& sig, const std::string& pk, const std::string& msg) {
		return crypto_ed25519_check(reinterpret_cast<const unsigned char*>(sig.data()),
			reinterpret_cast<const unsigned char*>(pk.data()),
			reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 0;
	};
	CHECK(check(sig1, pk1, ""));
	CHECK(check(sig2, pk2, msg2));
	CHECK(!check(sig2, pk1, msg2));
	CHECK(!check(sig1, pk1, "x"));
	sig1[10] ^= 1;
	CHECK(!check(sig1, pk1, ""));
}

static const char kEdKey[] = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIIlizSIN3DQV78VPNjVvvwjgf+P5HqbPYCYu3BOMdjAE ED25519 test key";
static const char kEdSig[] =
	"-----BEGIN SSH SIGNATURE-----\n"
	"U1NIU0lHAAAAAQAAADMAAAALc3NoLWVkMjU1MTkAAAAgiWLNIg3cNBXvxU82NW+/COB/4/\n"
	"keps9gJi7cE4x2MAQAAAAIdW5pdHRlc3QAAAAAAAAABnNoYTUxMgAAAFMAAAALc3NoLWVk\n"
	"MjU1MTkAAABAihQsbUzuNEFflk5Tw1+H9aLS7tZQk0RG8KW1DtOmDYYnWe3D3UKiG3fcJa\n"
	"DNg4vBWp1j1gLRiBMOF+gwYNegDg==\n"
	"-----END SSH SIGNATURE-----\n";
static const char kSkKey[] = "sk-ssh-ed25519@openssh.com AAAAGnNrLXNzaC1lZDI1NTE5QG9wZW5zc2guY29tAAAAIJsaDYXQYruc6bilCYDIK4YSOeG+zmrRO2M9t03//7LHAAAABHNzaDo= ED25519-SK test key";
// flags 0x01 (touched, no PIN), counter 0x12345678
static const char kSkSig[] =
	"-----BEGIN SSH SIGNATURE-----\r\n"
	"U1NIU0lHAAAAAQAAAEoAAAAac2stc3NoLWVkMjU1MTlAb3BlbnNzaC5jb20AAAAgmxoNhd\r\n"
	"Biu5zpuKUJgMgrhhI54b7OatE7Yz23Tf//sscAAAAEc3NoOgAAAAh1bml0dGVzdAAAAAAA\r\n"
	"AAAGc2hhNTEyAAAAZwAAABpzay1zc2gtZWQyNTUxOUBvcGVuc3NoLmNvbQAAAEAi+7eTjW\r\n"
	"/+LQ2M+sCD+KFtH1n7VFFJon/SZFsxODyV8cWTlFKj617Ys1Ur5TV6uaEXQhck8rBA2oQI\r\n"
	"HTPANLIPARI0Vng=\r\n"
	"-----END SSH SIGNATURE-----\r\n";
static const char kData[] = "This is a test, this is only a test";

static void TestSshsig()
{
	const char* const ed[] = { kEdKey };
	const char* const sk[] = { kSkKey };
	const char* const both[] = { kSkKey, kEdKey };
	const unsigned up = relsig::kFlagUserPresent, uv = relsig::kFlagUserVerified;
	std::string err;

	CHECK(relsig::VerifySshsig(kEdSig, kData, "unittest", ed, 1, up | uv, err));
	CHECK(relsig::VerifySshsig(kEdSig, kData, "unittest", both, 2, up | uv, err));
	CHECK(relsig::VerifySshsig(kSkSig, kData, "unittest", sk, 1, up, err));
	CHECK(relsig::VerifySshsig(kSkSig, kData, "unittest", both, 2, up, err));

	CHECK(!relsig::VerifySshsig(kSkSig, kData, "unittest", sk, 1, up | uv, err));
	CHECK(err.find("PIN") != std::string::npos);
	CHECK(!relsig::VerifySshsig(kEdSig, kData, "unittest", sk, 1, up, err));
	CHECK(err.find("release key") != std::string::npos);
	CHECK(!relsig::VerifySshsig(kEdSig, kData, "bhoptools-release", ed, 1, up, err));
	CHECK(err.find("namespace") != std::string::npos);
	CHECK(!relsig::VerifySshsig(kEdSig, std::string(kData) + "\n", "unittest", ed, 1, up, err));
	CHECK(!relsig::VerifySshsig(kSkSig, "This is a test, this is only a tesT", "unittest", sk, 1, up, err));
	CHECK(err == "the signature doesn't match");
	CHECK(!relsig::VerifySshsig("", kData, "unittest", ed, 1, up, err));
	CHECK(!relsig::VerifySshsig(kEdSig, kData, "unittest", ed, 0, up, err));

	// A changed byte in the signature itself.
	std::string bad = kEdSig;
	const size_t at = bad.find("DNg4");
	bad[at] = 'E';
	CHECK(!relsig::VerifySshsig(bad, kData, "unittest", ed, 1, up, err));

	// The real release key is pinned: a good signature from another key is turned away for that.
	std::string hash;
	CHECK(!relsig::CheckSums(kData, kEdSig, "1.13", "bhoptools.exe", hash, err));
	CHECK(err.find("release key") != std::string::npos);
}

static void TestSums()
{
	const std::string exe = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	const std::string so = "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef";
	const std::string sums = "# bhoptools release 1.13\n" + exe + "  bhoptools.exe\n" + so + " *bhoptools.so\r\n";
	std::string hash, err;
	CHECK(relsig::ParseSums(sums, "1.13", "bhoptools.exe", hash, err) && hash == exe);
	CHECK(relsig::ParseSums(sums, "1.13", "bhoptools.so", hash, err) && hash == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
	CHECK(!relsig::ParseSums(sums, "1.14", "bhoptools.exe", hash, err));
	CHECK(!relsig::ParseSums(sums, "1.1", "bhoptools.exe", hash, err));
	CHECK(!relsig::ParseSums(sums, "1.13", "bhoptools", hash, err));
	CHECK(!relsig::ParseSums(sums, "1.13", "bhoptools.ex", hash, err));
	CHECK(!relsig::ParseSums(exe + "  bhoptools.exe\n", "1.13", "bhoptools.exe", hash, err));
	CHECK(!relsig::ParseSums(sums + "# bhoptools release 1.14\n", "1.14", "bhoptools.exe", hash, err));
	CHECK(!relsig::ParseSums(sums + exe + "  bhoptools.exe\n", "1.13", "bhoptools.exe", hash, err));
	std::string badHex = sums;
	badHex[sums.find(exe)] = 'g';
	CHECK(!relsig::ParseSums(badHex, "1.13", "bhoptools.exe", hash, err));
}

int main()
{
	TestHashes();
	TestEd25519();
	TestSshsig();
	TestSums();
	printf("release signatures: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
