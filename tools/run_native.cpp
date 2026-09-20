// Runs the module natively, with the TRACE host functions stubbed, so the game
// can be tested without Windows, TERMinator or a BBS.
//
// The module is the same code that gets compiled to wasm; only the host side is
// faked. Assets come from real files instead of TERMinator's cache, frames are
// written out as .ppm instead of being presented, and audio is counted rather
// than played.
//
//   run_native <dir with sky.dsk, sky.cpt, sky.dnr> <seconds> [outdir]
//
// This is the Micropolis render_preview.c trick applied to a much bigger module.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <map>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

extern "C" {

// ---- what the module imports from TERMinator -------------------------------

static std::map<std::string, std::vector<unsigned char> > g_assets;
static unsigned g_frames = 0;
static long g_audioFrames = 0;
static int g_tickHz = 0;
static bool g_quit = false;
static std::string g_outDir;
static unsigned g_shotEvery = 0;
static struct timespec g_start;

static long nowMs() {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - g_start.tv_sec) * 1000L + (t.tv_nsec - g_start.tv_nsec) / 1000000L;
}

void trace_present(const unsigned int *pixels, int width, int height, int flags) {
	(void)flags;
	g_frames++;
	if (!g_shotEvery || (g_frames % g_shotEvery) != 0 || g_outDir.empty()) return;
	char path[512];
	snprintf(path, sizeof path, "%s/frame%05u.ppm", g_outDir.c_str(), g_frames);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", width, height);
	for (int i = 0; i < width * height; i++) {
		unsigned int p = pixels[i];
		unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

int  trace_input_pending(void) { return 0; }
int  trace_cpu_count(void) { return 1; }
void trace_frame_capacity(int *w, int *h) { *w = 3840; *h = 2400; }
void trace_log(const char *text, int length) {
	fprintf(stderr, "[module] %.*s\n", length, text);
}
void trace_quit(int code) {
	fprintf(stderr, "[module] trace_quit(%d) after %u frames\n", code, g_frames);
	g_quit = true;
}
int  trace_time_ms(void) { return (int)nowMs(); }
void trace_set_tick(int hz) { g_tickHz = hz; }
void trace_text_input(int on) { (void)on; }
void trace_mouse_mode(int mode) { (void)mode; }
void trace_pad_rumble(int a, int b, int c, int d) { (void)a;(void)b;(void)c;(void)d; }
int  trace_send(const void *d, int n) { (void)d; return n; }
int  trace_send_room(void) { return 4096; }
int  trace_audio_write(const short *frames, int n) { (void)frames; g_audioFrames += n; return n; }
int  trace_audio_room(void) { return 4096; }
int  trace_store_read(void *b, int n) { (void)b; (void)n; return 0; }
int  trace_store_write(const void *d, int n) { (void)d; (void)n; return 0; }

int trace_asset_size(const char *sha) {
	std::map<std::string, std::vector<unsigned char> >::iterator it = g_assets.find(sha);
	return it == g_assets.end() ? 0 : (int)it->second.size();
}

static long g_reads = 0, g_readBytes = 0;
int trace_asset_read(const char *sha, int off, void *buf, int len) {
	g_reads++; g_readBytes += len;
	std::map<std::string, std::vector<unsigned char> >::iterator it = g_assets.find(sha);
	if (it == g_assets.end()) return 0;
	const std::vector<unsigned char> &d = it->second;
	if (off < 0 || (size_t)off >= d.size()) return 0;
	if ((size_t)(off + len) > d.size()) len = (int)(d.size() - off);
	memcpy(buf, &d[off], len);
	return len;
}

// ---- what the module exports ----------------------------------------------
int  trace_init(void);
void trace_on_data(const char *data, int length);
void trace_on_input(int type, int flags, int a, int b, int c);
void trace_update(void);

} // extern "C"

static bool slurp(const std::string &path, std::vector<unsigned char> &out) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(n);
	bool ok = n == 0 || fread(&out[0], 1, n, f) == (size_t)n;
	fclose(f);
	return ok;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <game dir> <seconds> [outdir] [shot every N frames]\n", argv[0]);
		return 2;
	}
	const std::string dir = argv[1];
	const int seconds = atoi(argv[2]);
	if (argc > 3) { g_outDir = argv[3]; mkdir(g_outDir.c_str(), 0755); }
	g_shotEvery = argc > 4 ? (unsigned)atoi(argv[4]) : 25;

	clock_gettime(CLOCK_MONOTONIC, &g_start);

	// Stand in for TERMinator's asset cache. The "hashes" are just the names
	// here; the module only ever passes them back to us.
	const char *names[] = { "sky.dsk", "sky.cpt", "sky.dnr", NULL };
	std::string files;
	for (int i = 0; names[i]; i++) {
		std::vector<unsigned char> d;
		if (!slurp(dir + "/" + names[i], d)) {
			fprintf(stderr, "missing %s/%s\n", dir.c_str(), names[i]);
			return 1;
		}
		fprintf(stderr, "asset %-9s %10zu bytes\n", names[i], d.size());
		g_assets[names[i]] = d;
		if (i) files += " ";
		files += std::string(names[i]) + "=" + names[i];
	}

	if (trace_init() != 0) { fprintf(stderr, "trace_init failed\n"); return 1; }

	const std::string msg = "files " + files;
	trace_on_data(msg.c_str(), (int)msg.size());

	// Drive it the way the host would: tick, and let the game thread run.
	const long deadline = seconds * 1000L;
	const long step = g_tickHz > 0 ? 1000 / g_tickHz : 20;
	while (!g_quit && nowMs() < deadline) {
		trace_update();
		usleep(step * 1000);
	}

	fprintf(stderr, "\n--- %ld ms: %u frames presented, %ld audio frames written ---\n",
	        nowMs(), g_frames, g_audioFrames);
	fprintf(stderr, "--- asset reads: %ld calls, %.1f MB, average %ld bytes a call ---\n",
	        g_reads, g_readBytes / 1048576.0, g_reads ? g_readBytes / g_reads : 0);
	return g_frames > 0 ? 0 : 1;
}
