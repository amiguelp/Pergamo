/*
 * Páginas "reales" de un EPUB, sin depender del tamaño de letra del lector:
 *  1. Marcas de página de la edición impresa que incluyen algunos EPUB
 *     (EPUB 3: <nav epub:type="page-list">; EPUB 2: <pageList> en toc.ncx).
 *  2. Si no las hay, el número de caracteres del texto, para estimar
 *     "páginas estándar" (EPUB_CHARS_PER_PAGE caracteres por página).
 * Lector de ZIP propio; zlib se carga con dlopen.
 */
#include "epubpages.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#define MAX_EPUB_SIZE (80L * 1024 * 1024)
#define MAX_ENTRY_SIZE (40UL * 1024 * 1024)

/* --- zlib en tiempo de ejecución ---------------------------------------- */

static void *z_handle = NULL;
static int (*z_inflateInit2)(z_streamp, int, const char *, int);
static int (*z_inflate)(z_streamp, int);
static int (*z_inflateEnd)(z_streamp);

static int LoadZlib(void) {
    if (z_handle) return 0;
    const char *names[] = {"libz.so.1", "libz.so"};
    for (size_t i = 0; i < 2 && !z_handle; i++) z_handle = dlopen(names[i], RTLD_NOW);
    if (!z_handle) return -1;
    *(void **)&z_inflateInit2 = dlsym(z_handle, "inflateInit2_");
    *(void **)&z_inflate = dlsym(z_handle, "inflate");
    *(void **)&z_inflateEnd = dlsym(z_handle, "inflateEnd");
    if (!z_inflateInit2 || !z_inflate || !z_inflateEnd) {
        dlclose(z_handle);
        z_handle = NULL;
        return -1;
    }
    return 0;
}

/* --- ZIP ----------------------------------------------------------------- */

typedef struct {
    unsigned char *data;
    long size;
} Zip;

static uint32_t U32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t U16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int ZipOpen(const char *path, Zip *zip) {
    zip->data = NULL;
    zip->size = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 22 || size > MAX_EPUB_SIZE) { fclose(f); return -1; }
    rewind(f);
    zip->data = malloc(size);
    if (!zip->data || fread(zip->data, 1, size, f) != (size_t)size) {
        fclose(f);
        free(zip->data);
        zip->data = NULL;
        return -1;
    }
    fclose(f);
    zip->size = size;
    return 0;
}

static void ZipClose(Zip *zip) {
    free(zip->data);
    zip->data = NULL;
}

/* Recorre el directorio central. callback devuelve 1 para parar. */
typedef int (*ZipVisit)(const char *name, size_t name_len, uint16_t method, uint32_t csize,
                        uint32_t usize, uint32_t local_offset, void *ctx);

