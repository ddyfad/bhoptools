#define _USE_MATH_DEFINES
#include "prediction.h"

#include <cstdio>
#include <cmath>

static int g_fail = 0, g_pass = 0;

#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) do { const double a_ = (a), b_ = (b); if (fabs(a_ - b_) <= (eps)) ++g_pass; else { ++g_fail; \
	printf("FAIL %s:%d: %s = %.9g, expected %.9g\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

// As shipped in bhop_ceaseless's pakfile.
static const char* const kSpeedReset =
	"function SpeedReset()\r\n{\r\n    if (!activator || !activator.IsValid())\r\n        return;\r\n\r\n"
	"    activator.SetAbsVelocity(Vector(0, 0, 0));\r\n}";
static const char* const kBooster =
	"function Boost(amount)\r\n{\r\n    if (!activator || !activator.IsValid())\r\n        return;\r\n\r\n"
	"    local vel = activator.GetAbsVelocity();\r\n    local dir = Vector(vel.x, vel.y, 0);\r\n\r\n"
	"    if (dir.Length() <= 0.01)\r\n        return;\r\n\r\n    dir.Norm();\r\n\r\n"
	"    activator.SetAbsVelocity(Vector(\r\n        vel.x + dir.x * amount,\r\n        vel.y + dir.y * amount,\r\n"
	"        vel.z\r\n    ));\r\n}";

static vs::Player MakePlayer(float vx, float vy, float vz)
{
	vs::Player p;
	p.vel[0] = vx; p.vel[1] = vy; p.vel[2] = vz;
	p.origin[0] = 10; p.origin[1] = 20; p.origin[2] = 30;
	p.baseVel[0] = p.baseVel[1] = p.baseVel[2] = 0;
	p.gravity = 1.0f;
	return p;
}

struct Scope {
	vs::Program prog;
	vs::Scratch sc;
	explicit Scope(const char* src) { if (src) vs::CompileFile(prog, src, strlen(src)); sc.Reserve(); }
	int Snip(const char* code) { return vs::CompileSnippet(prog, code); }
	void Link() { vs::Link(prog); }
	bool Supported(const char* name) const { const int f = prog.Find(name); return f >= 0 && !prog.funcs[f].unsupported; }
	int Run(int fn, vs::Player& p) { return vs::Run(prog, fn, p, sc); }
};

// Boost(amount) by hand, in float32, with Vector::NormalizeInPlace's rsqrt.
static void BoostReference(const float in[3], float amount, float out[3])
{
	vs::Vec3 dir = { in[0], in[1], 0.0f };
	vs::NormalizeInPlace(dir);
	out[0] = in[0] + dir.x * amount;
	out[1] = in[1] + dir.y * amount;
	out[2] = in[2];
}

static void TestInterpreter()
{
	Scope s(kSpeedReset);
	vs::CompileFile(s.prog, kBooster, strlen(kBooster));
	const int boost = s.Snip("Boost(500)");
	const int reset = s.Snip("SpeedReset()");
	const int call = vs::CompileCall(s.prog, "SpeedReset");
	s.Link();
	CHECK(s.Supported("Boost"));
	CHECK(s.Supported("SpeedReset"));
	CHECK(boost >= 0 && !s.prog.funcs[boost].unsupported);
	CHECK(reset >= 0 && !s.prog.funcs[reset].unsupported);
	CHECK(call >= 0 && !s.prog.funcs[call].unsupported);

	{
		vs::Player p = MakePlayer(300, 400, 50);
		CHECK(s.Run(boost, p) == vs::R_OK);
		CHECK_NEAR(p.vel[0], 600, 1e-4);
		CHECK_NEAR(p.vel[1], 800, 1e-4);
		CHECK(p.vel[2] == 50.0f);
		float ref[3];
		const float in[3] = { 300, 400, 50 };
		BoostReference(in, 500, ref);
		CHECK(p.vel[0] == ref[0] && p.vel[1] == ref[1]);
		CHECK(p.origin[0] == 10 && p.gravity == 1.0f);
	}
	{
		// Not a round number: the float32 steps show.
		vs::Player p = MakePlayer(-123.25f, 77.5f, -3);
		CHECK(s.Run(boost, p) == vs::R_OK);
		float ref[3];
		const float in[3] = { -123.25f, 77.5f, -3 };
		BoostReference(in, 500, ref);
		CHECK(p.vel[0] == ref[0] && p.vel[1] == ref[1] && p.vel[2] == -3.0f);
		CHECK_NEAR(sqrt((double)p.vel[0] * p.vel[0] + (double)p.vel[1] * p.vel[1]), sqrt(123.25 * 123.25 + 77.5 * 77.5) + 500, 1e-3);
	}
	{
		vs::Player p = MakePlayer(0.001f, 0, 300);
		CHECK(s.Run(boost, p) == vs::R_OK);
		CHECK(p.vel[0] == 0.001f && p.vel[1] == 0 && p.vel[2] == 300);
	}
	{
		vs::Player p = MakePlayer(300, 400, 50);
		CHECK(s.Run(reset, p) == vs::R_OK);
		CHECK(p.vel[0] == 0 && p.vel[1] == 0 && p.vel[2] == 0);
		p = MakePlayer(1, 2, 3);
		CHECK(s.Run(call, p) == vs::R_OK);
		CHECK(p.vel[0] == 0 && p.vel[1] == 0 && p.vel[2] == 0);
	}
	{
		const int bad = s.Snip("Boost()");
		s.Link();
		vs::Player p = MakePlayer(300, 400, 50);
		CHECK(bad >= 0 && s.Run(bad, p) == vs::R_ERROR);
		CHECK(p.vel[0] == 300);
	}
}

static void TestSemantics()
{
	Scope s(
		"function Ints() { local a = 7 / 2; local b = 7.0 / 2; local c = -7 % 3\n"
		"  activator.SetAbsVelocity(Vector(a, b, 1 + 2 * 3 - c)) }\n"
		"function Floats() { local x = 0.1; local y = x * 3; activator.SetAbsVelocity(Vector(y, 16777217, 0.1 + 0.2)) }\n"
		"function NormLen() { local v = Vector(3, 4, 0); local l = v.Norm(); activator.SetAbsVelocity(Vector(l, v.x, v.y)) }\n"
		"function Refs() { local a = Vector(1, 2, 3); local b = a; b.x = 10; a.z += 5; activator.SetAbsVelocity(a) }\n"
		"function Loops() { local s = 0\n for (local i = 0; i < 10; i++) s += i\n local j = 0\n"
		"  while (true) { j++; if (j > 4) break; else continue; }\n activator.SetAbsVelocity(Vector(s, j, 0)) }\n"
		"function Helper(v, k) { return v * k }\n"
		"function Calls() { activator.SetAbsVelocity(Helper(activator.GetAbsVelocity(), 2) - Vector(1, 1, 1)) }\n"
		"function Move() { activator.SetOrigin(activator.GetOrigin() + Vector(0, 0, 64)); activator.SetGravity(activator.GetGravity() * 0.5) }\n"
		"function Err() { activator.SetAbsVelocity(Vector(1, 2, 3)); local x = 1 / 0; activator.SetAbsVelocity(Vector(9, 9, 9)) }\n"
		"function Spin() { while (true) {} }\n"
		"function Rec() { Rec() }\n"
		"function Ret() { return\n activator.SetAbsVelocity(Vector(5, 5, 5)) }\n"
		"function Misc() { local a = null; local b = a || 4; local c = !a && b == 4.0 ? 1.5 : 2\n"
		"  local d = Vector(1, 0, 0).Cross(Vector(0, 1, 0)); local e = Vector(1, 2, 3).Dot(Vector(4, 5, 6))\n"
		"  printl(\"misc \" + c)\n activator.SetAbsVelocity(Vector(c, d.z, e)) }\n"
		"function Div() { activator.SetAbsVelocity(activator.GetAbsVelocity() / 3) }\n");
	const int ints = s.Snip("Ints()"), floats = s.Snip("Floats()"), norm = s.Snip("NormLen()"), refs = s.Snip("Refs()");
	const int loops = s.Snip("Loops()"), calls = s.Snip("Calls();"), move = s.Snip("Move()"), err = s.Snip("Err()");
	const int spin = s.Snip("Spin()"), rec = s.Snip("Rec()"), ret = s.Snip("Ret()"), misc = s.Snip("Misc()");
	const int div = s.Snip("Div()"), inl = s.Snip("activator.SetAbsVelocity(Vector(-1, -2.5, 1e2))");
	s.Link();
	for (const char* f : { "Ints", "Floats", "NormLen", "Refs", "Loops", "Helper", "Calls", "Move", "Err", "Spin", "Rec", "Ret", "Misc", "Div" }) {
		if (!s.Supported(f)) printf("  %s unsupported: %s\n", f, s.prog.funcs[s.prog.Find(f)].why.c_str());
		CHECK(s.Supported(f));
	}

	vs::Player p = MakePlayer(0, 0, 0);
	CHECK(s.Run(ints, p) == vs::R_OK);
	CHECK(p.vel[0] == 3.0f && p.vel[1] == 3.5f && p.vel[2] == 8.0f); // -7 % 3 == -1

	p = MakePlayer(0, 0, 0);
	CHECK(s.Run(floats, p) == vs::R_OK);
	CHECK(p.vel[0] == 0.1f * 3.0f);
	CHECK(p.vel[1] == 16777216.0f);
	CHECK(p.vel[2] == 0.1f + 0.2f);
	CHECK(p.vel[2] != (float)(0.1 + 0.2) || 0.1f + 0.2f == (float)(0.1 + 0.2));

	p = MakePlayer(0, 0, 0);
	CHECK(s.Run(norm, p) == vs::R_OK);
	CHECK_NEAR(p.vel[0], 5, 1e-5);
	CHECK_NEAR(p.vel[1], 0.6, 1e-6);
	CHECK_NEAR(p.vel[2], 0.8, 1e-6);

	p = MakePlayer(0, 0, 0);
	CHECK(s.Run(refs, p) == vs::R_OK);
	CHECK(p.vel[0] == 10 && p.vel[1] == 2 && p.vel[2] == 8);

	p = MakePlayer(0, 0, 0);
	CHECK(s.Run(loops, p) == vs::R_OK);
	CHECK(p.vel[0] == 45 && p.vel[1] == 5);

	p = MakePlayer(10, 20, 30);
	CHECK(s.Run(calls, p) == vs::R_OK);
	CHECK(p.vel[0] == 19 && p.vel[1] == 39 && p.vel[2] == 59);

	p = MakePlayer(0, 0, 0);
	CHECK(s.Run(move, p) == vs::R_OK);
	CHECK(p.origin[0] == 10 && p.origin[1] == 20 && p.origin[2] == 94 && p.gravity == 0.5f);

	// A script error stops the call; what ran before it stays, as in the game.
	p = MakePlayer(0, 0, 0);
	CHECK(s.Run(err, p) == vs::R_ERROR);
	CHECK(p.vel[0] == 1 && p.vel[1] == 2 && p.vel[2] == 3);

	// Bounded: an endless loop or recursion is given up on, not predicted.
	p = MakePlayer(7, 7, 7);
	CHECK(s.Run(spin, p) == vs::R_UNMODELED);
	CHECK(s.Run(rec, p) == vs::R_UNMODELED);

	p = MakePlayer(7, 7, 7);
	CHECK(s.Run(ret, p) == vs::R_OK);
	CHECK(p.vel[0] == 7); // `return` then a newline returns null

	p = MakePlayer(0, 0, 0);
	CHECK(s.Run(misc, p) == vs::R_UNMODELED); // string concatenation isn't modeled

	p = MakePlayer(3, 6, 9);
	CHECK(s.Run(div, p) == vs::R_OK);
	CHECK(p.vel[0] == 3 * (1.0f / 3) && p.vel[1] == 6 * (1.0f / 3));

	p = MakePlayer(0, 0, 0);
	CHECK(inl >= 0 && s.Run(inl, p) == vs::R_OK);
	CHECK(p.vel[0] == -1 && p.vel[1] == -2.5f && p.vel[2] == 100);
}

static void TestUnsupported()
{
	Scope s(
		"function A() { local e = Entities.FindByClassname(null, \"player\") }\n"
		"function B() { ::x <- 1 }\n"
		"function C() { EntFire(\"a\", \"b\") }\n"
		"function D() { local t = [1, 2] }\n"
		"function E() { self.GetOrigin() }\n"
		"function F() { A() }\n"
		"function G() { local a = 1 local b = 2 }\n"
		"function H(a, b = 2) { }\n"
		"function I() { activator.SetAbsVelocity(activator.GetAbsVelocity() * activator.GetAbsVelocity()) }\n"
		"function J() { foreach (k in activator) {} }\n"
		"function K() { local v = activator.GetForwardVector() }\n"
		"function Ok() { activator.SetGravity(2) }\n"
		"function Redef() { }\n"
		"Redef <- function() { activator.SetGravity(9) }\n"
		"local unused = 5\n"
		"function Later() { Ok() }\n");
	const int snipBad = s.Snip("x <- Boost(1)");
	const int snipGlobal = s.Snip("Ok(); caller.Kill()");
	s.Link();
	for (const char* f : { "A", "B", "C", "D", "E", "F", "G", "H", "J", "K", "Redef" }) {
		const int fi = s.prog.Find(f);
		CHECK(fi >= 0);
		if (fi >= 0) CHECK(s.prog.funcs[fi].unsupported && !s.prog.funcs[fi].why.empty());
	}
	CHECK(s.Supported("I")); // parses; Vector * Vector is just not modeled when run
	vs::Player p = MakePlayer(1, 2, 3);
	CHECK(s.Run(vs::CompileCall(s.prog, "I"), p) == vs::R_UNMODELED); // not linked: refused before running
	CHECK(s.Supported("Ok"));
	CHECK(s.Supported("Later"));
	CHECK(snipBad < 0 && snipGlobal < 0);
	CHECK(s.prog.Tainted("x")); // what the failed snippet assigns

	// A file that pulls in more code can't be trusted at all.
	Scope inc("IncludeScript(\"lib\")\nfunction Ok() { activator.SetGravity(2) }\n");
	inc.Link();
	CHECK(!inc.Supported("Ok"));
	// Nor can one that redefines a builtin.
	Scope vec("Vector <- function(a, b, c) { }\nfunction Ok() { activator.SetGravity(2) }\n");
	vec.Link();
	CHECK(!vec.Supported("Ok"));
	// Something we can't lex: the whole file may fail to compile in the game.
	Scope lex("function Ok() { activator.SetGravity(2) }\nlocal s = \"\\q\"\n");
	lex.Link();
	CHECK(!lex.Supported("Ok"));
}

static uint32_t g_rng = 12345;
static uint32_t Rand() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

static void TestHostile()
{
	// Deep nesting: refused, not a stack overflow.
	std::string deep = "function D() { local a = ";
	for (int i = 0; i < 20000; ++i) deep += "(";
	deep += "1";
	for (int i = 0; i < 20000; ++i) deep += ")";
	deep += " }\nfunction Ok() { }\n";
	{
		Scope s(deep.c_str());
		s.Link();
		CHECK(!s.Supported("D") && s.Supported("Ok"));
	}
	std::string blocks = "function D() { ";
	for (int i = 0; i < 5000; ++i) blocks += "if (1) { ";
	for (int i = 0; i < 5000; ++i) blocks += "} ";
	blocks += "}";
	{
		Scope s(blocks.c_str());
		s.Link();
		CHECK(!s.Supported("D"));
	}
	// A long flat chain parses into a deep tree: running it is bounded too.
	std::string chain = "function L() { local a = 1";
	for (int i = 0; i < 20000; ++i) chain += " + 1";
	chain += "\n activator.SetGravity(a) }";
	{
		Scope s(chain.c_str());
		const int fn = s.Snip("L()");
		s.Link();
		vs::Player p = MakePlayer(0, 0, 0);
		if (fn >= 0 && !s.prog.funcs[fn].unsupported) CHECK(s.Run(fn, p) == vs::R_UNMODELED);
		CHECK(p.gravity == 1.0f);
	}
	// Garbage and mutations of the real scripts.
	const char* alphabet = "(){}[];,.:?+-*/%=<>!&|~\"'@#\n\t abcfxyzlocalfunctionreturnifelsewhileforVectoractivator0123456789.e";
	const size_t an = strlen(alphabet);
	int ran = 0;
	for (int iter = 0; iter < 3000; ++iter) {
		std::string src = iter & 1 ? kBooster : kSpeedReset;
		const int edits = iter % 3 ? 1 : 1 + (int)(Rand() % 8);
		for (int k = 0; k < edits && !src.empty(); ++k) {
			const size_t at = Rand() % src.size();
			switch (Rand() % 3) {
			case 0: src[at] = alphabet[Rand() % an]; break;
			case 1: src.erase(at, 1 + Rand() % 4); break;
			default: src.insert(at, 1, (char)(Rand() & 0xff)); break;
			}
		}
		if (iter % 7 == 0) { src.clear(); for (int k = 0; k < 400; ++k) src += alphabet[Rand() % an]; }
		Scope s(nullptr);
		vs::CompileFile(s.prog, src.data(), src.size());
		const int a = s.Snip("Boost(500)"), b = s.Snip("SpeedReset()");
		s.Link();
		for (int fn : { a, b }) {
			if (fn < 0 || s.prog.funcs[fn].unsupported) continue;
			vs::Player p = MakePlayer(300, 400, 50);
			const int r = s.Run(fn, p);
			CHECK(r == vs::R_OK || r == vs::R_ERROR || r == vs::R_UNMODELED);
			++ran;
		}
	}
	CHECK(ran > 100);
}

static void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xff); b.push_back(v >> 8); }
static void Put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xff); }

