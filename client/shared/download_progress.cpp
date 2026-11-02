#include "download_progress.h"
#include "sigs.h"
#include "utils.h"

#include <cwchar>

namespace dlprogress {

UpdateProgressBarFn    oUpdateProgressBar    = nullptr;
PersistToDiskFn        oPersistToDisk        = nullptr;
DecompressBZipToDiskFn oDecompressBZipToDisk = nullptr;
BZ2_bzreadFn           oBZ2_bzread           = nullptr;

static int  g_bytesCurrent = 0;
static int  g_bytesTotal   = 0;
static bool g_showBytes    = false;
static long long g_bz2BytesTotal = 0;
static double    g_bz2LastBar    = 0.0;

static void ProgressBar(const wchar_t* desc, float progress)
{
	g_bytesCurrent = g_bytesTotal = 0;
	g_showBytes = false;
	Draw(desc, progress);
}

void Hooked_UpdateProgressBar(void* self)
{
	char* rc = *(char**)((char*)self + OFF_CDownloadManager_m_activeRequest);
	if (rc && rc[OFF_RequestContext_bAsHTTP]) {
		g_bytesCurrent = *(int*)(rc + OFF_RequestContext_nBytesCurrent);
		g_bytesTotal   = *(int*)(rc + OFF_RequestContext_nBytesTotal);
		g_showBytes    = true;
	}
	oUpdateProgressBar(self);
}

const wchar_t* Describe(const wchar_t* desc, float& progress, wchar_t (&buf)[256])
{
	if (g_showBytes && desc) {
		const wchar_t prefix[] = L"Downloading ";
		const size_t prefix_len = sizeof(prefix) / sizeof(prefix[0]) - 1;
		if (wcsncmp(desc, prefix, prefix_len) == 0) desc += prefix_len;
		if (wcsncmp(desc, L"maps/", 5) == 0) desc += 5;
		int n = swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"DL %ls (%dM/%dM)", desc,
			g_bytesCurrent / 1024 / 1024, g_bytesTotal / 1024 / 1024);
		if (n >= 0) desc = buf;
		if (g_bytesTotal > 0) {
			float p = (float)g_bytesCurrent / (float)g_bytesTotal;
			progress = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
		}
	}
	g_bytesCurrent = g_bytesTotal = 0;
	g_showBytes = false;
	return desc;
}

void Hooked_PersistToDisk(void* self, void* rc)
{
	ProgressBar(L"Writing to disk...", 0.0f);
	oPersistToDisk(self, rc);
	ProgressBar(L"Done...", 1.0f);
}

bool Hooked_DecompressBZipToDisk(const char* out, const char* src, char* data, int bytes)
{
	ProgressBar(L"Decompressing bz2 to disk...", 0.0f);
	g_bz2BytesTotal = 0;
	g_bz2LastBar = MonotonicSeconds();
	return oDecompressBZipToDisk(out, src, data, bytes);
}

int Hooked_BZ2_bzread(void* bzfile, void* buf, int len)
{
	int n = oBZ2_bzread(bzfile, buf, len);
	if (n > 0) {
		g_bz2BytesTotal += n;
		// Each update re-renders VGUI, so cap it at 10 per second.
		const double now = MonotonicSeconds();
		if (now - g_bz2LastBar >= 0.1) {
			g_bz2LastBar = now;
			wchar_t msg[256];
			if (swprintf(msg, sizeof(msg) / sizeof(msg[0]), L"Bytes uncompressed and written: %lldM",
					g_bz2BytesTotal / 1024 / 1024) >= 0)
				ProgressBar(msg, 0.0f);
		}
	} else if (n == 0) {
		ProgressBar(L"Done...", 1.0f);
	} else {
		ProgressBar(L"bz2 error", 0.0f);
	}
	return n;
}

}