static int ZipEach(const Zip *zip, ZipVisit visit, void *ctx) {
    long start = zip->size - 22;
    long stop = zip->size - 22 - 65535;
    if (stop < 0) stop = 0;
    long eocd = -1;
    for (long i = start; i >= stop; i--) {
        if (U32(zip->data + i) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) return -1;
    uint16_t entries = U16(zip->data + eocd + 10);
    uint32_t cd = U32(zip->data + eocd + 16);
    long p = cd;
    for (uint16_t n = 0; n < entries; n++) {
        if (p + 46 > zip->size || U32(zip->data + p) != 0x02014b50) return -1;
        uint16_t method = U16(zip->data + p + 10);
        uint32_t csize = U32(zip->data + p + 20);
        uint32_t usize = U32(zip->data + p + 24);
        uint16_t nlen = U16(zip->data + p + 28), elen = U16(zip->data + p + 30), clen = U16(zip->data + p + 32);
        uint32_t local = U32(zip->data + p + 42);
        if (p + 46 + nlen > zip->size) return -1;
        if (visit((const char *)zip->data + p + 46, nlen, method, csize, usize, local, ctx)) return 0;
        p += 46 + nlen + elen + clen;
    }
    return 0;
}

typedef struct {
    const char *want;
    unsigned char *out;
    size_t out_size;
    const Zip *zip;
    int found;
} ReadCtx;

static int ReadVisit(const char *name, size_t nlen, uint16_t method, uint32_t csize, uint32_t usize,
                     uint32_t local, void *vctx) {
    ReadCtx *c = vctx;
    if (strlen(c->want) != nlen || strncmp(name, c->want, nlen) != 0) return 0;
    c->found = -1;
    const Zip *zip = c->zip;
    if ((long)local + 30 > zip->size || U32(zip->data + local) != 0x04034b50) return 1;
    long data = local + 30 + U16(zip->data + local + 26) + U16(zip->data + local + 28);
    if (data + (long)csize > zip->size || usize > MAX_ENTRY_SIZE) return 1;
    unsigned char *out = malloc(usize + 1);
    if (!out) return 1;
    if (method == 0) {
        memcpy(out, zip->data + data, usize);
    } else if (method == 8) {
        if (LoadZlib() != 0) { free(out); return 1; }
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (z_inflateInit2(&zs, -MAX_WBITS, ZLIB_VERSION, (int)sizeof(z_stream)) != Z_OK) { free(out); return 1; }
        zs.next_in = zip->data + data;
        zs.avail_in = csize;
        zs.next_out = out;
        zs.avail_out = usize;
        int r = z_inflate(&zs, Z_FINISH);
        z_inflateEnd(&zs);
        if (r != Z_STREAM_END && !(r == Z_OK && zs.avail_out == 0)) { free(out); return 1; }
    } else {
        free(out);
        return 1;
    }
    out[usize] = '\0';
    c->out = out;
    c->out_size = usize;
    c->found = 1;
    return 1;
}

/* Devuelve el contenido (terminado en '\0') o NULL. */
static unsigned char *ZipRead(const Zip *zip, const char *name, size_t *size) {
    ReadCtx c = {name, NULL, 0, zip, 0};
    if (ZipEach(zip, ReadVisit, &c) != 0 || c.found != 1) return NULL;
    if (size) *size = c.out_size;
    return c.out;
}

/* --- XML ----------------------------------------------------------------- */

static xmlDocPtr ParseXml(const unsigned char *data, size_t size) {
    return xmlReadMemory((const char *)data, (int)size, NULL, NULL,
                         XML_PARSE_RECOVER | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET);
}

static const char *LocalName(xmlNodePtr n) { return n && n->name ? (const char *)n->name : ""; }

static char *Attr(xmlNodePtr n, const char *name) {
    for (xmlAttrPtr a = n->properties; a; a = a->next) {
        if (a->name && strcmp((const char *)a->name, name) == 0) {
            return (char *)xmlNodeGetContent((xmlNodePtr)a);
        }
    }
    return NULL;
}

/* Ruta de un href relativa al directorio del OPF, sin "%20" ni "#ancla". */
static void ResolveHref(const char *base_dir, const char *href, char *out, size_t out_size) {
    char decoded[1024];
    size_t o = 0;
    for (const char *p = href; *p && *p != '#' && o + 1 < sizeof(decoded); p++) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            decoded[o++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            decoded[o++] = *p;
        }
    }
    decoded[o] = '\0';
    char joined[2048];
    snprintf(joined, sizeof(joined), "%s%s", base_dir, decoded);
    /* normalizar "dir/../x" */
    char *parts[256];
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(joined, "/", &save); t && n < 256; t = strtok_r(NULL, "/", &save)) {
        if (strcmp(t, ".") == 0) continue;
        if (strcmp(t, "..") == 0) { if (n > 0) n--; continue; }
        parts[n++] = t;
    }
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        strncat(out, parts[i], out_size - strlen(out) - 1);
        if (i + 1 < n) strncat(out, "/", out_size - strlen(out) - 1);
    }
}

/* Páginas según una lista de marcas: el mayor número arábigo de las
   etiquetas, o el número de marcas si no son números. */
typedef struct { int count; int max_label; } PageMarks;

static void AddMark(PageMarks *m, const char *label) {
    m->count++;
    if (!label) return;
    while (isspace((unsigned char)*label)) label++;
    if (isdigit((unsigned char)*label)) {
        int v = atoi(label);
        if (v > m->max_label && v < 100000) m->max_label = v;
    }
}

