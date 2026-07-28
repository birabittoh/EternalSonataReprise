/* Eternal Sonata (Xbox 360) asset decoder - C port of scripts/unpack_e.py.
 *
 * Reverse-engineered from default.xex:
 *   sub_8210DFA8  init (256-byte frequency table + 4 code bytes)
 *   sub_8210E0F8  range-coder symbol decode   (TOC flag bit 1)
 *   sub_8210E260  LZSS layer                  (TOC flag bit 0)
 *
 * Usage: unpack_e <assets_dir> <dest_dir> [suffix]
 *   Decodes every index.vmtoc entry whose name ends with `suffix` (default ".e").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

typedef unsigned char u8;
typedef unsigned int u32;

typedef struct {
    const u8 *d;
    size_t n, p;
    int coded;          /* range-coded? */
    u32 low, range, code;
    u8 freq[256];
    unsigned short cum[257];
    u8 *lut;
    unsigned total;
} src_t;

static int src_byte(src_t *s) { return s->p < s->n ? s->d[s->p++] : -1; }

static int src_init(src_t *s, const u8 *d, size_t n, int coded)
{
    memset(s, 0, sizeof(*s));
    s->d = d; s->n = n; s->coded = coded;
    if (!coded) return 0;
    if (n < 260) return -1;
    memcpy(s->freq, d, 256);
    s->p = 256;
    for (int i = 0; i < 256; i++) s->cum[i + 1] = (unsigned short)(s->cum[i] + s->freq[i]);
    s->total = s->cum[256];
    if (!s->total) return -1;
    s->lut = (u8 *)malloc(s->total);
    if (!s->lut) return -1;
    for (unsigned v = 0, sym = 0; sym < 256; sym++)
        while (v < s->cum[sym + 1]) s->lut[v++] = (u8)sym;
    s->low = 0; s->range = 0xFFFFFFFFu; s->code = 0;
    for (int i = 0; i < 4; i++) s->code = (s->code << 8) | (u32)src_byte(s);
    return 0;
}

static int src_get(src_t *s)
{
    int b;
    if (!s->coded) return src_byte(s);
    while (((s->low + s->range) ^ s->low) < 0x1000000u) {
        if ((b = src_byte(s)) < 0) return -1;
        s->low <<= 8; s->range <<= 8;
        s->code = (s->code << 8) | (u32)b;
    }
    while (s->range < 0x2000u) {
        u32 old = s->low;
        if ((b = src_byte(s)) < 0) return -1;
        s->low = old << 8;
        s->range = (0u - (old << 8)) & 0x1FFF00u;
        s->code = (s->code << 8) | (u32)b;
    }
    u32 r = s->range / s->total;
    u8 sym = s->lut[(s->code - s->low) / r];
    s->low += (u32)s->cum[sym] * r;
    s->range = (u32)s->freq[sym] * r;
    return sym;
}

/* Returns bytes produced. `out` must hold out_size bytes. */
static size_t unpack(const u8 *d, size_t n, u8 *out, size_t out_size, int flag)
{
    src_t s;
    size_t o = 0;
    if (src_init(&s, d, n, flag & 2) != 0) return 0;

    if (!(flag & 1)) {
        while (o < out_size) {
            int v = src_get(&s);
            if (v < 0) break;
            out[o++] = (u8)v;
        }
    } else {
        u8 ring[4096];
        unsigned pos = 4078, mask = 0;
        int state = 0, flagbyte = 0, matchlo = 0;
        memset(ring, 0, sizeof(ring));
        while (o < out_size) {
            int v = src_get(&s);
            if (v < 0) break;
            if (state == 0) {
                flagbyte = v; mask = 1;
                state = (v & 1) ? 1 : 2;
                continue;
            }
            if (state == 1) {
                out[o++] = (u8)v;
                ring[pos] = (u8)v;
                pos = (pos + 1) & 0xFFF;
            } else if (state == 2) {
                matchlo = v; state = 3;
                continue;
            } else {
                int len = (v & 0xF) + 3;
                unsigned off = (unsigned)(((v << 4) & 0xF00) | matchlo) & 0xFFF;
                for (int i = 0; i < len && o < out_size; i++) {
                    u8 c = ring[off];
                    off = (off + 1) & 0xFFF;
                    out[o++] = c;
                    ring[pos] = c;
                    pos = (pos + 1) & 0xFFF;
                }
            }
            mask = (mask << 1) & 0xFF;
            state = mask ? ((flagbyte & mask) ? 1 : 2) : 0;
        }
    }
    free(s.lut);
    return o;
}

static u32 rd_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void mkdirs(char *path)
{
    for (char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
#ifdef _WIN32
            _mkdir(path);
#else
            mkdir(path, 0777);
#endif
            *p = c;
        }
    }
}

static u8 *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *b = (u8 *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <assets_dir> <dest_dir> [suffix]\n", argv[0]);
        return 2;
    }
    const char *assets = argv[1], *dest = argv[2];
    const char *suffix = argc > 3 ? argv[3] : ".e";
    size_t sufn = strlen(suffix);

    char path[1024];
    snprintf(path, sizeof(path), "%s/index.vmtoc", assets);
    size_t tocn = 0;
    u8 *toc = slurp(path, &tocn);
    if (!toc) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    size_t nent = tocn / 48, ok = 0, bad = 0, seen = 0;
    unsigned long long total = 0;
    for (size_t i = 0; i < nent; i++) {
        const u8 *e = toc + i * 48;
        char name[33];
        memcpy(name, e, 32); name[32] = 0;
        size_t nl = strlen(name);
        if (!nl || nl < sufn || strcmp(name + nl - sufn, suffix) != 0) continue;
        seen++;
        u32 size = rd_be32(e + 32);
        int flag = e[36];

        snprintf(path, sizeof(path), "%s/%s", assets, name);
        size_t sn = 0;
        u8 *sd = slurp(path, &sn);
        if (!sd) { printf("MISSING  %s\n", name); bad++; continue; }

        u8 *out = (u8 *)malloc(size ? size : 1);
        size_t got = unpack(sd, sn, out, size, flag);
        free(sd);

        snprintf(path, sizeof(path), "%s/%s", dest, name);
        mkdirs(path);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(out, 1, got, f); fclose(f); }
        free(out);

        if (got == size) { ok++; total += got; }
        else { bad++; printf("SHORT    %s flag=%d expected=%u got=%zu\n", name, flag, size, got); }
    }
    printf("%zu/%zu decoded to full size (%zu bad), %.2f GB -> %s\n",
           ok, seen, bad, (double)total / 1e9, dest);
    free(toc);
    return bad ? 1 : 0;
}