static std::vector<uint8_t> MakeZip(const std::vector<std::pair<std::string, std::string>>& files)
{
	std::vector<uint8_t> z, cd;
	for (const auto& f : files) {
		const uint32_t lho = (uint32_t)z.size();
		Put32(z, 0x04034b50); Put16(z, 10); Put16(z, 0); Put16(z, 0); Put32(z, 0); Put32(z, 0);
		Put32(z, (uint32_t)f.second.size()); Put32(z, (uint32_t)f.second.size());
		Put16(z, (uint16_t)f.first.size()); Put16(z, 0);
		z.insert(z.end(), f.first.begin(), f.first.end());
		z.insert(z.end(), f.second.begin(), f.second.end());
		Put32(cd, 0x02014b50); Put16(cd, 20); Put16(cd, 10); Put16(cd, 0); Put16(cd, 0); Put32(cd, 0); Put32(cd, 0);
		Put32(cd, (uint32_t)f.second.size()); Put32(cd, (uint32_t)f.second.size());
		Put16(cd, (uint16_t)f.first.size()); Put16(cd, 0); Put16(cd, 0); Put16(cd, 0); Put16(cd, 0); Put32(cd, 0);
		Put32(cd, lho);
		cd.insert(cd.end(), f.first.begin(), f.first.end());
	}
	const uint32_t cdOfs = (uint32_t)z.size();
	z.insert(z.end(), cd.begin(), cd.end());
	Put32(z, 0x06054b50); Put16(z, 0); Put16(z, 0); Put16(z, (uint16_t)files.size()); Put16(z, (uint16_t)files.size());
	Put32(z, (uint32_t)cd.size()); Put32(z, cdOfs); Put16(z, 0);
	return z;
}

