#pragma once

namespace dlprogress {

typedef void (*UpdateProgressBarFn)(void* self);
typedef void (*PersistToDiskFn)(void* self, void* rc);
typedef bool (*DecompressBZipToDiskFn)(const char* out, const char* src, char* data, int bytes);
typedef int  (*BZ2_bzreadFn)(void* bzfile, void* buf, int len);

extern UpdateProgressBarFn    oUpdateProgressBar;
extern PersistToDiskFn        oPersistToDisk;
extern DecompressBZipToDiskFn oDecompressBZipToDisk;
extern BZ2_bzreadFn           oBZ2_bzread;

void Hooked_UpdateProgressBar(void* self);
void Hooked_PersistToDisk(void* self, void* rc);
bool Hooked_DecompressBZipToDisk(const char* out, const char* src, char* data, int bytes);
int  Hooked_BZ2_bzread(void* bzfile, void* buf, int len);

// For the OS's CEngineVGui::UpdateCustomProgressBar hook: returns the text to draw, and may
// replace progress with the byte ratio.
const wchar_t* Describe(const wchar_t* desc, float& progress, wchar_t (&buf)[256]);

// Per OS: draw through UpdateCustomProgressBar, keeping its GameUI null check.
void Draw(const wchar_t* desc, float progress);

}
