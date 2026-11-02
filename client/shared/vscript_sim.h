#pragma once

#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE__)
#include <xmmintrin.h>
#define VS_RSQRT_SSE 1
#endif

namespace vs {

static const size_t kMaxSource    = 256 * 1024;
static const size_t kMaxTokens    = 1 << 17;
static const size_t kMaxNodes     = 1 << 20;
static const int    kMaxDepth     = 64;
static const int    kMaxSlots     = 250;
static const int    kMaxArgs      = 16;
static const int    kMaxSteps     = 10000;
static const int    kMaxHeap      = 1024;
static const int    kMaxCallDepth = 8;
static const int    kMaxEvalDepth = 256;
static const int    kMaxStack     = 4096;

enum Op : uint8_t {
	O_NONE, O_LPAREN, O_RPAREN, O_LBRACE, O_RBRACE, O_LBRACK, O_RBRACK, O_COMMA, O_SEMI, O_DOT, O_COLON,
	O_QUESTION, O_PLUS, O_MINUS, O_STAR, O_SLASH, O_PERCENT, O_ASSIGN, O_EQ, O_NE, O_LT, O_LE, O_GT, O_GE,
	O_ANDAND, O_OROR, O_NOT, O_PLUSEQ, O_MINUSEQ, O_STAREQ, O_SLASHEQ, O_PERCENTEQ, O_INC, O_DEC,
	O_NEWSLOT, O_DCOLON, O_OTHER
};

enum TokType : uint8_t { TK_EOF, TK_IDENT, TK_INT, TK_FLOAT, TK_STRING, TK_OP, TK_BAD };

struct Token {
	uint8_t     type = TK_EOF;
	uint8_t     op = O_NONE;
	bool        nl = false; // newline since the previous token; as in Squirrel, block comments don't count
	int64_t     i = 0;
	float       f = 0.0f;
	std::string s;
};

inline bool IsIdStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
inline bool IsDigit(char c)   { return c >= '0' && c <= '9'; }
inline int  HexVal(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Rounded to double then float like Squirrel's scstrtod, whatever the locale's decimal point.
inline float ParseFloat(const std::string& s)
{
	char* end = nullptr;
	double d = strtod(s.c_str(), &end);
	if (*end == '.') {
		std::string t = s;
		const char* dp = localeconv()->decimal_point;
		size_t k = t.find('.');
		if (dp && dp[0] && k != std::string::npos) t.replace(k, 1, dp);
		d = strtod(t.c_str(), nullptr);
	}
	return (float)d;
}

inline bool Tokenize(const char* src, size_t len, std::vector<Token>& out)
{
	out.clear();
	bool clean = true;
	size_t p = 0;
	bool nl = false;
	auto bad = [&](size_t upto) {
		Token t; t.type = TK_BAD; t.nl = nl; nl = false;
		out.push_back(t);
		clean = false;
		p = upto;
	};
	while (p < len && out.size() < kMaxTokens) {
		const char c = src[p];
		if (c == 0) break;
		if (c == '\n') { nl = true; ++p; continue; }
		if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { ++p; continue; }
		if (c == '#' || (c == '/' && p + 1 < len && src[p + 1] == '/')) {
			while (p < len && src[p] != '\n' && src[p] != 0) ++p;
			continue;
		}
		if (c == '/' && p + 1 < len && src[p + 1] == '*') {
			p += 2;
			while (p + 1 < len && !(src[p] == '*' && src[p + 1] == '/')) ++p;
			if (p + 1 >= len) { bad(len); break; }
			p += 2;
			continue;
		}
		Token t;
		t.nl = nl;
		if (IsIdStart(c)) {
			size_t s = p;
			while (p < len && (IsIdStart(src[p]) || IsDigit(src[p]))) ++p;
			t.type = TK_IDENT;
			t.s.assign(src + s, p - s);
			out.push_back(std::move(t));
			nl = false;
			continue;
		}
		if (IsDigit(c)) {
			size_t s = p;
			if (c == '0' && p + 1 < len && (src[p + 1] == 'x' || src[p + 1] == 'X')) {
				p += 2;
				uint64_t v = 0;
				size_t d0 = p;
				while (p < len && HexVal(src[p]) >= 0) { v = v * 16u + (uint64_t)HexVal(src[p]); ++p; }
				if (p == d0) { bad(p); continue; }
				t.type = TK_INT;
				t.i = (int64_t)v;
			} else {
				bool isFloat = false;
				while (p < len && IsDigit(src[p])) ++p;
				if (p < len && src[p] == '.') {
					isFloat = true;
					++p;
					while (p < len && IsDigit(src[p])) ++p;
				}
				if (p < len && (src[p] == 'e' || src[p] == 'E')) {
					isFloat = true;
					++p;
					if (p < len && (src[p] == '+' || src[p] == '-')) ++p;
					if (p >= len || !IsDigit(src[p])) { bad(p); continue; }
					while (p < len && IsDigit(src[p])) ++p;
				}
				std::string num(src + s, p - s);
				if (isFloat) {
					t.type = TK_FLOAT;
					t.f = ParseFloat(num);
				} else if (num.size() > 1 && num[0] == '0') {
					bad(p); // octal
					continue;
				} else {
					uint64_t v = 0;
					for (char d : num) v = v * 10u + (uint64_t)(d - '0');
					t.type = TK_INT;
					t.i = (int64_t)v;
				}
			}
			if (p < len && (IsIdStart(src[p]) || src[p] == '.')) { bad(p); continue; }
			out.push_back(std::move(t));
			nl = false;
			continue;
		}
		if (c == '"' || (c == '@' && p + 1 < len && src[p + 1] == '"')) {
			const bool verbatim = c == '@';
			p += verbatim ? 2 : 1;
			bool ok = true, closed = false;
			while (p < len && src[p] != 0) {
				const char ch = src[p];
				if (ch == '"') {
					if (verbatim && p + 1 < len && src[p + 1] == '"') { t.s.push_back('"'); p += 2; continue; }
					closed = true;
					++p;
					break;
				}
				if (!verbatim && ch == '\n') { ok = false; break; }
				if (!verbatim && ch == '\\') {
					if (p + 1 >= len) { ok = false; break; }
					const char e = src[p + 1];
					p += 2;
					switch (e) {
					case 't': t.s.push_back('\t'); break;
					case 'n': t.s.push_back('\n'); break;
					case 'r': t.s.push_back('\r'); break;
					case 'a': t.s.push_back('\a'); break;
					case 'b': t.s.push_back('\b'); break;
					case 'v': t.s.push_back('\v'); break;
					case 'f': t.s.push_back('\f'); break;
					case '0': t.s.push_back('\0'); break;
					case '\\': t.s.push_back('\\'); break;
					case '"': t.s.push_back('"'); break;
					case '\'': t.s.push_back('\''); break;
					default: ok = false; break;
					}
					if (!ok) break;
					continue;
				}
				t.s.push_back(ch);
				++p;
			}
			if (!ok || !closed) { bad(p); continue; }
			t.type = TK_STRING;
			out.push_back(std::move(t));
			nl = false;
			continue;
		}
		if (c == '\'') {
			// Escaped char literals aren't worth supporting.
			if (p + 2 < len && src[p + 1] != '\\' && src[p + 1] != '\'' && src[p + 2] == '\'') {
				t.type = TK_INT;
				t.i = (int64_t)(signed char)src[p + 1];
				p += 3;
				out.push_back(std::move(t));
				nl = false;
				continue;
			}
			bad(p + 1);
			continue;
		}
		if ((unsigned char)c >= 0x80) { bad(p + 1); continue; }

		struct OpDef { const char* s; uint8_t op; };
		static const OpDef ops[] = {
			{ "<=>", O_OTHER }, { ">>>", O_OTHER }, { "...", O_OTHER },
			{ "==", O_EQ }, { "!=", O_NE }, { "<=", O_LE }, { ">=", O_GE }, { "&&", O_ANDAND }, { "||", O_OROR },
			{ "+=", O_PLUSEQ }, { "-=", O_MINUSEQ }, { "*=", O_STAREQ }, { "/=", O_SLASHEQ }, { "%=", O_PERCENTEQ },
			{ "++", O_INC }, { "--", O_DEC }, { "<-", O_NEWSLOT }, { "::", O_DCOLON }, { "<<", O_OTHER }, { ">>", O_OTHER },
			{ "(", O_LPAREN }, { ")", O_RPAREN }, { "{", O_LBRACE }, { "}", O_RBRACE }, { "[", O_LBRACK }, { "]", O_RBRACK },
			{ ",", O_COMMA }, { ";", O_SEMI }, { ".", O_DOT }, { ":", O_COLON }, { "?", O_QUESTION }, { "+", O_PLUS },
			{ "-", O_MINUS }, { "*", O_STAR }, { "/", O_SLASH }, { "%", O_PERCENT }, { "=", O_ASSIGN }, { "<", O_LT },
			{ ">", O_GT }, { "!", O_NOT },
		};
		uint8_t op = O_OTHER;
		size_t n = 1;
		for (const OpDef& d : ops) {
			const size_t k = strlen(d.s);
			if (p + k <= len && memcmp(src + p, d.s, k) == 0) { op = d.op; n = k; break; }
		}
		t.type = TK_OP;
		t.op = op;
		p += n;
		out.push_back(std::move(t));
		nl = false;
	}
	if (p < len && src[p] != 0) clean = false; // token cap
	return clean;
}

enum NodeKind : uint8_t {
	N_INT, N_FLOAT, N_STR, N_NULL, N_BOOL, N_LOCAL, N_ACTIVATOR, N_CALL, N_VECTOR, N_PRINT,
	N_MEMBER, N_METHOD, N_NEG, N_NOT, N_BIN, N_AND, N_OR, N_TERNARY, N_ASSIGN, N_INCDEC,
	S_BLOCK, S_EXPR, S_LOCAL, S_IF, S_WHILE, S_FOR, S_RETURN, S_BREAK, S_CONTINUE, S_EMPTY
};

enum Member : uint8_t {
	M_X, M_Y, M_Z, M_LENGTH, M_LENGTHSQR, M_LENGTH2D, M_LENGTH2DSQR, M_NORM, M_DOT, M_CROSS,
	M_ISVALID, M_ISPLAYER, M_GETABSVELOCITY, M_GETVELOCITY, M_SETABSVELOCITY, M_SETVELOCITY,
	M_GETORIGIN, M_SETORIGIN, M_SETABSORIGIN, M_GETGRAVITY, M_SETGRAVITY, M_GETBASEVELOCITY,
	M_COUNT
};

static const char* const kMemberNames[M_COUNT] = {
	"x", "y", "z", "Length", "LengthSqr", "Length2D", "Length2DSqr", "Norm", "Dot", "Cross",
	"IsValid", "IsPlayer", "GetAbsVelocity", "GetVelocity", "SetAbsVelocity", "SetVelocity",
	"GetOrigin", "SetOrigin", "SetAbsOrigin", "GetGravity", "SetGravity", "GetBaseVelocity",
};

// N_CALL: a/b arg list, c resolved function, d name string. N_VECTOR/N_PRINT: a/b arg list.
// N_MEMBER: a object, op member. N_METHOD: a object, op member, b/c arg list.
// N_ASSIGN: op, a target, b value. N_INCDEC: op 1 prefix, i delta, a target.
// S_BLOCK: a/b list. S_LOCAL: a slot, b init. S_IF: a cond, b then, c else.
// S_WHILE: a cond, b body. S_FOR: a init, b cond, c step, d body.
struct Node {
	uint8_t k = 0;
	uint8_t op = 0;
	int     a = -1, b = -1, c = -1, d = -1;
	int64_t i = 0;
	float   f = 0.0f;
};

struct Function {
	std::string name; // empty for a RunScriptCode / CallScriptFunction snippet
	int  params = 0;
	int  slots = 0;
	int  body = -1;
	int  nodeBegin = 0, nodeEnd = 0;
	bool unsupported = false;
	std::string why;
};

inline bool IsBuiltinName(const std::string& w)
{
	return w == "activator" || w == "Vector" || w == "printl" || w == "print";
}

struct Program {
	std::vector<Node>        nodes;
	std::vector<int>         lists;
	std::vector<std::string> strings;
	std::vector<Function>    funcs;
	std::vector<std::string> taint;
	bool dynamic = false;
	std::string dynamicWhy;

	// Squirrel keeps the last definition of a name.
	int Find(const char* name) const
	{
		for (int i = (int)funcs.size() - 1; i >= 0; --i)
			if (!funcs[i].name.empty() && funcs[i].name == name) return i;
		return -1;
	}
	bool Tainted(const std::string& name) const
	{
		for (const std::string& t : taint)
			if (t == name) return true;
		return false;
	}
	void AddTaint(const std::string& name)
	{
		if (!Tainted(name) && taint.size() < 4096) taint.push_back(name);
	}
	bool TaintsBuiltin() const
	{
		for (const std::string& t : taint)
			if (IsBuiltinName(t)) return true;
		return false;
	}
	void MakeDynamic(const char* why)
	{
		if (!dynamic) { dynamic = true; dynamicWhy = why; }
	}
};

inline bool IsReserved(const std::string& w)
{
	static const char* const kw[] = {
		"while", "do", "if", "else", "break", "continue", "return", "null", "function", "local", "for", "foreach",
		"in", "typeof", "base", "delete", "try", "catch", "throw", "clone", "yield", "resume", "switch", "case",
		"default", "this", "class", "extends", "constructor", "instanceof", "true", "false", "static", "enum",
		"const", "__LINE__", "__FILE__", "rawcall",
	};
	for (const char* k : kw)
		if (w == k) return true;
	return false;
}

inline bool IsFuncDef(const std::vector<Token>& t, size_t p)
{
	return p + 2 < t.size() && t[p].type == TK_IDENT && t[p].s == "function"
		&& t[p + 1].type == TK_IDENT && !IsReserved(t[p + 1].s)
		&& t[p + 2].type == TK_OP && t[p + 2].op == O_LPAREN;
}

inline void TaintScan(Program& prog, const std::vector<Token>& t, size_t b, size_t e)
{
	for (size_t p = b; p < e && p < t.size(); ++p) {
		const Token& k = t[p];
		if (k.type == TK_STRING) { prog.AddTaint(k.s); continue; }
		if (k.type != TK_IDENT) continue;
		if (k.s == "IncludeScript" || k.s == "DoIncludeScript" || k.s == "compilestring" || k.s == "setroottable"
				|| k.s == "setconsttable" || k.s == "RunScriptFile") {
			prog.MakeDynamic("loads more code");
			continue;
		}
		const bool defined = p > b && t[p - 1].type == TK_IDENT
			&& (t[p - 1].s == "function" || t[p - 1].s == "class" || t[p - 1].s == "delete" || t[p - 1].s == "local"
				|| t[p - 1].s == "const" || t[p - 1].s == "enum");
		bool assigned = false;
		if (p + 1 < e && p + 1 < t.size() && t[p + 1].type == TK_OP) {
			const uint8_t o = t[p + 1].op;
			assigned = o == O_ASSIGN || o == O_NEWSLOT || o == O_PLUSEQ || o == O_MINUSEQ || o == O_STAREQ
				|| o == O_SLASHEQ || o == O_PERCENTEQ || o == O_INC || o == O_DEC;
		}
		if (defined || assigned) prog.AddTaint(k.s);
	}
}

class Parser {
public:
	Parser(Program& prog, const std::vector<Token>& toks, size_t begin, size_t end)
		: pr_(prog), t_(toks), p_(begin), end_(end < toks.size() ? end : toks.size()) {}

	void AddParam(const std::string& name) { NewSlot(name); }

	int Body()
	{
		std::vector<int> list;
		while (!failed_ && p_ < end_) {
			const int s = Statement();
			if (s < 0) break;
			list.push_back(s);
		}
		if (failed_) return -1;
		return AddList(S_BLOCK, list);
	}

	const std::string& why() const { return why_; }
	int slots() const { return slots_; }

private:
	Program&                  pr_;
	const std::vector<Token>& t_;
	size_t                    p_;
	size_t                    end_;
	int                       depth_ = 0;
	int                       loops_ = 0;
	int                       slots_ = 0;
	bool                      failed_ = false;
	std::string               why_;
	std::vector<std::pair<std::string, int>> vars_;

	const Token& Cur() const
	{
		static const Token eof;
		return p_ < end_ ? t_[p_] : eof;
	}
	bool IsOp(uint8_t op) const { const Token& k = Cur(); return k.type == TK_OP && k.op == op; }
	bool IsWord(const char* w) const { const Token& k = Cur(); return k.type == TK_IDENT && k.s == w; }

	int Fail(const std::string& why)
	{
		if (!failed_) { failed_ = true; why_ = why; }
		return -1;
	}
	bool Expect(uint8_t op, const char* what)
	{
		if (failed_) return false;
		if (!IsOp(op)) { Fail(std::string("expected ") + what); return false; }
		++p_;
		return true;
	}

	int Add(uint8_t k, int a = -1, int b = -1, int c = -1, int d = -1)
	{
		if (failed_) return -1;
		if (pr_.nodes.size() >= kMaxNodes) return Fail("script too big");
		Node n;
		n.k = k; n.a = a; n.b = b; n.c = c; n.d = d;
		pr_.nodes.push_back(n);
		return (int)pr_.nodes.size() - 1;
	}
	int AddList(uint8_t k, const std::vector<int>& list)
	{
		if (failed_) return -1;
		const int start = (int)pr_.lists.size();
		pr_.lists.insert(pr_.lists.end(), list.begin(), list.end());
		return Add(k, start, (int)list.size());
	}

	int NewSlot(const std::string& name)
	{
		if (slots_ >= kMaxSlots) return Fail("too many locals");
		vars_.push_back(std::make_pair(name, slots_));
		return slots_++;
	}
	int FindLocal(const std::string& name) const
	{
		for (size_t i = vars_.size(); i-- > 0; )
			if (vars_[i].first == name) return vars_[i].second;
		return -1;
	}

	bool AtEndOfStatement() const
	{
		const Token& k = Cur();
		return p_ >= end_ || k.nl || (k.type == TK_OP && (k.op == O_SEMI || k.op == O_RBRACE));
	}
	// Squirrel's OptionalSemicolon: `else` may follow a statement on the same line.
	bool EndStmt()
	{
		if (failed_) return false;
		if (IsOp(O_SEMI)) { ++p_; return true; }
		if (AtEndOfStatement() || IsWord("else")) return true;
		Fail("end of statement expected");
		return false;
	}

	int Statement()
	{
		if (failed_) return -1;
		if (++depth_ > kMaxDepth) return Fail("nested too deep");
		const int r = StatementInner();
		--depth_;
		return r;
	}

	int StatementInner()
	{
		const Token& k = Cur();
		if (k.type == TK_OP && k.op == O_SEMI) { ++p_; return Add(S_EMPTY); }
		if (k.type == TK_OP && k.op == O_LBRACE) { ++p_; return Block(); }
		if (k.type == TK_IDENT) {
			const std::string& w = k.s;
			if (w == "local") {
				++p_;
				const int r = Locals();
				return r >= 0 && EndStmt() ? r : -1;
			}
			if (w == "if") return If();
			if (w == "while") return While();
			if (w == "for") return For();
			if (w == "return") {
				++p_;
				int e = -1;
				if (!AtEndOfStatement()) { e = Expr(); if (e < 0) return -1; }
				return EndStmt() ? Add(S_RETURN, e) : -1;
			}
			if (w == "break" || w == "continue") {
				if (!loops_) return Fail(w + " outside a loop");
				++p_;
				return EndStmt() ? Add(w == "break" ? S_BREAK : S_CONTINUE) : -1;
			}
			if (IsReserved(w) && w != "null" && w != "true" && w != "false") return Fail("'" + w + "'");
		}
		const int e = Expr();
		if (e < 0) return -1;
		return EndStmt() ? Add(S_EXPR, e) : -1;
	}

	int Block()
	{
		const size_t mark = vars_.size();
		std::vector<int> list;
		for (;;) {
			if (failed_) return -1;
			if (p_ >= end_) return Fail("unterminated block");
			if (IsOp(O_RBRACE)) { ++p_; break; }
			const int s = Statement();
			if (s < 0) return -1;
			list.push_back(s);
		}
		vars_.resize(mark);
		return AddList(S_BLOCK, list);
	}

	int Locals()
	{
		std::vector<int> decls;
		for (;;) {
			const Token& k = Cur();
			if (k.type != TK_IDENT || IsReserved(k.s)) return Fail("local name");
			const std::string name = k.s;
			++p_;
			int init = -1;
			if (IsOp(O_ASSIGN)) {
				++p_;
				init = Expr();
				if (init < 0) return -1;
			}
			const int slot = NewSlot(name);
			if (slot < 0) return -1;
			decls.push_back(Add(S_LOCAL, slot, init));
			if (IsOp(O_COMMA)) { ++p_; continue; }
			break;
		}
		if (decls.size() == 1) return decls[0];
		return AddList(S_BLOCK, decls);
	}

	int If()
	{
		++p_;
		if (!Expect(O_LPAREN, "(")) return -1;
		const int c = Expr();
		if (c < 0 || !Expect(O_RPAREN, ")")) return -1;
		const int th = Statement();
		if (th < 0) return -1;
		int el = -1;
		if (IsWord("else")) {
			++p_;
			el = Statement();
			if (el < 0) return -1;
		}
		return Add(S_IF, c, th, el);
	}

	int While()
	{
		++p_;
		if (!Expect(O_LPAREN, "(")) return -1;
		const int c = Expr();
		if (c < 0 || !Expect(O_RPAREN, ")")) return -1;
		++loops_;
		const int body = Statement();
		--loops_;
		if (body < 0) return -1;
		return Add(S_WHILE, c, body);
	}

	int For()
	{
		++p_;
		if (!Expect(O_LPAREN, "(")) return -1;
		const size_t mark = vars_.size();
		int init = -1, cond = -1, step = -1;
		if (IsWord("local")) {
			++p_;
			init = Locals();
			if (init < 0) return -1;
		} else if (!IsOp(O_SEMI)) {
			const int e = Expr();
			if (e < 0) return -1;
			init = Add(S_EXPR, e);
		}
		if (!Expect(O_SEMI, ";")) return -1;
		if (!IsOp(O_SEMI)) { cond = Expr(); if (cond < 0) return -1; }
		if (!Expect(O_SEMI, ";")) return -1;
		if (!IsOp(O_RPAREN)) { step = Expr(); if (step < 0) return -1; }
		if (!Expect(O_RPAREN, ")")) return -1;
		++loops_;
		const int body = Statement();
		--loops_;
		if (body < 0) return -1;
		vars_.resize(mark);
		return Add(S_FOR, init, cond, step, body);
	}

	bool IsLValue(int n) const
	{
		const Node& x = pr_.nodes[n];
		return x.k == N_LOCAL || (x.k == N_MEMBER && x.op <= M_Z);
	}

	int Expr()
	{
		if (failed_) return -1;
		if (++depth_ > kMaxDepth) return Fail("nested too deep");
		int r = Ternary();
		if (r >= 0 && Cur().type == TK_OP) {
			const uint8_t op = Cur().op;
			if (op == O_ASSIGN || op == O_PLUSEQ || op == O_MINUSEQ || op == O_STAREQ || op == O_SLASHEQ || op == O_PERCENTEQ) {
				if (!IsLValue(r)) { --depth_; return Fail("assignment to something other than a local or x/y/z"); }
				++p_;
				const int v = Expr();
				r = v < 0 ? -1 : Add(N_ASSIGN, r, v);
				if (r >= 0) pr_.nodes[r].op = op;
			} else if (op == O_NEWSLOT) {
				r = Fail("'<-'");
			}
		}
		--depth_;
		return r;
	}

	int Ternary()
	{
		const int c = Or();
		if (c < 0 || !IsOp(O_QUESTION)) return c;
		++p_;
		const int a = Expr();
		if (a < 0 || !Expect(O_COLON, ":")) return -1;
		const int b = Expr();
		return b < 0 ? -1 : Add(N_TERNARY, c, a, b);
	}

	int Or()
	{
		int l = And();
		while (l >= 0 && IsOp(O_OROR)) {
			++p_;
			const int r = And();
			l = r < 0 ? -1 : Add(N_OR, l, r);
		}
		return l;
	}

	int And()
	{
		int l = Binary(0);
		while (l >= 0 && IsOp(O_ANDAND)) {
			++p_;
			const int r = Binary(0);
			l = r < 0 ? -1 : Add(N_AND, l, r);
		}
		return l;
	}

	int Binary(int level)
	{
		if (level > 3) return Unary();
		int l = Binary(level + 1);
		for (;;) {
			if (l < 0 || Cur().type != TK_OP) return l;
			const uint8_t op = Cur().op;
			bool match = false;
			switch (level) {
			case 0: match = op == O_EQ || op == O_NE; break;
			case 1: match = op == O_LT || op == O_LE || op == O_GT || op == O_GE; break;
			case 2: match = op == O_PLUS || op == O_MINUS; break;
			default: match = op == O_STAR || op == O_SLASH || op == O_PERCENT; break;
			}
			if (!match) return l;
			++p_;
			const int r = Binary(level + 1);
			l = r < 0 ? -1 : Add(N_BIN, l, r);
			if (l >= 0) pr_.nodes[l].op = op;
		}
	}

	int Unary()
	{
		if (failed_) return -1;
		if (++depth_ > kMaxDepth) return Fail("nested too deep");
		int r;
		if (IsOp(O_MINUS)) {
			++p_;
			const int x = Unary();
			r = x < 0 ? -1 : Add(N_NEG, x);
		} else if (IsOp(O_NOT)) {
			++p_;
			const int x = Unary();
			r = x < 0 ? -1 : Add(N_NOT, x);
		} else if (IsOp(O_INC) || IsOp(O_DEC)) {
			const bool inc = IsOp(O_INC);
			++p_;
			const int x = Unary();
			if (x >= 0 && !IsLValue(x)) r = Fail("++/-- on something other than a local or x/y/z");
			else r = x < 0 ? -1 : Add(N_INCDEC, x);
			if (r >= 0) { pr_.nodes[r].op = 1; pr_.nodes[r].i = inc ? 1 : -1; }
		} else {
			r = Postfix();
		}
		--depth_;
		return r;
	}

	bool Args(std::vector<int>& out)
	{
		if (!Expect(O_LPAREN, "(")) return false;
		out.clear();
		if (IsOp(O_RPAREN)) { ++p_; return true; }
		for (;;) {
			if ((int)out.size() >= kMaxArgs) { Fail("too many arguments"); return false; }
			const int e = Expr();
			if (e < 0) return false;
			out.push_back(e);
			if (IsOp(O_COMMA)) { ++p_; continue; }
			return Expect(O_RPAREN, ")");
		}
	}

	int ListNode(uint8_t k, const std::vector<int>& args, int extraA = -1)
	{
		const int start = (int)pr_.lists.size();
		pr_.lists.insert(pr_.lists.end(), args.begin(), args.end());
		if (k == N_METHOD) return Add(k, extraA, start, (int)args.size());
		return Add(k, start, (int)args.size());
	}

	int Postfix()
	{
		int e = Primary();
		std::vector<int> args;
		for (;;) {
			if (e < 0) return -1;
			if (IsOp(O_DOT)) {
				++p_;
				const Token& k = Cur();
				if (k.type != TK_IDENT) return Fail("member name");
				int m = -1;
				for (int i = 0; i < M_COUNT; ++i)
					if (k.s == kMemberNames[i]) { m = i; break; }
				if (m < 0) return Fail("member '" + k.s + "'");
				++p_;
				if (IsOp(O_LPAREN)) {
					if (m <= M_Z) return Fail("calling x/y/z");
					if (!Args(args)) return -1;
					e = ListNode(N_METHOD, args, e);
				} else {
					if (m > M_Z) return Fail("'" + std::string(kMemberNames[m]) + "' without a call");
					e = Add(N_MEMBER, e);
				}
				if (e >= 0) pr_.nodes[e].op = (uint8_t)m;
				continue;
			}
			if ((IsOp(O_INC) || IsOp(O_DEC)) && !Cur().nl) {
				const bool inc = IsOp(O_INC);
				if (!IsLValue(e)) return Fail("++/-- on something other than a local or x/y/z");
				++p_;
				e = Add(N_INCDEC, e);
				if (e >= 0) { pr_.nodes[e].op = 0; pr_.nodes[e].i = inc ? 1 : -1; }
				continue;
			}
			if (IsOp(O_LBRACK)) return Fail("indexing");
			if (IsOp(O_LPAREN)) return Fail("calling an expression");
			return e;
		}
	}

	int Primary()
	{
		const Token& k = Cur();
		switch (k.type) {
		case TK_INT: {
			++p_;
			const int n = Add(N_INT);
			if (n >= 0) pr_.nodes[n].i = k.i;
			return n; }
		case TK_FLOAT: {
			++p_;
			const int n = Add(N_FLOAT);
			if (n >= 0) pr_.nodes[n].f = k.f;
			return n; }
		case TK_STRING: {
			++p_;
			pr_.strings.push_back(k.s);
			return Add(N_STR, (int)pr_.strings.size() - 1); }
		case TK_OP:
			if (k.op == O_LPAREN) {
				++p_;
				const int e = Expr();
				return e >= 0 && Expect(O_RPAREN, ")") ? e : -1;
			}
			if (k.op == O_DCOLON) return Fail("'::'");
			return Fail("unexpected operator");
		case TK_IDENT:
			break;
		default:
			return Fail("unexpected token");
		}

		const std::string name = k.s;
		if (name == "null") { ++p_; return Add(N_NULL); }
		if (name == "true" || name == "false") {
			++p_;
			const int n = Add(N_BOOL);
			if (n >= 0) pr_.nodes[n].i = name == "true";
			return n;
		}
		if (IsReserved(name)) return Fail("'" + name + "'");
		const int slot = FindLocal(name);
		++p_;
		if (slot >= 0) {
			if (IsOp(O_LPAREN)) return Fail("calling a local");
			return Add(N_LOCAL, slot);
		}
		if (IsOp(O_LPAREN)) {
			std::vector<int> args;
			if (!Args(args)) return -1;
			if (name == "Vector") return ListNode(N_VECTOR, args);
			if (name == "printl" || name == "print") return ListNode(N_PRINT, args);
			const int n = ListNode(N_CALL, args);
			if (n >= 0) {
				pr_.strings.push_back(name);
				pr_.nodes[n].d = (int)pr_.strings.size() - 1;
			}
			return n;
		}
		if (name == "activator") return Add(N_ACTIVATOR);
		return Fail("global '" + name + "'");
	}
};

inline size_t MatchBrace(const std::vector<Token>& t, size_t open)
{
	int depth = 0;
	for (size_t p = open; p < t.size(); ++p) {
		if (t[p].type != TK_OP) continue;
		if (t[p].op == O_LBRACE) ++depth;
		else if (t[p].op == O_RBRACE && --depth == 0) return p;
	}
	return (size_t)-1;
}

inline void CompileFile(Program& prog, const char* text, size_t len)
{
	if (len > kMaxSource) { prog.MakeDynamic("script too big"); return; }
	std::vector<Token> toks;
	const bool clean = Tokenize(text, len, toks);
	const size_t firstFunc = prog.funcs.size();
	size_t p = 0;
	while (p < toks.size()) {
		if (!IsFuncDef(toks, p)) {
			const size_t s = p;
			int depth = 0;
			for (; p < toks.size(); ++p) {
				if (depth == 0 && p != s && IsFuncDef(toks, p)) break;
				if (toks[p].type != TK_OP) continue;
				const uint8_t o = toks[p].op;
				if (o == O_LPAREN || o == O_LBRACE || o == O_LBRACK) ++depth;
				else if ((o == O_RPAREN || o == O_RBRACE || o == O_RBRACK) && depth > 0) --depth;
			}
			TaintScan(prog, toks, s, p);
			continue;
		}

		Function f;
		f.name = toks[p + 1].s;
		size_t q = p + 3;
		std::vector<std::string> params;
		bool ok = true;
		if (q < toks.size() && toks[q].type == TK_OP && toks[q].op == O_RPAREN) {
			++q;
		} else {
			for (;;) {
				if (q >= toks.size() || toks[q].type != TK_IDENT || IsReserved(toks[q].s)) { ok = false; break; }
				params.push_back(toks[q].s);
				++q;
				if (q < toks.size() && toks[q].type == TK_OP && toks[q].op == O_COMMA) { ++q; continue; }
				if (q < toks.size() && toks[q].type == TK_OP && toks[q].op == O_RPAREN) { ++q; break; }
				ok = false;
				break;
			}
			if (!ok) {
				f.why = "parameter list";
				int depth = 1;
				for (q = p + 3; q < toks.size() && depth > 0; ++q)
					if (toks[q].type == TK_OP) depth += toks[q].op == O_LPAREN ? 1 : toks[q].op == O_RPAREN ? -1 : 0;
			}
		}
		if (q >= toks.size() || toks[q].type != TK_OP || toks[q].op != O_LBRACE) {
			// Not a plain definition after all, or truncated.
			TaintScan(prog, toks, p, q);
			prog.AddTaint(f.name);
			p = q;
			continue;
		}
		const size_t close = MatchBrace(toks, q);
		if (close == (size_t)-1) {
			f.unsupported = true;
			f.why = "unterminated function";
			prog.funcs.push_back(f);
			break;
		}
		if (ok && params.size() > (size_t)kMaxArgs) { ok = false; f.why = "too many parameters"; }
		if (ok) {
			const size_t nodeMark = prog.nodes.size(), listMark = prog.lists.size(), strMark = prog.strings.size();
			Parser ps(prog, toks, q + 1, close);
			for (const std::string& a : params) ps.AddParam(a);
			const int body = ps.Body();
			if (body < 0) {
				f.unsupported = true;
				f.why = ps.why();
				prog.nodes.resize(nodeMark);
				prog.lists.resize(listMark);
				prog.strings.resize(strMark);
				// What it didn't parse may still redefine things when called.
				TaintScan(prog, toks, q + 1, close);
			} else {
				f.params = (int)params.size();
				f.body = body;
				f.slots = ps.slots();
				f.nodeBegin = (int)nodeMark;
				f.nodeEnd = (int)prog.nodes.size();
			}
		} else {
			f.unsupported = true;
			TaintScan(prog, toks, q + 1, close);
		}
		prog.funcs.push_back(f);
		p = close + 1;
	}
	// If Squirrel rejects what we couldn't lex, none of the file exists.
	if (!clean)
		for (size_t i = firstFunc; i < prog.funcs.size(); ++i)
			if (!prog.funcs[i].unsupported) { prog.funcs[i].unsupported = true; prog.funcs[i].why = "file may not compile"; }
}

inline int CompileSnippet(Program& prog, const std::string& code)
{
	if (code.empty() || code.size() > kMaxSource) return -1;
	std::vector<Token> toks;
	const bool clean = Tokenize(code.c_str(), code.size(), toks);
	const size_t nodeMark = prog.nodes.size(), listMark = prog.lists.size(), strMark = prog.strings.size();
	Parser ps(prog, toks, 0, toks.size());
	const int body = clean ? ps.Body() : -1;
	if (body < 0) {
		prog.nodes.resize(nodeMark);
		prog.lists.resize(listMark);
		prog.strings.resize(strMark);
		TaintScan(prog, toks, 0, toks.size());
		return -1;
	}
	Function f;
	f.body = body;
	f.slots = ps.slots();
	f.nodeBegin = (int)nodeMark;
	f.nodeEnd = (int)prog.nodes.size();
	prog.funcs.push_back(f);
	return (int)prog.funcs.size() - 1;
}

inline int CompileCall(Program& prog, const std::string& name)
{
	if (name.empty() || IsReserved(name)) return -1;
	for (char c : name)
		if (!IsIdStart(c) && !IsDigit(c)) return -1;
	if (prog.nodes.size() + 3 > kMaxNodes) return -1;
	Function f;
	f.nodeBegin = (int)prog.nodes.size();
	Node call;
	call.k = N_CALL;
	call.a = (int)prog.lists.size();
	call.b = 0;
	prog.strings.push_back(name);
	call.d = (int)prog.strings.size() - 1;
	prog.nodes.push_back(call);
	Node expr;
	expr.k = S_EXPR;
	expr.a = (int)prog.nodes.size() - 1;
	prog.nodes.push_back(expr);
	Node block;
	block.k = S_BLOCK;
	block.a = (int)prog.lists.size();
	block.b = 1;
	prog.lists.push_back((int)prog.nodes.size() - 1);
	prog.nodes.push_back(block);
	f.body = (int)prog.nodes.size() - 1;
	f.nodeEnd = (int)prog.nodes.size();
	prog.funcs.push_back(f);
	return (int)prog.funcs.size() - 1;
}

inline void Link(Program& prog)
{
	if (prog.TaintsBuiltin()) prog.MakeDynamic("redefines a builtin");
	for (const Function& f : prog.funcs)
		if (IsBuiltinName(f.name)) prog.MakeDynamic("redefines a builtin");

	for (Function& f : prog.funcs) {
		if (f.unsupported) continue;
		if (prog.dynamic) { f.unsupported = true; f.why = prog.dynamicWhy; continue; }
		if (!f.name.empty() && prog.Tainted(f.name)) { f.unsupported = true; f.why = "may be redefined"; continue; }
		for (int n = f.nodeBegin; n < f.nodeEnd && !f.unsupported; ++n) {
			Node& x = prog.nodes[n];
			if (x.k != N_CALL) continue;
			const std::string& callee = prog.strings[x.d];
			x.c = prog.Find(callee.c_str());
			if (x.c < 0) { f.unsupported = true; f.why = "calls '" + callee + "'"; }
		}
	}
	for (bool changed = true; changed; ) {
		changed = false;
		for (Function& f : prog.funcs) {
			if (f.unsupported) continue;
			for (int n = f.nodeBegin; n < f.nodeEnd; ++n) {
				const Node& x = prog.nodes[n];
				if (x.k != N_CALL || x.c < 0 || !prog.funcs[x.c].unsupported) continue;
				f.unsupported = true;
				f.why = "calls unsupported '" + prog.funcs[x.c].name + "'";
				changed = true;
				break;
			}
		}
	}
}

enum Result { R_OK = 0, R_ERROR = 1, R_UNMODELED = 2 };

enum ValType : uint8_t { V_NULL, V_BOOL, V_INT, V_FLOAT, V_VEC, V_STR, V_PLAYER };

struct Val {
	uint8_t t = V_NULL;
	int64_t i = 0; // bool, integer, or Vector / string index
	float   f = 0.0f;
};

struct Vec3 { float x, y, z; };

struct Player {
	float vel[3];
	float origin[3];
	float baseVel[3];
	float gravity;
};

struct Scratch {
	std::vector<Val>  stack;
	std::vector<Vec3> heap;
	void Reserve()
	{
		stack.reserve(kMaxStack);
		heap.reserve(kMaxHeap);
	}
};

// Vector::NormalizeInPlace on x86: rsqrtss and one Newton step (mathlib vector.h).
inline float NormalizeInPlace(Vec3& v)
{
	const float sqrlen = v.x * v.x + v.y * v.y + v.z * v.z + 1.0e-10f;
	float inv;
#ifdef VS_RSQRT_SSE
	__m128 xx = _mm_load_ss(&sqrlen);
	__m128 xr = _mm_rsqrt_ss(xx);
	__m128 xt = _mm_mul_ss(xr, xr);
	xt = _mm_mul_ss(xt, xx);
	xt = _mm_sub_ss(_mm_set_ss(3.f), xt);
	xt = _mm_mul_ss(xt, _mm_set_ss(0.5f));
	xr = _mm_mul_ss(xr, xt);
	_mm_store_ss(&inv, xr);
#else
	inv = 1.0f / sqrtf(sqrlen);
#endif
	v.x *= inv;
	v.y *= inv;
	v.z *= inv;
	return sqrlen * inv;
}

class Exec {
public:
	Exec(const Program& prog, Player& pl, Scratch& sc, int maxSteps) : pr_(prog), pl_(pl), sc_(sc), maxSteps_(maxSteps) {}

	int status = R_OK;
	int steps() const { return steps_; }

	bool Call(int fi, const Val* args, int argc, Val& out)
	{
		if (fi < 0 || fi >= (int)pr_.funcs.size()) return Abort(R_UNMODELED);
		const Function& f = pr_.funcs[fi];
		if (f.unsupported || f.body < 0) return Abort(R_UNMODELED);
		if (argc != f.params) return Abort(R_ERROR);
		if (calls_ >= kMaxCallDepth) return Abort(R_UNMODELED);
		if (sc_.stack.size() + (size_t)f.slots > (size_t)kMaxStack) return Abort(R_UNMODELED);
		const size_t savedBase = base_;
		base_ = sc_.stack.size();
		sc_.stack.resize(base_ + (size_t)f.slots, Val());
		for (int i = 0; i < argc; ++i) sc_.stack[base_ + i] = args[i];
		++calls_;
		const int flow = Stmt(f.body);
		--calls_;
		sc_.stack.resize(base_);
		base_ = savedBase;
		if (flow == F_ABORT) return false;
		out = flow == F_RETURN ? ret_ : Val();
		return true;
	}

private:
	enum Flow { F_NORMAL, F_BREAK, F_CONTINUE, F_RETURN, F_ABORT };

	const Program& pr_;
	Player&        pl_;
	Scratch&       sc_;
	int            maxSteps_;
	size_t         base_ = 0;
	int            steps_ = 0;
	int            calls_ = 0;
	int            depth_ = 0;
	Val            ret_;

	bool Abort(int why)
	{
		if (status == R_OK) status = why;
		return false;
	}
	bool Step() { return ++steps_ <= maxSteps_ || Abort(R_UNMODELED); }

	static Val Int(int64_t i)   { Val v; v.t = V_INT; v.i = i; return v; }
	static Val Float(float f)   { Val v; v.t = V_FLOAT; v.f = f; return v; }
	static Val Bool(bool b)     { Val v; v.t = V_BOOL; v.i = b ? 1 : 0; return v; }
	static bool Numeric(const Val& v) { return v.t == V_INT || v.t == V_FLOAT; }
	static float ToFloat(const Val& v) { return v.t == V_INT ? (float)v.i : v.f; }
	static bool IsFalse(const Val& v)
	{
		switch (v.t) {
		case V_NULL:  return true;
		case V_BOOL:  return v.i == 0;
		case V_INT:   return v.i == 0;
		case V_FLOAT: return v.f == 0.0f;
		default:      return false;
		}
	}

	bool NewVec(float x, float y, float z, Val& out)
	{
		if ((int)sc_.heap.size() >= kMaxHeap) return Abort(R_UNMODELED);
		Vec3 v = { x, y, z };
		sc_.heap.push_back(v);
		out.t = V_VEC;
		out.i = (int64_t)sc_.heap.size() - 1;
		return true;
	}
	Vec3& V(const Val& v) { return sc_.heap[(size_t)v.i]; }

	bool PushList(int start, int count)
	{
		for (int k = 0; k < count; ++k) {
			Val v;
			if (!Eval(pr_.lists[(size_t)start + k], v)) return false;
			if (sc_.stack.size() >= (size_t)kMaxStack) return Abort(R_UNMODELED);
			sc_.stack.push_back(v);
		}
		return true;
	}

	// Squirrel's arithmetic, float32 like the game's build.
	bool Arith(uint8_t op, const Val& a, const Val& b, Val& out)
	{
		if (a.t == V_INT && b.t == V_INT) {
			const uint64_t x = (uint64_t)a.i, y = (uint64_t)b.i;
			switch (op) {
			case O_PLUS:  out = Int((int64_t)(x + y)); return true;
			case O_MINUS: out = Int((int64_t)(x - y)); return true;
			case O_STAR:  out = Int((int64_t)(x * y)); return true;
			case O_SLASH:
			case O_PERCENT:
				if (b.i == 0) return Abort(R_ERROR);
				if (a.i == INT64_MIN && b.i == -1) out = Int(op == O_SLASH ? INT64_MIN : 0);
				else out = Int(op == O_SLASH ? a.i / b.i : a.i % b.i);
				return true;
			default: return Abort(R_UNMODELED);
			}
		}
		if (Numeric(a) && Numeric(b)) {
			const float x = ToFloat(a), y = ToFloat(b);
			switch (op) {
			case O_PLUS:    out = Float(x + y); return true;
			case O_MINUS:   out = Float(x - y); return true;
			case O_STAR:    out = Float(x * y); return true;
			case O_SLASH:   out = Float(x / y); return true;
			default:        return Abort(R_UNMODELED); // including float modulo
			}
		}
		if (a.t == V_VEC) {
			if (b.t == V_VEC && (op == O_PLUS || op == O_MINUS)) {
				const Vec3 p = V(a), q = V(b);
				return op == O_PLUS ? NewVec(p.x + q.x, p.y + q.y, p.z + q.z, out)
				                    : NewVec(p.x - q.x, p.y - q.y, p.z - q.z, out);
			}
			if (Numeric(b) && op == O_STAR) {
				const Vec3 p = V(a);
				const float s = ToFloat(b);
				return NewVec(p.x * s, p.y * s, p.z * s, out);
			}
			if (Numeric(b) && op == O_SLASH) {
				// Vector::operator/(float) multiplies by the reciprocal.
				const Vec3 p = V(a);
				const float s = 1.0f / ToFloat(b);
				return NewVec(p.x * s, p.y * s, p.z * s, out);
			}
			return Abort(R_UNMODELED);
		}
		if (a.t == V_STR || b.t == V_STR || a.t == V_PLAYER) return Abort(R_UNMODELED);
		return Abort(R_ERROR);
	}

	bool Compare(uint8_t op, const Val& a, const Val& b, Val& out)
	{
		if (op == O_EQ || op == O_NE) {
			bool eq;
			if (Numeric(a) && Numeric(b)) {
				eq = a.t == V_INT && b.t == V_INT ? a.i == b.i : ToFloat(a) == ToFloat(b);
			} else if (a.t != b.t) {
				eq = false;
			} else if (a.t == V_STR) {
				eq = pr_.strings[(size_t)a.i] == pr_.strings[(size_t)b.i];
			} else {
				eq = a.i == b.i; // null, bool, same Vector instance, the player
			}
			out = Bool(op == O_EQ ? eq : !eq);
			return true;
		}
		if (!Numeric(a) || !Numeric(b)) {
			if (a.t == V_NULL || b.t == V_NULL || a.t == V_BOOL || b.t == V_BOOL) return Abort(R_ERROR);
			return Abort(R_UNMODELED);
		}
		bool r;
		if (a.t == V_INT && b.t == V_INT) {
			r = op == O_LT ? a.i < b.i : op == O_LE ? a.i <= b.i : op == O_GT ? a.i > b.i : a.i >= b.i;
		} else {
			const float x = ToFloat(a), y = ToFloat(b);
			r = op == O_LT ? x < y : op == O_LE ? x <= y : op == O_GT ? x > y : x >= y;
		}
		out = Bool(r);
		return true;
	}

	bool GetMember(const Val& obj, uint8_t m, Val& out)
	{
		if (obj.t == V_NULL) return Abort(R_ERROR);
		if (obj.t != V_VEC || m > M_Z) return Abort(R_UNMODELED);
		const Vec3& v = V(obj);
		out = Float(m == M_X ? v.x : m == M_Y ? v.y : v.z);
		return true;
	}

	bool SetMember(const Val& obj, uint8_t m, const Val& value)
	{
		if (obj.t == V_NULL) return Abort(R_ERROR);
		if (obj.t != V_VEC || m > M_Z) return Abort(R_UNMODELED);
		if (!Numeric(value)) return Abort(R_ERROR);
		Vec3& v = V(obj);
		(m == M_X ? v.x : m == M_Y ? v.y : v.z) = ToFloat(value);
		return true;
	}

	bool VecArg(const Val* args, int argc, Vec3& out)
	{
		if (argc != 1) return Abort(R_ERROR);
		if (args[0].t != V_VEC) return Abort(args[0].t == V_NULL || Numeric(args[0]) ? R_ERROR : R_UNMODELED);
		out = V(args[0]);
		return true;
	}

	bool Method(const Val& obj, uint8_t m, const Val* args, int argc, Val& out)
	{
		if (obj.t == V_NULL) return Abort(R_ERROR);
		if (obj.t == V_VEC) {
			if (m < M_LENGTH || m > M_CROSS) return Abort(R_UNMODELED);
			const bool binary = m == M_DOT || m == M_CROSS;
			if (argc != (binary ? 1 : 0)) return Abort(R_ERROR);
			const Vec3 v = V(obj);
			switch (m) {
			case M_LENGTH:      out = Float(sqrtf(v.x * v.x + v.y * v.y + v.z * v.z)); return true;
			case M_LENGTHSQR:   out = Float(v.x * v.x + v.y * v.y + v.z * v.z); return true;
			case M_LENGTH2D:    out = Float(sqrtf(v.x * v.x + v.y * v.y)); return true;
			case M_LENGTH2DSQR: out = Float(v.x * v.x + v.y * v.y); return true;
			case M_NORM:        out = Float(NormalizeInPlace(V(obj))); return true;
			case M_DOT:
			case M_CROSS: {
				Vec3 w;
				if (!VecArg(args, argc, w)) return false;
				if (m == M_DOT) { out = Float(v.x * w.x + v.y * w.y + v.z * w.z); return true; }
				return NewVec(v.y * w.z - v.z * w.y, v.z * w.x - v.x * w.z, v.x * w.y - v.y * w.x, out); }
			default:
				return Abort(R_UNMODELED);
			}
		}
		if (obj.t != V_PLAYER || m < M_ISVALID) return Abort(R_UNMODELED);
		Vec3 w;
		switch (m) {
		case M_ISVALID:
		case M_ISPLAYER:
			if (argc != 0) return Abort(R_ERROR);
			out = Bool(true);
			return true;
		case M_GETABSVELOCITY:
		case M_GETVELOCITY:
			if (argc != 0) return Abort(R_ERROR);
			return NewVec(pl_.vel[0], pl_.vel[1], pl_.vel[2], out);
		case M_GETORIGIN:
			if (argc != 0) return Abort(R_ERROR);
			return NewVec(pl_.origin[0], pl_.origin[1], pl_.origin[2], out);
		case M_GETBASEVELOCITY:
			if (argc != 0) return Abort(R_ERROR);
			return NewVec(pl_.baseVel[0], pl_.baseVel[1], pl_.baseVel[2], out);
		case M_GETGRAVITY:
			if (argc != 0) return Abort(R_ERROR);
			out = Float(pl_.gravity);
			return true;
		case M_SETABSVELOCITY:
		case M_SETVELOCITY:
			if (!VecArg(args, argc, w)) return false;
			pl_.vel[0] = w.x; pl_.vel[1] = w.y; pl_.vel[2] = w.z;
			out = Val();
			return true;
		case M_SETORIGIN:
		case M_SETABSORIGIN:
			if (!VecArg(args, argc, w)) return false;
			pl_.origin[0] = w.x; pl_.origin[1] = w.y; pl_.origin[2] = w.z;
			out = Val();
			return true;
		case M_SETGRAVITY:
			if (argc != 1 || !Numeric(args[0])) return Abort(R_ERROR);
			pl_.gravity = ToFloat(args[0]);
			out = Val();
			return true;
		default:
			return Abort(R_UNMODELED);
		}
	}

	bool Store(int target, const Val& value)
	{
		const Node& t = pr_.nodes[target];
		if (t.k == N_LOCAL) { sc_.stack[base_ + (size_t)t.a] = value; return true; }
		Val obj;
		if (!Eval(t.a, obj)) return false;
		return SetMember(obj, t.op, value);
	}

	bool Eval(int n, Val& out)
	{
		if (depth_ >= kMaxEvalDepth) return Abort(R_UNMODELED);
		++depth_;
		const bool ok = EvalNode(n, out);
		--depth_;
		return ok;
	}

	bool EvalNode(int n, Val& out)
	{
		if (!Step()) return false;
		const Node& x = pr_.nodes[n];
		switch (x.k) {
		case N_INT:       out = Int(x.i); return true;
		case N_FLOAT:     out = Float(x.f); return true;
		case N_STR:       out.t = V_STR; out.i = x.a; return true;
		case N_NULL:      out = Val(); return true;
		case N_BOOL:      out = Bool(x.i != 0); return true;
		case N_LOCAL:     out = sc_.stack[base_ + (size_t)x.a]; return true;
		case N_ACTIVATOR: out = Val(); out.t = V_PLAYER; return true;
		case N_CALL: {
			const size_t mark = sc_.stack.size();
			if (!PushList(x.a, x.b)) return false;
			const bool ok = Call(x.c, sc_.stack.data() + mark, x.b, out);
			sc_.stack.resize(mark);
			return ok; }
		case N_VECTOR: {
			const size_t mark = sc_.stack.size();
			if (!PushList(x.a, x.b)) return false;
			if (x.b > 3) return Abort(R_UNMODELED);
			float c[3] = { 0.0f, 0.0f, 0.0f };
			for (int k = 0; k < x.b; ++k) {
				const Val& a = sc_.stack[mark + k];
				if (!Numeric(a)) return Abort(R_UNMODELED);
				c[k] = ToFloat(a);
			}
			sc_.stack.resize(mark);
			return NewVec(c[0], c[1], c[2], out); }
		case N_PRINT: {
			const size_t mark = sc_.stack.size();
			if (!PushList(x.a, x.b)) return false;
			sc_.stack.resize(mark);
			out = Val();
			return true; }
		case N_MEMBER: {
			Val obj;
			return Eval(x.a, obj) && GetMember(obj, x.op, out); }
		case N_METHOD: {
			Val obj;
			if (!Eval(x.a, obj)) return false;
			const size_t mark = sc_.stack.size();
			if (!PushList(x.b, x.c)) return false;
			const bool ok = Method(obj, x.op, sc_.stack.data() + mark, x.c, out);
			sc_.stack.resize(mark);
			return ok; }
		case N_NEG: {
			Val v;
			if (!Eval(x.a, v)) return false;
			if (v.t == V_INT)   { out = Int((int64_t)(0u - (uint64_t)v.i)); return true; }
			if (v.t == V_FLOAT) { out = Float(-v.f); return true; }
			return Abort(v.t == V_NULL || v.t == V_BOOL ? R_ERROR : R_UNMODELED); }
		case N_NOT: {
			Val v;
			if (!Eval(x.a, v)) return false;
			out = Bool(IsFalse(v));
			return true; }
		case N_BIN: {
			Val a, b;
			if (!Eval(x.a, a) || !Eval(x.b, b)) return false;
			if (x.op == O_EQ || x.op == O_NE || x.op == O_LT || x.op == O_LE || x.op == O_GT || x.op == O_GE)
				return Compare(x.op, a, b, out);
			return Arith(x.op, a, b, out); }
		case N_AND:
			if (!Eval(x.a, out)) return false;
			return IsFalse(out) ? true : Eval(x.b, out);
		case N_OR:
			if (!Eval(x.a, out)) return false;
			return !IsFalse(out) ? true : Eval(x.b, out);
		case N_TERNARY: {
			Val c;
			if (!Eval(x.a, c)) return false;
			return Eval(IsFalse(c) ? x.c : x.b, out); }
		case N_ASSIGN: {
			Val v;
			if (x.op == O_ASSIGN) {
				if (!Eval(x.b, v)) return false;
			} else {
				Val cur, rhs;
				if (!Eval(x.a, cur) || !Eval(x.b, rhs)) return false;
				const uint8_t op = x.op == O_PLUSEQ ? O_PLUS : x.op == O_MINUSEQ ? O_MINUS : x.op == O_STAREQ ? O_STAR
					: x.op == O_SLASHEQ ? O_SLASH : O_PERCENT;
				if (!Arith(op, cur, rhs, v)) return false;
			}
			if (!Store(x.a, v)) return false;
			out = v;
			return true; }
		case N_INCDEC: {
			Val cur, v;
			if (!Eval(x.a, cur)) return false;
			if (!Numeric(cur)) return Abort(cur.t == V_NULL || cur.t == V_BOOL ? R_ERROR : R_UNMODELED);
			if (!Arith(O_PLUS, cur, Int(x.i), v)) return false;
			if (!Store(x.a, v)) return false;
			out = x.op ? v : cur;
			return true; }
		default:
			return Abort(R_UNMODELED);
		}
	}

	int Stmt(int n)
	{
		if (depth_ >= kMaxEvalDepth) { Abort(R_UNMODELED); return F_ABORT; }
		++depth_;
		const int f = StmtNode(n);
		--depth_;
		return f;
	}

	int StmtNode(int n)
	{
		if (!Step()) return F_ABORT;
		const Node& x = pr_.nodes[n];
		switch (x.k) {
		case S_EMPTY: return F_NORMAL;
		case S_BLOCK:
			for (int k = 0; k < x.b; ++k) {
				const int f = Stmt(pr_.lists[(size_t)x.a + k]);
				if (f != F_NORMAL) return f;
			}
			return F_NORMAL;
		case S_EXPR: {
			Val v;
			return Eval(x.a, v) ? F_NORMAL : F_ABORT; }
		case S_LOCAL: {
			Val v;
			if (x.b >= 0 && !Eval(x.b, v)) return F_ABORT;
			sc_.stack[base_ + (size_t)x.a] = v;
			return F_NORMAL; }
		case S_IF: {
			Val c;
			if (!Eval(x.a, c)) return F_ABORT;
			if (!IsFalse(c)) return Stmt(x.b);
			return x.c >= 0 ? Stmt(x.c) : F_NORMAL; }
		case S_WHILE:
			for (;;) {
				Val c;
				if (!Eval(x.a, c)) return F_ABORT;
				if (IsFalse(c)) return F_NORMAL;
				const int f = Stmt(x.b);
				if (f == F_BREAK) return F_NORMAL;
				if (f == F_RETURN || f == F_ABORT) return f;
			}
		case S_FOR: {
			if (x.a >= 0) {
				const int f = Stmt(x.a);
				if (f != F_NORMAL) return f;
			}
			for (;;) {
				if (x.b >= 0) {
					Val c;
					if (!Eval(x.b, c)) return F_ABORT;
					if (IsFalse(c)) return F_NORMAL;
				}
				const int f = Stmt(x.d);
				if (f == F_BREAK) return F_NORMAL;
				if (f == F_RETURN || f == F_ABORT) return f;
				if (x.c >= 0) {
					Val v;
					if (!Eval(x.c, v)) return F_ABORT;
				} else if (!Step()) {
					return F_ABORT;
				}
			} }
		case S_RETURN:
			ret_ = Val();
			if (x.a >= 0 && !Eval(x.a, ret_)) return F_ABORT;
			return F_RETURN;
		case S_BREAK:    return F_BREAK;
		case S_CONTINUE: return F_CONTINUE;
		default:
			Abort(R_UNMODELED);
			return F_ABORT;
		}
	}
};

// R_OK and R_ERROR (the game stops the call at a script error) leave `pl` as the game
// would. On R_UNMODELED `pl` must be thrown away.
inline int Run(const Program& prog, int fi, Player& pl, Scratch& sc, int maxSteps = kMaxSteps, int* stepsUsed = nullptr)
{
	// Argument pointers into the stack must survive pushes.
	if (sc.stack.capacity() < (size_t)kMaxStack || sc.heap.capacity() < (size_t)kMaxHeap) sc.Reserve();
	sc.stack.clear();
	sc.heap.clear();
	Exec ex(prog, pl, sc, maxSteps < kMaxSteps ? maxSteps : kMaxSteps);
	Val out;
	ex.Call(fi, nullptr, 0, out);
	if (stepsUsed) *stepsUsed = ex.steps();
	return ex.status;
}

}