static std::string ScratchPath(const char* name)
{
	const char* dir = getenv("TMPDIR");
	if (!dir || !dir[0]) dir = getenv("TEMP"); // Windows
	return std::string(dir && dir[0] ? dir : "/tmp") + "/" + name;
}

static void LoadFromZip(const std::vector<uint8_t>& zip, std::vector<PredScriptFile>& files)
{
	const std::string path = ScratchPath("vscript_test_zip.bin");
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) { CHECK(!"can't write a temp file"); return; }
	const std::vector<uint8_t> pad(100, 0xAA);
	fwrite(pad.data(), 1, pad.size(), f);
	if (!zip.empty()) fwrite(zip.data(), 1, zip.size(), f);
	fwrite(pad.data(), 1, pad.size(), f);
	fclose(f);
	f = fopen(path.c_str(), "rb");
	BspLump l = {};
	l.fileofs = 100;
	l.filelen = (int32_t)zip.size();
	PredZipLoadScripts(f, (long)(zip.size() + 200), l, files);
	fclose(f);
	remove(path.c_str());
}

static void TestPakfile()
{
	std::vector<std::string> names;
	PredSplitVScripts("speedreset.nut directionalbooster  doubleboostonly.nut bad.txt", names);
	CHECK(names.size() == 4 && names[0] == "speedreset.nut" && names[1] == "directionalbooster.nut" && names[3].empty());

	const std::vector<uint8_t> zip = MakeZip({ { "materials/x.vmt", "junk" }, { "scripts\\VScripts\\SpeedReset.nut", kSpeedReset },
		{ "scripts/vscripts/directionalbooster.nut", kBooster } });
	std::vector<PredScriptFile> files(3);
	files[0].name = "speedreset.nut";
	files[1].name = "directionalbooster.nut";
	files[2].name = "doubleboostonly.nut";
	LoadFromZip(zip, files);
	CHECK(files[0].state == PredScriptFile::LOADED && files[0].text == kSpeedReset);
	CHECK(files[1].state == PredScriptFile::LOADED && files[1].text == kBooster);
	CHECK(files[2].state == PredScriptFile::MISSING);

	// Truncated or corrupted zips must not crash or load anything oversized.
	for (int iter = 0; iter < 2000; ++iter) {
		std::vector<uint8_t> z = zip;
		if (iter % 3 == 0) z.resize(Rand() % (z.size() + 1));
		const int flips = 1 + (int)(Rand() % 6);
		for (int k = 0; k < flips && !z.empty(); ++k) z[Rand() % z.size()] = (uint8_t)Rand();
		std::vector<PredScriptFile> fz(2);
		fz[0].name = "speedreset.nut";
		fz[1].name = "directionalbooster.nut";
		LoadFromZip(z, fz);
		for (const PredScriptFile& sf : fz) CHECK(sf.text.size() <= vs::kMaxSource);
	}
}