static int MarksToPages(const PageMarks *m) {
    if (m->count == 0) return 0;
    return m->max_label > 0 ? m->max_label : m->count;
}

static void WalkNavPageList(xmlNodePtr node, int inside, PageMarks *marks) {
    for (xmlNodePtr n = node; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) continue;
        int here = inside;
        if (strcmp(LocalName(n), "nav") == 0) {
            char *type = Attr(n, "type");
            if (type && strstr(type, "page-list")) here = 1;
            if (type) xmlFree(type);
        }
        if (here && strcmp(LocalName(n), "a") == 0) {
            xmlChar *text = xmlNodeGetContent(n);
            AddMark(marks, (const char *)text);
            if (text) xmlFree(text);
        }
        WalkNavPageList(n->children, here, marks);
    }
}

static void WalkNcxPageList(xmlNodePtr node, PageMarks *marks) {
    for (xmlNodePtr n = node; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) continue;
        if (strcmp(LocalName(n), "pageTarget") == 0) {
            char *value = Attr(n, "value");
            AddMark(marks, value);
            if (value) xmlFree(value);
            continue;
        }
        WalkNcxPageList(n->children, marks);
    }
}

static const char *FindNoCase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) if (strncasecmp(hay, needle, n) == 0) return hay;
    return NULL;
}

/* Caracteres visibles del <body> de un XHTML (espacios agrupados). */
static long CountTextChars(const char *html) {
    const char *p = FindNoCase(html, "<body");
    if (!p) p = html;
    long count = 0;
    int space = 1;
    while (*p) {
        if (*p == '<') {
            if (strncasecmp(p, "<script", 7) == 0 || strncasecmp(p, "<style", 6) == 0) {
                const char *end = strncasecmp(p, "<script", 7) == 0 ? "</script" : "</style";
                const char *e = FindNoCase(p, end);
                if (!e) break;
                p = e;
            }
            const char *gt = strchr(p, '>');
            if (!gt) break;
            p = gt + 1;
            continue;
        }
        unsigned char c = (unsigned char)*p;
        if (c == '&') {
            const char *semi = strchr(p, ';');
            if (semi && semi - p < 10) p = semi;
            count++;
            space = 0;
        } else if (isspace(c)) {
            if (!space) { count++; space = 1; }
        } else if ((c & 0xC0) != 0x80) {
            count++;
            space = 0;
        }
        p++;
    }
    return count;
}

/* --- API ------------------------------------------------------------------ */

