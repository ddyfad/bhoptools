// CShaderSystem::LoadTexture drops a stray leading slash from a VMT's texture name, but its test is
// pName[0] == '/' || pName[1] == '/', so "$basetexture" "z/lightpost" loads as "/lightpost" and the
// texture is looked for at materials//lightpost.vtf. Pointing the second test back at pName[0]
// leaves only the leading-slash case.

#include <cstdint>
#include <cstring>
#include <sys/mman.h>

#include "utils.h"
#include "sigs.h"
#include "singleletter_fix.h"

#define MATERIALSYSTEM_MODULE "materialsystem.so"

namespace singleletter {

void Install()
{
	const std::vector<ModuleSegment> text = FindExecSegments(MATERIALSYSTEM_MODULE);
	uint8_t* site = (uint8_t*)FindPatternIn(MATERIALSYSTEM_MODULE, SIG_ShaderSystem_LoadTexture_SlashSkip);
	if (!site) {
		CoreLog("single-letter folders: texture name check not found, not installing");
		return;
	}
	int32_t rel1, rel2;
	memcpy(&rel1, site + OFF_SlashSkip_Je1 + 2, 4);
	memcpy(&rel2, site + OFF_SlashSkip_Je2 + 2, 4);
	const uint8_t* skip = site + OFF_SlashSkip_Je1 + 6 + rel1;
	const ParsedSig inc = ParseSig(SIG_SlashSkip_Increment);
	bool inText = false;
	for (const auto& s : text) inText |= (uintptr_t)skip >= s.start && (uintptr_t)skip + inc.bytes.size() <= s.end;
	if (skip != site + OFF_SlashSkip_Je2 + 6 + rel2 || !inText || memcmp(skip, inc.bytes.data(), inc.bytes.size()) != 0) {
		CoreLog("single-letter folders: texture name check isn't the build this was written for, not installing");
		return;
	}

	uint8_t* index = site + OFF_SlashSkip_Index;
	const int prot = PageProt((uintptr_t)index);
	if (!mprotect_range((uintptr_t)index, 1, PROT_READ | PROT_WRITE | PROT_EXEC)) {
		CoreLog("single-letter folders: couldn't patch materialsystem, not installing");
		return;
	}
	*index = 0;
	__builtin___clear_cache((char*)index, (char*)index + 1);
	mprotect_range((uintptr_t)index, 1, prot);
	CoreLog("single-letter folders: textures under one-character folders keep the folder");
}

}