static PredMapEntity Ent(std::initializer_list<std::pair<const char*, const char*>> kvs)
{
	PredMapEntity e;
	for (const auto& kv : kvs) e.kv.push_back({ kv.first, kv.second });
	return e;
}

struct TestPlayer {
	float origin[3] = { 0, 0, 0 }, velocity[3] = { 0, 0, 0 }, baseVel[3] = { 0, 0, 0 };
	float gravity = 1.0f;
	int flags = FL_CLIENT_BIT;
	uint8_t moveFlags = 1;
	PredPlayerIO io;
	PredPlayerIO& IO(int tick)
	{
		io.origin = origin;
		io.velocity = velocity;
		io.baseVelocity = baseVel;
		io.gravity = &gravity;
		io.flags = &flags;
		io.moveFlags = &moveFlags;
		io.tick = tick;
		io.moveType = PRED_MOVETYPE_WALK;
		io.solid = true;
		io.interval = 1.0f / 64.0f;
		io.curtime = tick * io.interval;
		PredSetHull(io, false);
		return io;
	}
	void Set(const float o[3], float vx, float vy, float vz)
	{
		memcpy(origin, o, sizeof origin);
		velocity[0] = vx; velocity[1] = vy; velocity[2] = vz;
	}
};

static void Tick(PredRuntime& rt, TestPlayer& p, int tick)
{
	PredPlayerIO& io = p.IO(tick);
	PredPreMove(rt, io);
	PredPostMove(rt, io);
}