int EpubAnalyze(const char *path, EpubPageInfo *info) {
    memset(info, 0, sizeof(*info));
    Zip zip;
    if (ZipOpen(path, &zip) != 0) return -1;

    size_t size = 0;
    unsigned char *container = ZipRead(&zip, "META-INF/container.xml", &size);
    char opf_path[1024] = {0};
    if (container) {
        const char *fp = strstr((const char *)container, "full-path=\"");
        if (fp) {
            fp += 11;
            const char *e = strchr(fp, '"');
            if (e && (size_t)(e - fp) < sizeof(opf_path)) memcpy(opf_path, fp, e - fp);
        }
        free(container);
    }
    if (!opf_path[0]) { ZipClose(&zip); return -1; }
    unsigned char *opf = ZipRead(&zip, opf_path, &size);
    if (!opf) { ZipClose(&zip); return -1; }

    char base_dir[1024] = {0};
    const char *slash = strrchr(opf_path, '/');
    if (slash) snprintf(base_dir, sizeof(base_dir), "%.*s/", (int)(slash - opf_path), opf_path);

    xmlDocPtr doc = ParseXml(opf, size);
    free(opf);
    if (!doc) { ZipClose(&zip); return -1; }

    /* manifest, spine, identificadores */
    enum { MAX_ITEMS = 2000 };
    static char ids[MAX_ITEMS][128], hrefs[MAX_ITEMS][512];
    static int is_nav[MAX_ITEMS];
    int item_count = 0;
    static char spine_ids[MAX_ITEMS][128];
    int spine_count = 0;
    char ncx_id[128] = {0};
    xmlNodePtr root = xmlDocGetRootElement(doc);
    for (xmlNodePtr section = root ? root->children : NULL; section; section = section->next) {
        if (section->type != XML_ELEMENT_NODE) continue;
        const char *sname = LocalName(section);
        for (xmlNodePtr n = section->children; n; n = n->next) {
            if (n->type != XML_ELEMENT_NODE) continue;
            const char *nname = LocalName(n);
            if (strcmp(sname, "manifest") == 0 && strcmp(nname, "item") == 0 && item_count < MAX_ITEMS) {
                char *id = Attr(n, "id"), *href = Attr(n, "href"), *props = Attr(n, "properties");
                if (id && href) {
                    snprintf(ids[item_count], sizeof(ids[0]), "%s", id);
                    ResolveHref(base_dir, href, hrefs[item_count], sizeof(hrefs[0]));
                    is_nav[item_count] = props && strstr(props, "nav") != NULL;
                    item_count++;
                }
                if (id) xmlFree(id);
                if (href) xmlFree(href);
                if (props) xmlFree(props);
            } else if (strcmp(sname, "spine") == 0 && strcmp(nname, "itemref") == 0 && spine_count < MAX_ITEMS) {
                char *idref = Attr(n, "idref");
                if (idref) { snprintf(spine_ids[spine_count++], sizeof(spine_ids[0]), "%s", idref); xmlFree(idref); }
            } else if (strcmp(sname, "metadata") == 0 && strcmp(nname, "identifier") == 0 && !info->isbn[0]) {
                xmlChar *text = xmlNodeGetContent(n);
                if (text) {
                    char digits[32];
                    int d = 0;
                    for (const xmlChar *p = text; *p && d < 31; p++) if (isdigit(*p) || *p == 'X') digits[d++] = (char)*p;
                    digits[d] = '\0';
                    if ((d == 13 && (strncmp(digits, "978", 3) == 0 || strncmp(digits, "979", 3) == 0)) || d == 10)
                        snprintf(info->isbn, sizeof(info->isbn), "%s", digits);
                    xmlFree(text);
                }
            }
        }
        if (strcmp(sname, "spine") == 0) {
            char *toc = Attr(section, "toc");
            if (toc) { snprintf(ncx_id, sizeof(ncx_id), "%s", toc); xmlFree(toc); }
        }
    }
    xmlFreeDoc(doc);

    /* 1. Marcas de página de la edición impresa */
    PageMarks marks = {0, 0};
    for (int i = 0; i < item_count && marks.count == 0; i++) {
        if (!is_nav[i]) continue;
        unsigned char *nav = ZipRead(&zip, hrefs[i], &size);
        if (!nav) continue;
        xmlDocPtr nd = ParseXml(nav, size);
        free(nav);
        if (nd) { WalkNavPageList(xmlDocGetRootElement(nd), 0, &marks); xmlFreeDoc(nd); }
    }
    if (marks.count == 0 && ncx_id[0]) {
        for (int i = 0; i < item_count; i++) {
            if (strcmp(ids[i], ncx_id) != 0) continue;
            unsigned char *ncx = ZipRead(&zip, hrefs[i], &size);
            if (!ncx) break;
            xmlDocPtr nd = ParseXml(ncx, size);
            free(ncx);
            if (nd) { WalkNcxPageList(xmlDocGetRootElement(nd), &marks); xmlFreeDoc(nd); }
            break;
        }
    }
    info->print_pages = MarksToPages(&marks);

    /* 2. Caracteres del texto (para estimar) */
    for (int s = 0; s < spine_count; s++) {
        for (int i = 0; i < item_count; i++) {
            if (strcmp(ids[i], spine_ids[s]) != 0) continue;
            unsigned char *html = ZipRead(&zip, hrefs[i], &size);
            if (html) { info->text_chars += CountTextChars((const char *)html); free(html); }
            break;
        }
    }
    info->estimated_pages = (int)((info->text_chars + EPUB_CHARS_PER_PAGE / 2) / EPUB_CHARS_PER_PAGE);
    ZipClose(&zip);
    return 0;
}
