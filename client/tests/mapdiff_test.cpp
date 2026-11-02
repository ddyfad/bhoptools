#include "mapdiff.h"

#include <cstdio>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

// As vbsp writes it, NUL included, with an empty block the parser skips.
static const char kLump[] =
	"{\n\"world_maxs\" \"1024 1024 512\"\n\"classname\" \"worldspawn\"\n}\n"
	"{\n\"origin\" \"0 0 64\"\n\"targetname\" \"dest1\"\n\"classname\" \"info_teleport_destination\"\n\"hammerid\" \"10\"\n}\n"
	"{}\n"
	"{\n\"model\" \"*1\"\n\"target\" \"dest1\"\n\"classname\" \"trigger_teleport\"\n\"hammerid\" \"11\"\n}\n"
	"{\n\"model\" \"*2\"\n\"OnStartTouch\" \"relay1,Trigger,,0,-1\"\n\"classname\" \"trigger_multiple\"\n\"hammerid\" \"12\"\n}\n"
	"{\n\"targetname\" \"relay1\"\n\"classname\" \"logic_relay\"\n\"hammerid\" \"13\"\n}\n";

static std::vector<PredMapEntity> Bsp()
{
	std::vector<PredMapEntity> ents;
	PredParseEntityLump(kLump, sizeof(kLump), ents);
	return ents;
}

static int Count(const PredMapEntity& e, const char* key)
{
	int n = 0;
	for (const PredKv& kv : e.kv) n += !strcasecmp(kv.key.c_str(), key);
	return n;
}

static void TestCrc()
{
	CHECK(PredCrc32("123456789", 9) == 0xCBF43926u);
	CHECK(PredCrc32("", 0) == 0u);
}

static void TestApply()
{
	const std::vector<PredMapEntity> bsp = Bsp();
	CHECK(bsp.size() == 5);

	// Stripper took the teleport out, gave the trigger_multiple a second output, and added a
	// destination (no hammerid) and a push after the relay.
	const char diff[] =
		"r 2 11\n"
		"m 3 12 {\"model\" \"*2\" \"OnStartTouch\" \"relay1,Trigger,,0,-1\" \"OnStartTouch\" \"!activator,AddOutput,gravity 0.5,0,-1\" \"classname\" \"trigger_multiple\" \"hammerid\" \"12\"}\n"
		"a 1 {\"origin\" \"128 0 64\" \"targetname\" \"dest2\" \"classname\" \"info_teleport_destination\"}\n"
		"a 4 {\"model\" \"*3\" \"pushdir\" \"0 90 0\" \"speed\" \"500\" \"classname\" \"trigger_push\"}\n";
	std::vector<PredMapDiffEdit> edits;
	CHECK(PredParseMapDiff(diff, sizeof(diff) - 1, edits));
	CHECK(edits.size() == 4);

	std::vector<PredMapEntity> out;
	CHECK(PredApplyMapDiff(bsp, edits, out));
	CHECK(out.size() == 6);
	if (out.size() != 6) return;
	CHECK(!strcmp(out[0].Get("classname"), "worldspawn"));
	CHECK(!strcmp(out[1].Get("targetname"), "dest1"));
	// Added right after the entity it followed on the server, without a hammerid.
	CHECK(!strcmp(out[2].Get("targetname"), "dest2") && !strcmp(out[2].Get("origin"), "128 0 64"));
	CHECK(!out[2].Get("hammerid")[0]);
	CHECK(!strcmp(out[3].Get("classname"), "trigger_multiple") && Count(out[3], "OnStartTouch") == 2);
	CHECK(!strcmp(out[3].kv[2].value.c_str(), "!activator,AddOutput,gravity 0.5,0,-1"));
	CHECK(!strcmp(out[4].Get("classname"), "logic_relay"));
	// The push came after the relay, the last BSP entity. The teleport is gone.
	CHECK(!strcmp(out[5].Get("classname"), "trigger_push") && !strcmp(out[5].Get("pushdir"), "0 90 0"));
	for (const PredMapEntity& e : out) CHECK(strcmp(e.Get("classname"), "trigger_teleport") != 0);

	// -1 goes before the first entity.
	const char first[] = "a -1 {\"classname\" \"filter_activator_name\"}\n";
	CHECK(PredParseMapDiff(first, sizeof(first) - 1, edits) && PredApplyMapDiff(bsp, edits, out));
	CHECK(out.size() == 6 && !strcmp(out[0].Get("classname"), "filter_activator_name"));

	// An empty diff is the BSP.
	CHECK(PredParseMapDiff("", 0, edits) && edits.empty() && PredApplyMapDiff(bsp, edits, out) && out.size() == bsp.size());
}

static void TestReject()
{
	const std::vector<PredMapEntity> bsp = Bsp();
	std::vector<PredMapDiffEdit> edits;
	std::vector<PredMapEntity> out;

	// Made against another lump: the hammerid at that ordinal differs.
	const char wrong[] = "r 2 99\n";
	CHECK(PredParseMapDiff(wrong, sizeof(wrong) - 1, edits) && !PredApplyMapDiff(bsp, edits, out));
	const char twice[] = "r 2 11\nm 2 11 {\"classname\" \"trigger_teleport\"}\n";
	CHECK(PredParseMapDiff(twice, sizeof(twice) - 1, edits) && !PredApplyMapDiff(bsp, edits, out));
	const char range[] = "a 5 {\"classname\" \"trigger_push\"}\n";
	CHECK(PredParseMapDiff(range, sizeof(range) - 1, edits) && !PredApplyMapDiff(bsp, edits, out));
	// Worldspawn has no hammerid, so it goes by 0.
	const char world[] = "m 0 0 {\"classname\" \"worldspawn\"}\n";
	CHECK(PredParseMapDiff(world, sizeof(world) - 1, edits) && PredApplyMapDiff(bsp, edits, out) && out[0].kv.size() == 1);

	CHECK(!PredParseMapDiff("x 1 2\n", 6, edits));
	CHECK(!PredParseMapDiff("r 1\n", 4, edits));
	CHECK(!PredParseMapDiff("m 1 10\n", 7, edits));
	const char two[] = "a 1 {\"classname\" \"a\"}{\"classname\" \"b\"}\n";
	CHECK(!PredParseMapDiff(two, sizeof(two) - 1, edits));
}

int main()
{
	TestCrc();
	TestApply();
	TestReject();
	printf("mapdiff: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