// Model 1: a box trigger at x,y in [-64, 64], z in [0, 64].
static PredMap* SyntheticMap()
{
	PredMap* m = new PredMap();
	const float lo[3] = { -64, -64, 0 }, hi[3] = { 64, 64, 64 };
	PredBrushSpan b = {};
	b.firstPlane = 0;
	b.contents = 1;
	b.isBox = true;
	for (int c = 0; c < 3; ++c) {
		BspPlane up = {}, dn = {};
		up.normal[c] = 1; up.dist = hi[c]; up.type = c;
		dn.normal[c] = -1; dn.dist = -lo[c]; dn.type = c;
		m->brushPlanes.push_back(up);
		m->brushPlanes.push_back(dn);
		b.boxMins[c] = lo[c];
		b.boxMaxs[c] = hi[c];
	}
	b.planeCount = 6;
	m->brushes.push_back(b);
	BspModel world = {}, trig = {};
	for (int c = 0; c < 3; ++c) { trig.mins[c] = lo[c]; trig.maxs[c] = hi[c]; world.mins[c] = -4096; world.maxs[c] = 4096; }
	m->models = { world, trig };
	m->modelBrushes = { { 0, 0 }, { 0, 1 } };

	m->entities.push_back(Ent({ { "classname", "worldspawn" } }));
	m->entities.push_back(Ent({ { "classname", "logic_script" }, { "targetname", "vscript" },
		{ "vscripts", "speedreset.nut directionalbooster.nut doubleboostonly.nut" } }));
	m->entities.push_back(Ent({ { "classname", "filter_activator_name" }, { "targetname", "meow" }, { "filtername", "meow" },
		{ "Negated", "Allow entities that match criteria" } }));
	m->entities.push_back(Ent({ { "classname", "trigger_multiple" }, { "model", "*1" }, { "wait", "1" }, { "spawnflags", "1" },
		{ "filtername", "meow" }, { "hammerid", "800931" },
		{ "OnStartTouch", "vscript,RunScriptCode,Boost(500),0,-1" },
		{ "OnStartTouch", "clientcommand,Command,playgamesound plar/horizontal_boost.wav,0,-1" },
		{ "OnStartTouch", "!activator,AddOutput,targetname reset,0,-1" },
		{ "OnEndTouch", "vscript\x1bRunScriptCode\x1b" "activator.SetGravity(0.5); if (activator.GetGravity() < 1) { SpeedReset() }\x1b" "0.05\x1b-1" },
		{ "OnEndTouch", "vscript,RunScriptCode,Undefined(),0,-1" },
		{ "OnEndTouch", "vscript,CallScriptFunction,SpeedReset,0,-1" } }));
	PredScriptFile a, b2, c;
	a.name = "speedreset.nut"; a.text = kSpeedReset; a.state = PredScriptFile::LOADED;
	b2.name = "directionalbooster.nut"; b2.text = kBooster; b2.state = PredScriptFile::LOADED;
	c.name = "doubleboostonly.nut";
	m->scriptFiles = { a, b2, c };
	m->loaded = true;
	return m;
}

