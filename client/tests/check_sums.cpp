// check_sums <SHA256SUMS> <SHA256SUMS.sig> <tag> <file>...
// What the client checks before it installs a release, run by the workflows before they publish
// one: the signature against the pinned release keys (PIN flag included), the tag, and each file.
#include "release_sig.h"

#include <cstdio>
#include <cstring>
#include <string>

static bool Read(const char* path, std::string& data)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	char chunk[4096];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) data.append(chunk, n);
	return fclose(f) == 0;
}

int main(int argc, char** argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: check_sums <SHA256SUMS> <SHA256SUMS.sig> <tag> <file>...\n");
		return 2;
	}
	std::string sums, sig;
	if (!Read(argv[1], sums) || !Read(argv[2], sig)) {
		fprintf(stderr, "can't read %s or %s\n", argv[1], argv[2]);
		return 1;
	}
	int bad = 0;
	for (int i = 4; i < argc; i++) {
		const char* slash = strrchr(argv[i], '/');
		const char* name = slash ? slash + 1 : argv[i];
		std::string hash, err, data;
		if (!relsig::CheckSums(sums, sig, argv[3], name, hash, err)) {
			fprintf(stderr, "%s: %s\n", name, err.c_str());
			bad++;
		} else if (!Read(argv[i], data) || relsig::Sha256Hex(data) != hash) {
			fprintf(stderr, "%s: not the file SHA256SUMS lists\n", name);
			bad++;
		} else {
			printf("%s: OK\n", name);
		}
	}
	return bad ? 1 : 0;
}