static void TestTriggerOutputs()
{
	PredMap* map = SyntheticMap();
	PredBuildTriggers(*map);
	CHECK(map->stats.vsScripts == 2 && map->stats.vsMissing == 1);
	CHECK(map->stats.vsFunctions == 2 && map->stats.vsUnsupported == 0);
	CHECK(map->stats.vsPredicted == 3 && map->stats.vsSkipped == 1); // Undefined() is skipped
	CHECK(map->stats.vsUnreadable == 0);

	PredRuntime rt;
	rt.eventQueueFix = true;
	PredAdopt(rt, map);
	rt.world.playerName = "meow";
	TestPlayer p;
	const float inside[3] = { 0, 0, 1 }, outside[3] = { 1000, 0, 1 };
	int tick = 100;
	p.Set(inside, 300, 400, 50);
	Tick(rt, p, tick++); // StartTouch: the outputs are queued
	CHECK(p.velocity[0] == 300);
	p.Set(inside, 300, 400, 50);
	Tick(rt, p, tick++); // eventqueuefix: serviced at the start of the next command
	CHECK_NEAR(p.velocity[0], 600, 1e-4);
	CHECK_NEAR(p.velocity[1], 800, 1e-4);
	CHECK(p.velocity[2] == 50);
	CHECK(rt.world.playerName == "reset");

	// Leave: CallScriptFunction SpeedReset at once, the ESC-separated code 0.05 s later.
	p.Set(outside, 300, 400, 50);
	Tick(rt, p, tick++);
	p.Set(outside, 300, 400, 50);
	Tick(rt, p, tick++);
	CHECK(p.velocity[0] == 0 && p.velocity[1] == 0 && p.velocity[2] == 0);
	CHECK(p.gravity == 1.0f);
	for (int k = 0; k < 4; ++k) { p.Set(outside, 1, 2, 3); Tick(rt, p, tick++); }
	CHECK(p.gravity == 0.5f && p.velocity[0] == 0); // 0.05 s = 4 ticks
	p.gravity = 1.0f;

	// Back in, now named "reset": the filter refuses it.
	for (int k = 0; k < 70; ++k) { p.Set(inside, 300, 400, 50); Tick(rt, p, tick++); }
	CHECK(p.velocity[0] == 300 && p.velocity[1] == 400);

	// Replays from the ring give the same result.
	rt.world.playerName = "meow";
	for (int k = 0; k < 3; ++k) { p.Set(outside, 0, 0, 0); Tick(rt, p, tick++); }
	for (int k = 0; k < 8; ++k) { p.gravity = 1.0f; p.Set(outside, 0, 0, 0); Tick(rt, p, tick++); }
	const int start = tick;
	float first[2] = { 0, 0 };
	for (int pass = 0; pass < 3; ++pass) {
		int t = start;
		p.gravity = 1.0f;
		p.Set(inside, 30, 40, 0);
		Tick(rt, p, t++);
		p.Set(inside, 30, 40, 0);
		Tick(rt, p, t++);
		if (pass == 0) { first[0] = p.velocity[0]; first[1] = p.velocity[1]; }
		CHECK(p.velocity[0] == first[0] && p.velocity[1] == first[1]);
	}
	CHECK_NEAR(first[0], 330, 1e-4);
	CHECK_NEAR(first[1], 440, 1e-4);
	PredReset(rt);
}

static void TestCeaseless()
{
	const char* path = getenv("CEASELESS_BSP");
	FILE* probe = path ? fopen(path, "rb") : nullptr;
	if (!probe) { printf("  bhop_ceaseless.bsp not found, skipped\n"); return; }
	fclose(probe);

	PredMap* map = new PredMap();
	CHECK(PredLoadBsp(path, *map, true, "/nonexistent"));
	PredBuildTriggers(*map);
	CHECK(map->stats.vsUnreadable == 0);
	CHECK(map->stats.vsScripts == 2 && map->stats.vsMissing == 1);
	CHECK(map->stats.vsFunctions == 2 && map->stats.vsUnsupported == 0);

	int booster = -1, teleportsWithReset = 0;
	for (int i = 0, s = 0; i < (int)map->entities.size(); ++i) {
		const PredMapEntity& e = map->entities[i];
		if (PredClassify(e.Get("classname")) == PC_NONE) continue;
		const int si = s++;
		const PredSimEnt& se = map->sim[si];
		bool resets = false;
		for (int k = 0; k < se.actionCount; ++k) {
			const PredAction& a = map->actions[se.actionStart + k];
			if (a.target != PRED_TARGET_SCRIPT) continue;
			vs::Player pl = MakePlayer(300, 400, 50);
			vs::Scratch sc;
			CHECK(vs::Run(map->vsPrograms[a.vsScope], a.vsFunc, pl, sc) == vs::R_OK);
			if (!strcmp(e.Get("hammerid"), "800931")) {
				CHECK_NEAR(pl.vel[0], 600, 1e-4);
				CHECK_NEAR(pl.vel[1], 800, 1e-4);
				CHECK(pl.vel[2] == 50);
				CHECK(a.output == OUT_ONSTARTTOUCH && a.delay == 0 && a.timesToFire == -1);
			}
			if (se.cls == PC_TELEPORT) {
				resets = resets || (pl.vel[0] == 0 && pl.vel[1] == 0 && pl.vel[2] == 0);
				CHECK(a.timesToFire == -1); // ",0": EVENT_FIRE_ALWAYS
			}
		}
		if (!strcmp(e.Get("hammerid"), "800931")) booster = si;
		if (resets) ++teleportsWithReset;
	}
	CHECK(booster >= 0);
	CHECK(teleportsWithReset == 9);
	if (booster < 0) { delete map; return; }

	const PredSimEnt& be = map->sim[booster];
	float mins[3], maxs[3];
	PredTriggerWorldBounds(*map, be, 0.0f, mins, maxs);
	const float inside[3] = { (mins[0] + maxs[0]) * 0.5f, (mins[1] + maxs[1]) * 0.5f, (mins[2] + maxs[2]) * 0.5f - 31.0f };
	const float outside[3] = { 60000, 60000, 60000 };
	PredRuntime rt;
	rt.eventQueueFix = true;
	PredAdopt(rt, map);
	rt.world.playerName = "meow";
	TestPlayer p;
	int tick = 1000;
	p.Set(outside, 0, 0, 0);
	Tick(rt, p, tick++);
	p.Set(inside, 300, 400, 50);
	Tick(rt, p, tick++);
	CHECK(rt.world.ents[booster].inList);
	p.Set(inside, 300, 400, 50);
	Tick(rt, p, tick++);
	CHECK_NEAR(p.velocity[0], 600, 1e-4);
	CHECK_NEAR(p.velocity[1], 800, 1e-4);
	CHECK(p.velocity[2] == 50);
	CHECK(rt.world.playerName == "reset");
	for (int k = 0; k < 4; ++k) { p.Set(outside, 0, 0, 0); Tick(rt, p, tick++); }
	CHECK(rt.world.playerName == "reset");
	for (int k = 0; k < 80; ++k) { p.Set(inside, 300, 400, 50); Tick(rt, p, tick++); }
	CHECK(!rt.world.ents[booster].inList);
	CHECK(p.velocity[0] == 300 && p.velocity[1] == 400 && p.velocity[2] == 50);
	PredReset(rt);
}

int main()
{
	TestInterpreter();
	TestSemantics();
	TestUnsupported();
	TestHostile();
	TestPakfile();
	TestTriggerOutputs();
	TestCeaseless();
	printf("vscript: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
