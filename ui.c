#include "opds_app.h"
#include "secrets.h"
#include "settings_store.h"
#include "updater.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <inkview.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <dirent.h>
#include <dlfcn.h>
#include "icons.h" 
#include "uninstall.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Font and image pointers
ifont *font48 = NULL;
ifont *font36 = NULL;
ifont *font30 = NULL;
ifont *font24 = NULL;
ibitmap *current_cover_bmp = NULL;

ibitmap *folder_icon_bmp = NULL;
ibitmap *book_fallback_bmp = NULL;
ibitmap *list_thumbs[MAX_ENTRIES] = {NULL};

// Navigation tracking
#define MAX_NAV_STACK 32
char nav_stack[MAX_NAV_STACK][MAX_STR_LEN];
/* Título de cada nivel guardado, para mostrar la ruta en la cabecera. */
static char nav_title_stack[MAX_NAV_STACK][128];
/* Nombre de la lista actual tal como se eligió (el título de la entrada
   tocada, o "Búsqueda: ..."). Muchos servidores, como Calibre-Web, ponen el
   mismo título a todas sus listas, así que no sirve el título del feed. */
static char current_level_label[128] = {0};
int nav_stack_ptr = 0;

#define MAX_PAGE_STACK 16
char page_stack[MAX_PAGE_STACK][MAX_STR_LEN];
int page_offset_stack[MAX_PAGE_STACK];
int page_stack_ptr = 0;
int current_global_offset = 0;
int return_to_last_page = 0; 
int jump_target_page = -1;
int current_page = 0;

char last_loaded_url[MAX_STR_LEN] = {0};
static char catalog_error[MAX_STR_LEN] = {0};

extern void LogDebug(const char *msg);
extern int is_opensearch_url;
extern int ParseOpenSearch(const char *xml_data, const char *base_url, char *template_out);

static int summary_scroll_y = 0;
static int details_list_scroll = 0;
static int view_x = 0;
static int view_y = 0;
static int view_w = 0;
static int view_h = 0;
static char pending_ebiblio_action[MAX_STR_LEN] = {0};
static int pending_ebiblio_action_uses_delete = 0;
static int pending_ebiblio_action_uses_put = 0;

void SetTextFont(int size, int color);
void DrawButton(int x, int y, int w, int h, const char *text, int is_dark);
void Repaint();
void LoadCatalog(const char *url);
void GoBackInHierarchy();
void FetchThumbnailsForPage();
void ManageImageCache(); 
void JIT_DecodePageThumbnails(); 
static int EbiblioActionCount(const OPDSEntry *entry);
static int EbiblioHasBorrowAction(const OPDSEntry *entry);
static const char *EbiblioActionURL(const OPDSEntry *entry, int index);
static const char *EbiblioActionLabel(const OPDSEntry *entry, int index);
static void BuildBookDetailSummary(const OPDSEntry *entry, char *output,
                                   size_t output_size);
static void ConfirmEbiblioAction(int button);
static void RequestEbiblioAction(const char *url, const char *label,
                                 int use_delete, int use_put);

// --- NEW: Bulletproof Aggressive Dark Mode Detection ---
int IsDarkMode() {
    // 1. Try dynamic loading of ANY firmware 6.8+ inversion check function
    static int checked_dl = 0;
    static int (*dl_funcs[6])() = {NULL};
    
    if (!checked_dl) {
        void *h = dlopen("libinkview.so", RTLD_LAZY);
        if (!h) h = dlopen(NULL, RTLD_LAZY);
        if (h) {
            dl_funcs[0] = dlsym(h, "GetScreenInversionState");
            dl_funcs[1] = dlsym(h, "IvGetScreenInversionState");
            dl_funcs[2] = dlsym(h, "GetDarkMode");
            dl_funcs[3] = dlsym(h, "IsDarkMode");
            dl_funcs[4] = dlsym(h, "GetThemeDark");
            dl_funcs[5] = dlsym(h, "IsThemeDark");
        }
        checked_dl = 1;
    }
    
    for (int i = 0; i < 6; i++) {
        if (dl_funcs[i] && dl_funcs[i]()) {
            return 1;
        }
    }

    // 2. Aggressive Config Hunt - bypass cache and look for any relevant key
    const char* config_paths[] = {
        "/mnt/ext1/system/config/global.cfg",
        "/mnt/ext1/system/profiles/default/config/global.cfg",
        NULL
    };

    for (int p = 0; config_paths[p] != NULL; p++) {
        FILE *f = fopen(config_paths[p], "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                // Look for keys set to 1 or true
                if (strstr(line, "=1\n") || strstr(line, "=1\r") || strstr(line, "=true")) {
                    char lower[256];
                    for(int i = 0; line[i]; i++) lower[i] = tolower(line[i]);
                    
                    // Does it sound like dark mode? (Ignoring "night" to avoid smartlight false positives)
                    if ((strstr(lower, "dark") || strstr(lower, "invert") || strstr(lower, "invers")) && 
                        !strstr(lower, "night")) {
                        
                        char msg[512]; snprintf(msg, sizeof(msg), "HUNT: Detected dark mode via key -> %s", line);
                        LogDebug(msg);
                        fclose(f);
                        return 1;
                    }
                }
            }
            fclose(f);
        }
    }
    
    return 0;
}

void DrawSmartBitmap(int x, int y, int w, int h, ibitmap *bmp, int flags, int preserve_color) {
    if (!bmp) return;
    
    int is_dark = IsDarkMode();
    int buffer_size = bmp->scanline * bmp->height;
    unsigned char *ptr = bmp->data;
    
    // Pre-invert the raw image data in memory so the hardware inversion flips it back to normal
    if (is_dark && preserve_color) {
        for (int i = 0; i < buffer_size; i++) {
            ptr[i] = ~ptr[i]; // Fast bitwise inversion
        }
    }
    
    DrawBitmapRect(x, y, w, h, bmp, flags);
    
    // Restore the image data immediately so the cached cover isn't permanently altered
    if (is_dark && preserve_color) {
        for (int i = 0; i < buffer_size; i++) {
            ptr[i] = ~ptr[i]; // Flip it back
        }
    }
}

// Calculates the requested rows for the active server dynamically
int GetCatalogRows() {
    if (current_server_index >= 0 && current_server_index < server_count) {
        int r = servers[current_server_index].catalog_rows;
        return (r >= 4 && r <= 10) ? r : 10;
    }
    return 10;
}

// --- NEW: Centralized Visual Feedback Helper ---
void FlashArea(int x, int y, int w, int h) {
    InvertArea(x, y, w, h);
    PartialUpdate(x, y, w, h);
    usleep(100000); 
}

void SetTextFont(int size, int color) {
    // Dynamic caching system so we can generate proportionally sized fonts on the fly!
    static int cached_sizes[16] = {0};
    static ifont* cached_fonts[16] = {NULL};
    ifont *f = NULL;
    for (int i = 0; i < 16; i++) {
        if (cached_sizes[i] == size) { f = cached_fonts[i]; break; }
        if (cached_sizes[i] == 0) {
            cached_fonts[i] = OpenFont("default", size, 1);
            cached_sizes[i] = size;
            f = cached_fonts[i];
            break;
        }
    }
    if (f) SetFont(f, color);
}

/* --- Geometría de la vista de catálogo (compartida entre dibujo y toques) ---
   Abajo, de arriba a abajo: texto de página, barra de navegación
   (Atrás | Buscar | Inicio) y fila de paginación. Para salir de la app se
   usa el botón de la pantalla principal. */
#define BROWSE_NAV_BUTTONS 3

static int BrowseFooterY(void) { return sys_height - sys_height / 13 - 10; }
static int BrowseNavH(void) { return sys_height / 13; }
static int BrowseNavY(void) { return BrowseFooterY() - sys_height / 60 - BrowseNavH(); }
static int BrowseListBottom(void) { return BrowseNavY() - 55; }

static int IsCatalogRootURL(const char *url) {
    return url && url[0] && strcmp(url, servers[current_server_index].url) == 0;
}

/* "Inicio" solo tiene sentido si lleva a un sitio distinto del que ya
   estás o del que te llevaría "Atrás". */
static int HomeButtonUseful(void) {
    if (nav_stack_ptr == 0) {
        return !(IsCatalogRootURL(last_loaded_url) && page_stack_ptr == 0 && current_page == 0);
    }
    if (nav_stack_ptr == 1 && IsCatalogRootURL(nav_stack[0])) return 0;
    return 1;
}

static int PreviousPageAvailable(void) {
    return current_page > 0 || page_stack_ptr > 0;
}

static int NextPageAvailable(void) {
    int entries_per_page = GetCatalogRows();
    return (current_page + 1) * entries_per_page < entry_count || strlen(next_page_url) > 0;
}

/* Ruta "Portada > Autores > A". Si no cabe, se recorta por el principio
   ("... > Autores > A"). Usa la fuente activa para medir. */
static void BuildBreadcrumb(char *out, size_t out_size, int max_width) {
    const char *parts[MAX_NAV_STACK + 1];
    int count = 0;
    for (int i = 0; i < nav_stack_ptr && i < MAX_NAV_STACK; i++) {
        parts[count++] = nav_title_stack[i][0] ? nav_title_stack[i] : "...";
    }
    if (current_level_label[0]) parts[count++] = current_level_label;
    else if (nav_stack_ptr == 0) parts[count++] = T("Portada");
    else parts[count++] = current_feed_title[0] ? current_feed_title : last_loaded_url;

    for (int start = 0; start < count; start++) {
        size_t used = 0;
        out[0] = '\0';
        if (start > 0) used += snprintf(out + used, out_size - used, "... > ");
        for (int i = start; i < count && used < out_size; i++) {
            used += snprintf(out + used, out_size - used, "%s%s", parts[i], i + 1 < count ? " > " : "");
        }
        if (start == count - 1 || StringWidth(out) <= max_width) return;
    }
}

static void BrowseNavButtonRect(int index, int *x, int *w) {
    int margin = 20, spacing = 10;
    int bw = (sys_width - margin * 2 - spacing * (BROWSE_NAV_BUTTONS - 1)) / BROWSE_NAV_BUTTONS;
    *x = margin + index * (bw + spacing);
    *w = bw;
}

void DrawButton(int x, int y, int w, int h, const char *text, int style) {
    int bg_color = 0xDDDDDD; 
    int fg_color = 0x000000; 

    int border = 0x000000;
    if (style == 1) { bg_color = 0x555555; fg_color = 0xFFFFFF; } 
    else if (style == 2) { bg_color = 0xCC0000; fg_color = 0xFFFFFF; }
    else if (style == BUTTON_DISABLED) { bg_color = 0xF2F2F2; fg_color = 0xAAAAAA; border = 0xBBBBBB; }

    FillArea(x, y, w, h, bg_color); 
    DrawRect(x, y, w, h, border); 
    SetTextFont(36, fg_color); 
    DrawTextRect(x, y + (h/2) - 20, w, 40, text, ALIGN_CENTER);
}

unsigned long hash_str(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) { hash = ((hash << 5) + hash) + c; }
    return hash;
}

ibitmap* LoadCoverSTB_Memory(const unsigned char *buffer, int len) {
    int width, height, channels;
    unsigned char *img = stbi_load_from_memory(buffer, len, &width, &height, &channels, 3);
    if (!img) return NULL;

    int scanline = (width * 3 + 3) & ~3; 
    int data_size = scanline * height;

    ibitmap *bm = (ibitmap *)malloc(sizeof(ibitmap) + data_size);
    if (!bm) { stbi_image_free(img); return NULL; }

    bm->width = width; 
    bm->height = height; 
    bm->depth = 24; 
    bm->scanline = scanline;

    unsigned char *dst = (unsigned char *)bm->data;
    for (int y = 0; y < height; y++) {
        unsigned char *src_row = img + (y * width * 3);
        unsigned char *dst_row = dst + (y * scanline);
        for (int x = 0; x < width; x++) {
            dst_row[x * 3 + 0] = src_row[x * 3 + 0]; 
            dst_row[x * 3 + 1] = src_row[x * 3 + 1]; 
            dst_row[x * 3 + 2] = src_row[x * 3 + 2]; 
        }
    }
    stbi_image_free(img);
    return bm;
}

ibitmap* LoadCoverSTB(const char *path) {
    int width, height, channels;
    unsigned char *img = stbi_load(path, &width, &height, &channels, 3);
    if (!img) return NULL;

    int scanline = (width * 3 + 3) & ~3; 
    int data_size = scanline * height;

    ibitmap *bm = (ibitmap *)malloc(sizeof(ibitmap) + data_size);
    if (!bm) { stbi_image_free(img); return NULL; }

    bm->width = width; 
    bm->height = height; 
    bm->depth = 24; 
    bm->scanline = scanline;

    unsigned char *dst = (unsigned char *)bm->data;
    for (int y = 0; y < height; y++) {
        unsigned char *src_row = img + (y * width * 3);
        unsigned char *dst_row = dst + (y * scanline);
        for (int x = 0; x < width; x++) {
            dst_row[x * 3 + 0] = src_row[x * 3 + 0]; 
            dst_row[x * 3 + 1] = src_row[x * 3 + 1]; 
            dst_row[x * 3 + 2] = src_row[x * 3 + 2]; 
        }
    }
    stbi_image_free(img);
    return bm;
}

void ShowDownloadProgress(long long total, long long current) {
    static int last_percent = -1;
    int percent = (total > 0) ? (int)((current * 100) / total) : 0;
    if (percent == last_percent) return;
    last_percent = percent;

    int bw = sys_width * 0.8;
    int bh = 60;
    int bx = (sys_width - bw) / 2;
    int by = (sys_height / 2) - 60;

    FillArea(bx - 20, by - 80, bw + 40, 200, 0xFFFFFF);
    DrawRect(bx - 20, by - 80, bw + 40, 200, 0x000000);

    SetTextFont(36, 0x000000);
    char buf[64]; snprintf(buf, sizeof(buf), T("Descargando: %d%%"), percent);
    DrawTextRect(bx, by - 50, bw, 40, buf, ALIGN_CENTER);
    
    DrawRect(bx, by + 10, bw, bh, 0x000000);
    int fill_w = (int)((bw - 4) * percent / 100);
    if (fill_w > 0) FillArea(bx + 2, by + 12, fill_w, bh - 4, 0x555555);

    PartialUpdate(bx - 30, by - 90, bw + 60, 220);
}

int CheckDownloadCancel() { return 0; }

int EnsureNetwork() { 
    if (NetConnect(NULL) != 0) {
        Message(ICON_ERROR, T("Sin conexión"), T("Active el Wi-Fi para conectarse."), 3000);
        return -1; 
    }
    return 0; 
}

void EnsureDirectories() {
    mkdir(APP_ROOT_DIR, 0777);
    mkdir(IMAGES_DIR, 0777);
    mkdir(BOOKS_DIR, 0777); 
}

void FreeListThumbs() {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (list_thumbs[i]) { free(list_thumbs[i]); list_thumbs[i] = NULL; }
    }
}

// --- Image Cache Management ---
#define MAX_CACHE_BYTES 20971520 
#define TARGET_CACHE_BYTES 15728640 

typedef struct {
    char path[256];
    time_t mtime;
    off_t size;
} CacheEntry;

int compare_cache_entries(const void *a, const void *b) {
    CacheEntry *ea = (CacheEntry *)a;
    CacheEntry *eb = (CacheEntry *)b;
    if (ea->mtime < eb->mtime) return -1;
    if (ea->mtime > eb->mtime) return 1;
    return 0;
}

void ManageImageCache() {
    DIR *dir = opendir(IMAGES_DIR);
    if (!dir) return;

    struct dirent *ent;
    int capacity = 100;
    int count = 0;
    CacheEntry *entries = malloc(capacity * sizeof(CacheEntry));
    if (!entries) { closedir(dir); return; }

    long long total_size = 0;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "cache_", 6) == 0) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s%s", IMAGES_DIR, ent->d_name);
            
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
                if (count >= capacity) {
                    capacity *= 2;
                    CacheEntry *tmp = realloc(entries, capacity * sizeof(CacheEntry));
                    if (!tmp) break; 
                    entries = tmp;
                }
                strncpy(entries[count].path, full_path, 255);
                entries[count].path[255] = '\0';
                entries[count].mtime = st.st_mtime;
                entries[count].size = st.st_size;
                total_size += st.st_size;
                count++;
            }
        }
    }
    closedir(dir);

    if (total_size > MAX_CACHE_BYTES) {
        LogDebug("Cache Janitor: 20MB Threshold Reached. Cleaning oldest images...");
        qsort(entries, count, sizeof(CacheEntry), compare_cache_entries);
        
        for (int i = 0; i < count; i++) {
            if (remove(entries[i].path) == 0) {
                total_size -= entries[i].size;
                if (total_size <= TARGET_CACHE_BYTES) {
                    break;
                }
            }
        }
    }
    free(entries);
}

void LoadStockIcons() {
    if (!folder_icon_bmp) folder_icon_bmp = LoadCoverSTB_Memory(folder_icon_png, folder_icon_png_len);
    if (!book_fallback_bmp) book_fallback_bmp = LoadCoverSTB_Memory(book_icon_png, book_icon_png_len);
}

void UnloadGlobalCover() { 
    if (current_cover_bmp) { free(current_cover_bmp); current_cover_bmp = NULL; } 
}

/* Guarda el nivel actual antes de entrar en otro (o en una búsqueda);
   new_label es el nombre con el que se mostrará el nivel nuevo. */
static void PushNavigationLevel(const char *new_label) {
    if (nav_stack_ptr < MAX_NAV_STACK) {
        strncpy(nav_stack[nav_stack_ptr], last_loaded_url, MAX_STR_LEN - 1);
        nav_stack[nav_stack_ptr][MAX_STR_LEN - 1] = '\0';
        snprintf(nav_title_stack[nav_stack_ptr], sizeof(nav_title_stack[0]), "%s",
                 current_level_label[0] ? current_level_label
                 : nav_stack_ptr == 0 ? T("Portada") : current_feed_title);
        nav_stack_ptr++;
    }
    snprintf(current_level_label, sizeof(current_level_label), "%s", new_label ? new_label : "");
}

void GoBackInHierarchy() {
    if (nav_stack_ptr > 0) {
        nav_stack_ptr--; 
        snprintf(current_level_label, sizeof(current_level_label), "%s", nav_title_stack[nav_stack_ptr]);
        page_stack_ptr = 0;      
        current_global_offset = 0;
        return_to_last_page = 0;  
        LoadCatalog(nav_stack[nav_stack_ptr]); 
    } else { 
        current_state = STATE_SERVER_OPTIONS; 
        Repaint(); 
    }
}

void StripHTML(char *src) {
    if (!src || strlen(src) == 0) return;

    char *read = src;
    char *write = src;
    while (*read) {
        if (*read == '\n' || *read == '\r' || *read == '\t') {
            *write++ = ' ';
            read++;
            continue;
        }

        if ((unsigned char)read[0] == 0xE2 && (unsigned char)read[1] == 0x98 && (unsigned char)read[2] == 0x85) {
            *write++ = '*'; read += 3;
        } else if ((unsigned char)read[0] == 0xE2 && (unsigned char)read[1] == 0x98 && (unsigned char)read[2] == 0x86) {
            *write++ = '-'; read += 3;
        } 
        else if (*read == '&') {
            if (strncmp(read, "&lt;", 4) == 0) { *write++ = '<'; read += 4; }
            else if (strncmp(read, "&gt;", 4) == 0) { *write++ = '>'; read += 4; }
            else if (strncmp(read, "&amp;", 5) == 0) { *write++ = '&'; read += 5; }
            else if (strncmp(read, "&quot;", 6) == 0) { *write++ = '"'; read += 6; }
            else if (strncmp(read, "&nbsp;", 6) == 0) { *write++ = ' '; read += 6; }
            else if (strncmp(read, "&mdash;", 7) == 0) { *write++ = '-'; *write++ = '-'; read += 7; }
            else if (strncmp(read, "&ndash;", 7) == 0) { *write++ = '-'; read += 7; }
            else if (strncmp(read, "&ldquo;", 7) == 0) { *write++ = '"'; read += 7; }
            else if (strncmp(read, "&rdquo;", 7) == 0) { *write++ = '"'; read += 7; }
            else if (strncmp(read, "&lsquo;", 7) == 0) { *write++ = '\''; read += 7; }
            else if (strncmp(read, "&rsquo;", 7) == 0) { *write++ = '\''; read += 7; }
            else if (strncmp(read, "&hellip;", 8) == 0) { *write++ = '.'; *write++ = '.'; *write++ = '.'; read += 8; }
            else if (strncmp(read, "&#9733;", 7) == 0) { *write++ = '*'; read += 7; } 
            else if (strncmp(read, "&#9734;", 7) == 0) { *write++ = '-'; read += 7; } 
            else if (strncmp(read, "&#", 2) == 0) {
                int num = 0; char *temp = read + 2; int digits = 0;
                while (*temp >= '0' && *temp <= '9' && digits < 5) { num = num * 10 + (*temp - '0'); temp++; digits++; }
                if (digits > 0) {
                    if (num == 9733) { *write++ = '*'; }
                    else if (num == 9734) { *write++ = '-'; }
                    else if (num == 8212) { *write++ = '-'; *write++ = '-'; }
                    else if (num == 8211) { *write++ = '-'; }
                    else if (num == 8220 || num == 8221) { *write++ = '"'; }
                    else if (num == 8216 || num == 8217) { *write++ = '\''; }
                    else if (num == 8230) { *write++ = '.'; *write++ = '.'; *write++ = '.'; }
                    else if (num == 160) { *write++ = ' '; }
                    else if (num >= 32 && num <= 126) { *write++ = (char)num; }
                    if (*temp == ';') read = temp + 1; else read = temp; 
                } else { *write++ = *read++; }
            } else { *write++ = *read++; }
        } else { *write++ = *read++; }
    }
    *write = '\0';
    
    int in_tag = 0; char *dest = src; char *orig = src; 
    while (*src) {
        if (*src == '<') {
            in_tag = 1;
            if (strncasecmp(src, "<br", 3) == 0 || strncasecmp(src, "<p", 2) == 0 || strncasecmp(src, "</p", 3) == 0) {
                *dest++ = '\n';
            }
        } 
        else if (*src == '>') { in_tag = 0; } 
        else if (!in_tag) { *dest++ = *src; }
        src++;
    }
    *dest = '\0';
    
    char *d = orig; int space = 0; int newlines = 0;
    for (char *s = orig; *s; s++) {
        if (*s == ' ') {
            if (!space && newlines == 0) { *d++ = ' '; space = 1; }
        } else if (*s == '\n') {
            if (newlines < 2) { *d++ = '\n'; newlines++; }
            space = 0;
        } else {
            *d++ = *s; space = 0; newlines = 0;
        }
    }
    *d = '\0';

    char *r_star = orig;
    char *w_star = orig;
    int in_stars = 0;
    while (*r_star) {
        if (*r_star == '*' || *r_star == '-') {
            in_stars = 1;
            *w_star++ = *r_star++;
        } else if (in_stars && (*r_star == ' ' || *r_star == '\n' || *r_star == '\r')) {
            char *peek = r_star;
            while (*peek == ' ' || *peek == '\n' || *peek == '\r') peek++;
            if (*peek == '*' || *peek == '-') {
                r_star = peek; 
            } else {
                in_stars = 0;
                *w_star++ = *r_star++;
            }
        } else {
            in_stars = 0;
            *w_star++ = *r_star++;
        }
    }
    *w_star = '\0';

    int len = strlen(orig);
    while (len > 0 && (orig[len - 1] == ' ' || orig[len - 1] == '\n')) { orig[len - 1] = '\0'; len--; }
    char *start = orig;
    while (*start == ' ' || *start == '\n') start++;
    if (start != orig) memmove(orig, start, strlen(start) + 1);
}

void SeparateTags(char *text) {
    if (!text || strlen(text) == 0) return;
    
    const char *keywords[] = {"series:", "summary:", "tags:"};
    const char *separator = "\n\n";
    int sep_len = strlen(separator);

    for (int k = 0; k < 3; k++) {
        char temp[1024] = {0};
        strncpy(temp, text, 1023);
        
        for (int i = 0; temp[i]; i++) {
            temp[i] = tolower(temp[i]);
        }

        char *found = strstr(temp, keywords[k]);

        if (found) {
            char *target = text + (found - temp);
            
            if (target && target != text) {
                int current_len = strlen(text);

                if (current_len + sep_len < 1020) {
                    memmove(target + sep_len, target, strlen(target) + 1);
                    memcpy(target, separator, sep_len);
                }
            }
        }

    }
}

void KbdCallback(char *text) { Repaint(); }

void JumpToPageCallback(char *text) {
    if (!text || strlen(text) == 0) { Repaint(); return; }
    
    int target_absolute = atoi(text);
    if (target_absolute <= 0) { Repaint(); return; }

    int entries_per_page = GetCatalogRows();
    int current_batch_start = current_global_offset + 1;
    int max_local_pages = (entry_count + entries_per_page - 1) / entries_per_page;
    int current_batch_end = current_global_offset + max_local_pages;

    if (target_absolute >= current_batch_start && target_absolute <= current_batch_end) {
        current_page = target_absolute - current_global_offset - 1;
        JIT_DecodePageThumbnails();
        Repaint();
        FetchThumbnailsForPage();
    } 
    else if (target_absolute < current_batch_start) {
        int found_index = -1;
        
        for (int i = page_stack_ptr - 1; i >= 0; i--) {
            if (target_absolute > page_offset_stack[i]) {
                found_index = i;
                break;
            }
        }
        
        if (found_index >= 0) {
            int new_local_page = target_absolute - page_offset_stack[found_index] - 1;
            
            page_stack_ptr = found_index; 
            current_global_offset = page_offset_stack[found_index];
            
            jump_target_page = new_local_page;
            LoadCatalog(page_stack[found_index]);
        } else {
            Message(ICON_INFORMATION, T("Error de salto"), T("No se encontró la página en el historial."), 2000);
            Repaint();
        }
    } 
    else {
        Message(ICON_INFORMATION, T("Página desconocida"), T("No se puede ir a páginas futuras sin cargarlas primero."), 3000);
        Repaint();
    }
}

void TriggerLibraryRefresh() {
    if (access("/usr/bin/iv2sh", F_OK) == 0) system("/usr/bin/iv2sh SendEventTo -1 154 &");
    else if (access("/ebrmain/bin/iv2sh", F_OK) == 0) system("/ebrmain/bin/iv2sh SendEventTo -1 154 &");
    else Message(ICON_INFORMATION, "Error", T("No se encontró iv2sh"), 2000);
}

static int TriggerLibraryScan(void) {
    return SendEventTo(OTHERTASKS, EVT_STARTSCAN, 0, 0);
}

void JIT_DecodePageThumbnails() {
    if (!servers[current_server_index].fetch_thumbs) return;
    int entries_per_page = GetCatalogRows();
    int start = current_page * entries_per_page;
    int end = start + entries_per_page;
    if (end > entry_count) end = entry_count;

    for (int i = start; i < end; i++) {
        if (current_entries[i].is_book && !list_thumbs[i] && strlen(current_entries[i].thumb_url) > 10) {
            char t_path[MAX_STR_LEN];
            snprintf(t_path, sizeof(t_path), "%scache_%lu.jpg", IMAGES_DIR, hash_str(current_entries[i].thumb_url));
            struct stat st;
            if (stat(t_path, &st) == 0) list_thumbs[i] = LoadCoverSTB(t_path);
        }
    }
}

void FetchThumbnailsForPage() {
    if (!servers[current_server_index].fetch_thumbs) return;
    int entries_per_page = GetCatalogRows();
    int start = current_page * entries_per_page;
    int end = start + entries_per_page;
    if (end > entry_count) end = entry_count;

    int hh = sys_height / 14, gap = sys_height / 60;
    int row_h = sys_height / 13;
    int footer_y = sys_height - row_h - 10;
    int list_start_y = hh + gap;
    (void)footer_y;
    int list_avail_h = BrowseListBottom() - list_start_y;
    int dynamic_row_h = list_avail_h / entries_per_page;
    int icon_h = dynamic_row_h - 10, book_w = (int)(icon_h / 1.4);

    for (int i = start; i < end; i++) {
        if (list_thumbs[i]) continue;
        if (current_entries[i].is_book && strlen(current_entries[i].thumb_url) > 10) {
            char t_path[MAX_STR_LEN];
            snprintf(t_path, sizeof(t_path), "%scache_%lu.jpg", IMAGES_DIR, hash_str(current_entries[i].thumb_url));

            struct stat st;
            int image_result = stat(t_path, &st) == 0 ? 0 :
                (IsEbiblioURL(servers[current_server_index].url)
                    ? EbiblioDownload(current_entries[i].thumb_url, t_path,
                                      servers[current_server_index].user,
                                      servers[current_server_index].pass, NULL, 0)
                    : DownloadImage(current_entries[i].thumb_url, t_path,
                                    servers[current_server_index].user,
                                    servers[current_server_index].pass));
            if (image_result == 0) {
                list_thumbs[i] = LoadCoverSTB(t_path);
                if (list_thumbs[i]) {
                    int row_index = i - start;
                    int y_pos = list_start_y + (row_index * dynamic_row_h);
                    int icon_y = y_pos + 5, b_x = 20 + ((icon_h - book_w) / 2);
                    DrawSmartBitmap(b_x, icon_y, book_w, icon_h, list_thumbs[i], STRETCH, 1);
                    PartialUpdate(b_x, icon_y, book_w, icon_h);
                }
            }
        }
    }
}

void LoadCatalog(const char *url) {
    catalog_error[0] = '\0';
    if (EnsureNetwork() != 0) {
        if (current_state != STATE_BROWSING) {
            current_state = STATE_SERVER_OPTIONS;
        }
        Repaint();
        return; 
    }

    EnsureDirectories(); LoadStockIcons(); FreeListThumbs(); 

    int is_dark = IsDarkMode();
    int top_bg = is_dark ? 0xDDDDDD : 0x000000;
    int top_fg = is_dark ? 0x000000 : 0xFFFFFF;

    int hh = sys_height / 14; 
    FillArea(0, 0, sys_width, hh, top_bg);
    SetTextFont(36, top_fg);
    DrawTextRect(0, (hh/2)-20, sys_width, 40, T("Conectando..."), ALIGN_CENTER);
    FullUpdate(); 
    
    strncpy(last_loaded_url, url, MAX_STR_LEN - 1); 
    last_loaded_url[MAX_STR_LEN - 1] = '\0';
    
    struct MemoryStruct chunk = {NULL, 0};
    char network_error[MAX_STR_LEN] = {0};
    int is_ebiblio_server = IsEbiblioURL(servers[current_server_index].url);
    if (is_ebiblio_server && !IsEbiblioURL(last_loaded_url)) {
        entry_count = 0;
        snprintf(catalog_error, sizeof(catalog_error),
                 T("Se rechazó un enlace de catálogo fuera de eBiblio Madrid."));
        current_state = STATE_BROWSING;
        Repaint();
        return;
    }
    int fetch_result = is_ebiblio_server
        ? EbiblioFetchFeed(last_loaded_url, servers[current_server_index].user,
                           servers[current_server_index].pass, 0, &chunk,
                           network_error, sizeof(network_error))
        : FetchFeed(last_loaded_url, servers[current_server_index].user,
                    servers[current_server_index].pass, &chunk,
                    network_error, sizeof(network_error));
    if (fetch_result == 0) {
        memset(current_entries, 0, sizeof(OPDSEntry) * MAX_ENTRIES);
        entry_count = 0; 
 
        int parse_result = is_ebiblio_server
            ? ParseEbiblioFeed(chunk.memory, last_loaded_url)
            : ParseOPDSFeed(chunk.memory, last_loaded_url);
        free(chunk.memory);
        if (parse_result != 0) {
            entry_count = 0;
            snprintf(catalog_error, sizeof(catalog_error),
                     T("El servidor respondió, pero el contenido no es un catálogo OPDS válido."));
            current_state = STATE_BROWSING;
            Repaint();
        } else {
            StripHTML(current_feed_title);
            for (int i = 0; i < entry_count; i++) {
                StripHTML(current_entries[i].title);
                StripHTML(current_entries[i].author);
                StripHTML(current_entries[i].summary);
                SeparateTags(current_entries[i].summary); 
            }

            current_state = STATE_BROWSING;         
            
            if (jump_target_page >= 0) {
                current_page = jump_target_page;
                int entries_per_page = GetCatalogRows();
                int max_p = (entry_count > 0) ? ((entry_count - 1) / entries_per_page) : 0;
                if (current_page > max_p) current_page = max_p;
                jump_target_page = -1;
            } else if (return_to_last_page) {
                int entries_per_page = GetCatalogRows();
                current_page = (entry_count > 0) ? ((entry_count - 1) / entries_per_page) : 0;
                return_to_last_page = 0;
            } else { 
                current_page = 0; 
            }

            JIT_DecodePageThumbnails();
            Repaint(); 
            FetchThumbnailsForPage();
        }
    } else { 
        entry_count = 0; 
        snprintf(catalog_error, sizeof(catalog_error),
                 T("No se pudo cargar el catálogo.\n%s"),
                 network_error[0] ? network_error : T("Error de red sin más detalles."));
        current_state = STATE_BROWSING;
        Repaint();
    }

    ManageImageCache();
}

void URLEncode(const char *src, char *dest) {
    const char *hex = "0123456789ABCDEF";
    while (*src) {
        if ((*src >= 'a' && *src <= 'z') || (*src >= 'A' && *src <= 'Z') || 
            (*src >= '0' && *src <= '9') || *src == '-' || *src == '_' || *src == '.') {
            *dest++ = *src;
        } else if (*src == ' ') { 
            *dest++ = '%'; *dest++ = '2'; *dest++ = '0'; 
        } else {
            *dest++ = '%'; *dest++ = hex[(*src >> 4) & 15]; *dest++ = hex[*src & 15];
        }
        src++;
    }
    *dest = '\0';
}

static char search_input_buffer[256] = {0};

void PerformSearch(char *text) {
    char search_label[128];
    snprintf(search_label, sizeof(search_label), T("Búsqueda: %s"), text ? text : "");
    if (!text || strlen(text) == 0) { Repaint(); return; }

    if (strlen(current_search_url) == 0) {
        Message(ICON_ERROR, T("No compatible"), T("Este servidor OPDS no ofrece un enlace de búsqueda."), 2000);
        Repaint(); return;
    }

    if (IsEbiblioURL(servers[current_server_index].url)) {
        char encoded_query[sizeof(search_input_buffer) * 3] = {0};
        URLEncode(text, encoded_query);
        char final_url[MAX_STR_LEN] = {0};
        const char *template = current_search_url;
        const char *query_marker = strstr(template, "{?query}");
        const char *append_marker = strstr(template, "{&query}");
        const char *marker = query_marker ? query_marker : append_marker;
        int url_length;
        size_t url_capacity = sizeof(final_url);

        if (marker) {
            size_t prefix_size = (size_t)(marker - template);
            if (prefix_size >= sizeof(final_url)) prefix_size = sizeof(final_url) - 1;
            memcpy(final_url, template, prefix_size);
            final_url[prefix_size] = '\0';
            url_capacity -= prefix_size;
            url_length = snprintf(final_url + prefix_size,
                                   url_capacity,
                                   "%squery=%s%s", query_marker ? "?" : "&",
                                   encoded_query, marker + 8);
        } else {
            url_length = snprintf(final_url, sizeof(final_url), "%s%squery=%s",
                                   template, strchr(template, '?') ? "&" : "?",
                                   encoded_query);
        }
        if (url_length < 0 || (size_t)url_length >= url_capacity) {
            Message(ICON_ERROR, T("Búsqueda"), T("La dirección de búsqueda es demasiado larga."), 4000);
            Repaint();
            return;
        }

        PushNavigationLevel(search_label);
        page_stack_ptr = 0;
        current_global_offset = 0;
        return_to_last_page = 0;
        LoadCatalog(final_url);
        return;
    }

    if (is_opensearch_url) {
        struct MemoryStruct osd_chunk = {NULL, 0};
        char network_error[MAX_STR_LEN] = {0};
        if (FetchFeed(current_search_url, servers[current_server_index].user,
                      servers[current_server_index].pass, &osd_chunk,
                      network_error, sizeof(network_error)) != 0) {
            snprintf(catalog_error, sizeof(catalog_error),
                     T("No se pudo consultar la búsqueda.\n%s"),
                     network_error[0] ? network_error : T("Error de red sin más detalles."));
            entry_count = 0;
            current_state = STATE_BROWSING;
            Repaint();
            return;
        }

        char new_template[MAX_STR_LEN] = {0};
        int parse_result = ParseOpenSearch(osd_chunk.memory, current_search_url, new_template);
        free(osd_chunk.memory);
        if (parse_result != 0) {
            snprintf(catalog_error, sizeof(catalog_error),
                     T("El servidor respondió, pero no se pudo leer su descripción de búsqueda OpenSearch."));
            entry_count = 0;
            current_state = STATE_BROWSING;
            Repaint();
            return;
        }
        strncpy(current_search_url, new_template, MAX_STR_LEN - 1);
        current_search_url[MAX_STR_LEN - 1] = '\0';
        is_opensearch_url = 0;
    }

    char encoded_query[512] = {0};
    URLEncode(text, encoded_query);
    char final_url[MAX_STR_LEN * 2] = {0};

    char *template_ptr = strstr(current_search_url, "{searchTerms}");
    if (template_ptr) {
        int prefix_len = template_ptr - current_search_url;
        strncpy(final_url, current_search_url, prefix_len);
        final_url[prefix_len] = '\0';
        strcat(final_url, encoded_query);
        strcat(final_url, template_ptr + 13); 
    } else {
        snprintf(final_url, sizeof(final_url), "%s%squery=%s", 
                 current_search_url, strstr(current_search_url, "?") ? "&" : "?", encoded_query);
    }

    if (strncmp(final_url, "http://", 7) != 0 && strncmp(final_url, "https://", 8) != 0) {
        char absolute_url[MAX_STR_LEN * 2] = {0};
        char base_domain[256] = {0};
        
        const char *proto_end = strstr(servers[current_server_index].url, "://");
        if (proto_end) {
            const char *path_start = strchr(proto_end + 3, '/');
            if (path_start) {
                strncpy(base_domain, servers[current_server_index].url, path_start - servers[current_server_index].url);
            } else {
                strcpy(base_domain, servers[current_server_index].url);
            }
        } else {
            strcpy(base_domain, servers[current_server_index].url);
        }
        
        if (final_url[0] == '/') {
            snprintf(absolute_url, sizeof(absolute_url), "%s%s", base_domain, final_url);
        } else {
            snprintf(absolute_url, sizeof(absolute_url), "%s/%s", base_domain, final_url);
        }
        strcpy(final_url, absolute_url);
    }

    PushNavigationLevel(search_label);
    page_stack_ptr = 0;
    current_global_offset = 0;
    return_to_last_page = 0;

    LoadCatalog(final_url);
}

// --- PIN de las contraseñas -------------------------------------------------

static void (*pin_then)(void) = NULL;
static char pin_buffer[64];
static char pin_new[64];

static void WipePinBuffers(void) {
    volatile char *p = pin_buffer;
    for (size_t i = 0; i < sizeof(pin_buffer); i++) p[i] = 0;
    p = pin_new;
    for (size_t i = 0; i < sizeof(pin_new); i++) p[i] = 0;
}

static void ForgotPinDialog(int button) {
    if (button == 1) {
        ForgetLockedServerPasswords();
        SecretsDisablePin();
        SaveServers();
        Message(ICON_INFORMATION, T("PIN desactivado"),
                T("Se borraron las contraseñas protegidas. Vuelve a escribirlas en los datos de cada servidor."), 5000);
    }
    pin_then = NULL;
    Repaint();
}

static void PinEnteredCallback(char *text) {
    if (!text) {
        WipePinBuffers();
        Dialog(ICON_QUESTION, T("¿Has olvidado el PIN?"),
               T("Sin el PIN no se pueden leer las contraseñas guardadas. Puedes borrarlas y desactivar el PIN; después tendrás que volver a escribirlas."),
               T("Borrar contraseñas"), T("Cancelar"), ForgotPinDialog);
        return;
    }
    ShowHourglass();
    int ok = SecretsUnlock(text) == 0;
    HideHourglass();
    WipePinBuffers();
    if (!ok) {
        pin_then = NULL;
        Message(ICON_ERROR, T("PIN incorrecto"), T("El PIN no es correcto."), 2500);
        Repaint();
        return;
    }
    UnlockLockedServerPasswords();
    void (*then)(void) = pin_then;
    pin_then = NULL;
    if (then) then(); else Repaint();
}

/* Ejecuta "then" directamente o, si las contraseñas están protegidas por un
   PIN que aún no se ha escrito, después de pedirlo. */
static void RequirePin(void (*then)(void)) {
    if (!SecretsPinEnabled() || SecretsPinUnlocked()) {
        then();
        return;
    }
    pin_then = then;
    WipePinBuffers();
    OpenKeyboard(T("PIN de Pergamo"), pin_buffer, sizeof(pin_buffer) - 1,
                 KBD_PASSWORD, PinEnteredCallback);
}

static void PinConfirmCallback(char *text) {
    if (text) {
        if (strcmp(text, pin_new) != 0) {
            Message(ICON_ERROR, T("PIN"), T("Los PIN no coinciden."), 2500);
        } else {
            ShowHourglass();
            int ok = SecretsEnablePin(pin_new) == 0;
            if (ok) SaveServers();
            HideHourglass();
            if (ok) {
                Message(ICON_INFORMATION, T("PIN activado"),
                        T("Las contraseñas están protegidas con tu PIN. Se pedirá al abrir un servidor con contraseña."), 4000);
            } else {
                Message(ICON_ERROR, T("PIN"), T("No se pudo activar el PIN."), 3000);
            }
        }
    }
    WipePinBuffers();
    Repaint();
}

static void PinNewCallback(char *text) {
    if (!text || !text[0]) { WipePinBuffers(); Repaint(); return; }
    if (strlen(text) < 4) {
        WipePinBuffers();
        Message(ICON_ERROR, T("PIN"), T("El PIN debe tener al menos 4 caracteres."), 2500);
        Repaint();
        return;
    }
    snprintf(pin_new, sizeof(pin_new), "%s", text);
    memset(pin_buffer, 0, sizeof(pin_buffer));
    OpenKeyboard(T("Repite el PIN"), pin_buffer, sizeof(pin_buffer) - 1,
                 KBD_PASSWORD, PinConfirmCallback);
}

static void DisablePinNow(void) {
    ShowHourglass();
    SecretsDisablePin();
    SaveServers();
    HideHourglass();
    Message(ICON_INFORMATION, T("PIN desactivado"),
            T("Las contraseñas siguen cifradas, pero ya no se pide el PIN."), 3000);
    Repaint();
}

static void TogglePinProtection(void) {
    if (SecretsPinEnabled()) {
        RequirePin(DisablePinNow);
    } else {
        WipePinBuffers();
        OpenKeyboard(T("Nuevo PIN (mínimo 4 caracteres)"), pin_buffer, sizeof(pin_buffer) - 1,
                     KBD_PASSWORD, PinNewCallback);
    }
}

static void OpenServerOptionsNow(void) {
    current_state = STATE_SERVER_OPTIONS;
    Repaint();
}

static void OpenNewServerFormNow(void) {
    memset(&temp_server, 0, sizeof(OPDSServer));
    temp_server.fetch_thumbs = 1;
    temp_server.catalog_rows = 10;
    editing_server_index = -1;
    current_state = STATE_SERVER_FORM;
    Repaint();
}

static void OpenEditServerFormNow(void) {
    editing_server_index = current_server_index;
    memcpy(&temp_server, &servers[current_server_index], sizeof(OPDSServer));
    current_state = STATE_SERVER_FORM;
    Repaint();
}

void DrawMainMenu() {
    SetPanelType(0); nav_stack_ptr = 0; current_level_label[0] = '\0'; page_stack_ptr = 0; current_global_offset = 0;
    int margin = sys_width / 20, gap = sys_height / 60, hh = sys_height / 14, row_h = sys_height / 12;
    SetTextFont(48, 0x000000);
    DrawTextRect(0, gap, sys_width, 60, T("Biblioteca OPDS"), ALIGN_CENTER);
    DrawLine(margin, hh, sys_width - margin, hh, 0x000000);
    int y = hh + gap; SetTextFont(36, 0x000000);
    for (int i = 0; i < server_count; i++) {
        char buf[300]; snprintf(buf, sizeof(buf), "%d. %s", i + 1, servers[i].name);
        DrawTextRect(margin * 2, y + (row_h / 4), sys_width - (margin * 4), 50, buf, ALIGN_LEFT);
        DrawLine(margin * 2, y + row_h, sys_width - (margin * 2), y + row_h, 0xAAAAAA);
        y += row_h; 
    }
    y += gap; 
    DrawButton(margin * 2, y, sys_width - (margin * 4), row_h, T("+ Agregar servidor"), 0);
    DrawButton(margin * 2, y + row_h + gap, sys_width - (margin * 4), row_h,
               T("Configuración"), 0);
    DrawButton(margin * 2, y + (row_h + gap) * 2, sys_width - (margin * 4), row_h,
               T("Salir de la aplicación"), 2);

    if (credentials_warning[0]) {
        char warning[MAX_STR_LEN + 128];
        snprintf(warning, sizeof(warning),
                 T("No se pudo leer la contraseña guardada de: %s. Vuelve a escribirla."),
                 credentials_warning);
        SetTextFont(24, 0xCC0000);
        DrawTextRect(margin, sys_height - 110, sys_width - margin * 2, 60, warning,
                     ALIGN_CENTER | DOTS);
    }

    SetTextFont(24, 0x555555); 
    DrawTextRect(0, sys_height - 40, sys_width, 30, "v" PERGAMO_VERSION, ALIGN_CENTER);
}

void HandleMainMenuTouch(int x, int y) {
    int margin = sys_width / 20, gap = sys_height / 60, hh = sys_height / 14, row_h = sys_height / 12, list_y = hh + gap;

    for (int i = 0; i < server_count; i++) {
        if (y >= list_y && y <= list_y + row_h) {
            FlashArea(margin * 2, list_y, sys_width - (margin * 4), row_h);
            current_server_index = i; 
            strncpy(current_host, servers[i].url, MAX_STR_LEN - 1);
            if (servers[i].pass_locked) RequirePin(OpenServerOptionsNow);
            else OpenServerOptionsNow();
            return; 
        } 
        list_y += row_h;
    }
    int add_y = list_y + gap;
    if (y >= add_y && y <= add_y + row_h) {
        FlashArea(margin * 2, add_y, sys_width - (margin * 4), row_h);
        RequirePin(OpenNewServerFormNow); return; 
    } 
    int settings_y = add_y + row_h + gap;
    if (x >= margin * 2 && x <= sys_width - margin * 2 &&
        y >= settings_y && y <= settings_y + row_h) {
        FlashArea(margin * 2, settings_y, sys_width - margin * 4, row_h);
        current_state = STATE_SETTINGS;
        Repaint();
        return;
    }
    int action_y = settings_y + row_h + gap;
    if (x >= margin * 2 && x <= sys_width - margin * 2 &&
        y >= action_y && y <= action_y + row_h) {
        FlashArea(margin * 2, action_y, sys_width - margin * 4, row_h);
        Message(ICON_INFORMATION, T("Saliendo"), T("Limpiando..."), 0); Repaint();
        ManageImageCache(); TriggerLibraryRefresh(); CloseApp();
    }
}

// --- Actualizaciones ---------------------------------------------------------

static char update_remote_version[32];

static void UpdateProgress(int percent) {
    UpdateProgressbar(T("Descargando Pergamo..."), percent);
}

static void CloseAfterUpdate(int button) {
    (void)button;
    CloseApp();
}

static void InstallUpdateNow(int button) {
    if (button != 1) { Repaint(); return; }
    if (EnsureNetwork() != 0) { Repaint(); return; }
    char error[MAX_STR_LEN] = {0};
    OpenProgressbar(ICON_INFORMATION, T("Actualizando"), T("Descargando Pergamo..."), 0, NULL);
    int result = UpdateInstall(UpdateProgress, error, sizeof(error));
    CloseProgressbar();
    if (result != 0) {
        Message(ICON_ERROR, T("Actualización"), error[0] ? error : T("No se pudo completar la operación."), 5000);
        Repaint();
        return;
    }
    static char text[512];
    snprintf(text, sizeof(text),
             T("Se ha instalado la versión %s. Pergamo se cerrará; vuelve a abrirla para usar la nueva versión."),
             update_remote_version);
    Dialog(ICON_INFORMATION, T("Pergamo actualizado"), text, "OK", NULL, CloseAfterUpdate);
}

/* interactive = 1: desde Configuración (conecta la Wi-Fi y avisa de todo).
   interactive = 0: al abrir la app (solo si ya hay Wi-Fi y sin molestar). */
static void CheckForUpdates(int interactive) {
    if (interactive) {
        if (EnsureNetwork() != 0) { Repaint(); return; }
    } else if (!(QueryNetwork() & NET_CONNECTED)) {
        return;
    }
    char error[MAX_STR_LEN] = {0};
    if (interactive) ShowHourglass();
    int result = UpdateCheck(update_remote_version, sizeof(update_remote_version),
                             error, sizeof(error));
    if (interactive) HideHourglass();
    if (result >= 0) SettingSetLong("last_update_check", (long)time(NULL));

    if (result < 0) {
        if (interactive) {
            Message(ICON_ERROR, T("Actualización"), error[0] ? error : T("No se pudo completar la operación."), 5000);
            Repaint();
        }
        return;
    }
    if (result == 0) {
        if (interactive) {
            char text[256];
            snprintf(text, sizeof(text), T("Tienes la última versión (%s)."), PERGAMO_VERSION);
            Message(ICON_INFORMATION, T("Actualización"), text, 3000);
            Repaint();
        }
        return;
    }
    static char text[512];
    snprintf(text, sizeof(text),
             T("Hay una versión nueva de Pergamo: %s (tienes la %s). ¿Actualizar ahora?"),
             update_remote_version, PERGAMO_VERSION);
    Dialog(ICON_QUESTION, T("Actualización disponible"), text,
           T("Actualizar"), T("Más tarde"), InstallUpdateNow);
}

void AutoCheckForUpdates(void) {
    if (!SettingGetLong("auto_update_check", 1)) return;
    if (current_state != STATE_MAIN_MENU) return;
    long last = SettingGetLong("last_update_check", 0);
    long now = (long)time(NULL);
    if (now - last >= 0 && now - last < 24L * 3600) return;
    CheckForUpdates(0);
}

// --- Configuración ---------------------------------------------------------

enum {
    SETTINGS_LANGUAGE,
    SETTINGS_PIN,
    SETTINGS_CHECK_UPDATES,
    SETTINGS_AUTO_UPDATES,
    SETTINGS_DEBUG_LOG,
    SETTINGS_CLEAR_CACHE,
    SETTINGS_ABOUT,
    SETTINGS_UNINSTALL,
    SETTINGS_ITEM_COUNT
};

static int DebugLogEnabled(void) {
    return access(DEBUG_LOG_TRIGGER, F_OK) == 0;
}

static int SettingsBackY(void) {
    return sys_height - sys_height / 12 - (sys_height / 60) * 3;
}

/* Las filas se ajustan para que todas las opciones quepan sobre "Volver". */
static void SettingsGeometry(int *margin, int *gap, int *hh, int *row_h, int *top) {
    *margin = sys_width / 20;
    *gap = sys_height / 80;
    *hh = sys_height / 14;
    *top = *hh + *gap * 2;
    int available = SettingsBackY() - *gap * 2 - *top;
    *row_h = available / SETTINGS_ITEM_COUNT - *gap;
    if (*row_h > sys_height / 12) *row_h = sys_height / 12;
}

void DrawSettings() {
    SetPanelType(0);
    int margin, gap, hh, row_h, top;
    SettingsGeometry(&margin, &gap, &hh, &row_h, &top);

    SetTextFont(48, 0x000000);
    DrawTextRect(0, gap, sys_width, 60, T("Configuración"), ALIGN_CENTER);
    DrawLine(margin, hh, sys_width - margin, hh, 0x000000);

    char text[256];
    int y = top;
    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        int style = 0;
        switch (i) {
            case SETTINGS_LANGUAGE:
                snprintf(text, sizeof(text), T("Idioma: %s"),
                         LanguageSettingName(app_language_setting));
                break;
            case SETTINGS_PIN:
                snprintf(text, sizeof(text), T("PIN para las contraseñas: %s"),
                         SecretsPinEnabled() ? T("activado") : T("desactivado"));
                break;
            case SETTINGS_CHECK_UPDATES:
                snprintf(text, sizeof(text), T("Buscar actualizaciones (v%s)"), PERGAMO_VERSION);
                break;
            case SETTINGS_AUTO_UPDATES:
                snprintf(text, sizeof(text), T("Buscar al abrir: %s"),
                         SettingGetLong("auto_update_check", 1) ? T("activado") : T("desactivado"));
                break;
            case SETTINGS_DEBUG_LOG:
                snprintf(text, sizeof(text), T("Registro de depuración: %s"),
                         DebugLogEnabled() ? T("activado") : T("desactivado"));
                break;
            case SETTINGS_CLEAR_CACHE:
                snprintf(text, sizeof(text), "%s", T("Vaciar caché de portadas"));
                break;
            case SETTINGS_ABOUT:
                snprintf(text, sizeof(text), "%s", T("Acerca de"));
                break;
            case SETTINGS_UNINSTALL:
                snprintf(text, sizeof(text), "%s", T("Desinstalar"));
                style = 2;
                break;
        }
        DrawButton(margin * 2, y, sys_width - margin * 4, row_h, text, style);
        y += row_h + gap;
    }
    DrawButton(margin * 2, SettingsBackY(), sys_width - margin * 4, sys_height / 12,
               T("Volver"), 1);

    SetTextFont(24, 0x555555);
    DrawTextRect(0, sys_height - 40, sys_width, 30, "v" PERGAMO_VERSION, ALIGN_CENTER);
}

static void CloseAboutDialog(int button) {
    (void)button;
    Repaint();
}

void HandleSettingsTouch(int x, int y) {
    int margin, gap, hh, row_h, top;
    SettingsGeometry(&margin, &gap, &hh, &row_h, &top);
    if (x < margin * 2 || x > sys_width - margin * 2) return;

    int back_y = SettingsBackY();
    if (y >= back_y && y <= back_y + sys_height / 12) {
        FlashArea(margin * 2, back_y, sys_width - margin * 4, sys_height / 12);
        current_state = STATE_MAIN_MENU;
        Repaint();
        return;
    }

    if (y < top) return;
    int item = (y - top) / (row_h + gap);
    int item_y = top + item * (row_h + gap);
    if (item >= SETTINGS_ITEM_COUNT || y > item_y + row_h) return;
    FlashArea(margin * 2, item_y, sys_width - margin * 4, row_h);

    switch (item) {
        case SETTINGS_LANGUAGE: {
            /* Automático -> Español -> English -> Automático */
            int next = app_language_setting == LANG_AUTO ? LANG_ES
                     : app_language_setting == LANG_ES ? LANG_EN : LANG_AUTO;
            SetLanguageSetting(next);
            Repaint();
            break;
        }
        case SETTINGS_PIN:
            TogglePinProtection();
            break;
        case SETTINGS_CHECK_UPDATES:
            CheckForUpdates(1);
            break;
        case SETTINGS_AUTO_UPDATES:
            SettingSetLong("auto_update_check", !SettingGetLong("auto_update_check", 1));
            Repaint();
            break;
        case SETTINGS_DEBUG_LOG: {
            int ok;
            if (DebugLogEnabled()) {
                ok = unlink(DEBUG_LOG_TRIGGER) == 0;
            } else {
                FILE *f = fopen(DEBUG_LOG_TRIGGER, "w");
                ok = f != NULL;
                if (f) fclose(f);
            }
            if (!ok) {
                Message(ICON_ERROR, T("Registro de depuración"),
                        T("No se pudo cambiar el registro de depuración."), 3000);
            } else if (DebugLogEnabled()) {
                Message(ICON_INFORMATION, T("Registro de depuración"),
                        T("El registro se guardará en applications/Pergamo/opds_client.log"), 3000);
            }
            Repaint();
            break;
        }
        case SETTINGS_CLEAR_CACHE:
            ClearPergamoCoverCache();
            Message(ICON_INFORMATION, T("Caché vaciada"),
                    T("Se borraron las portadas guardadas."), 2500);
            Repaint();
            break;
        case SETTINGS_ABOUT: {
            static char about[768];
            snprintf(about, sizeof(about),
                     T("Pergamo %s\n\nCliente OPDS para lectores PocketBook: explora catálogos (Calibre-Web, COPS, Proyecto Gutenberg...) y eBiblio Madrid, y descarga libros directamente al lector.\n\nBasado en OPDS Client:\ngithub.com/j2robin/Pocketbook-OPDSClient\n\ngithub.com/amiguelp/Pergamo"),
                     PERGAMO_VERSION);
            Dialog(ICON_INFORMATION, T("Acerca de"), about, "OK", NULL, CloseAboutDialog);
            break;
        }
        case SETTINGS_UNINSTALL:
            current_state = STATE_UNINSTALL_CONFIRM;
            Repaint();
            break;
    }
}

void DrawUninstallConfirmation() {
    int margin = sys_width / 20;
    int gap = sys_height / 40;
    int row_h = sys_height / 12;

    SetTextFont(48, 0x000000);
    DrawTextRect(0, sys_height / 6, sys_width, 70, T("Desinstalar Pergamo"), ALIGN_CENTER);
    SetTextFont(30, 0x000000);
    DrawTextRect(margin, sys_height / 3, sys_width - margin * 2, 55,
                 T("Se eliminarán la aplicación, sus ajustes y caché."), ALIGN_CENTER);
    DrawTextRect(margin, sys_height / 3 + 55 + gap, sys_width - margin * 2, 55,
                 T("Los libros descargados y otros archivos se conservarán."), ALIGN_CENTER);
    DrawTextRect(margin, sys_height / 3 + 110 + gap * 2, sys_width - margin * 2, 55,
                 T("La copia de seguridad de view.json también se conservará."), ALIGN_CENTER);

    int button_gap = gap;
    int button_w = (sys_width - margin * 2 - button_gap) / 2;
    int button_y = sys_height - row_h - gap;
    DrawButton(margin, button_y, button_w, row_h, T("Cancelar"), 1);
    DrawButton(margin + button_w + button_gap, button_y, button_w, row_h,
               T("Desinstalar"), 2);
}

void HandleUninstallConfirmationTouch(int x, int y) {
    int margin = sys_width / 20;
    int gap = sys_height / 40;
    int row_h = sys_height / 12;
    int button_gap = gap;
    int button_w = (sys_width - margin * 2 - button_gap) / 2;
    int button_y = sys_height - row_h - gap;

    if (y < button_y || y > button_y + row_h) return;
    if (x >= margin && x < margin + button_w) {
        FlashArea(margin, button_y, button_w, row_h);
        current_state = STATE_SETTINGS;
        Repaint();
        return;
    }
    if (x < margin + button_w + button_gap ||
        x > margin + button_w + button_gap + button_w) {
        return;
    }

    FlashArea(margin + button_w + button_gap, button_y, button_w, row_h);
    int result = UninstallPergamo();
    if (result < 0) {
        current_state = STATE_SETTINGS;
        Message(ICON_ERROR, T("Desinstalación"), T("No se pudo retirar del launcher; no se borraron archivos."), 5000);
        Repaint();
        return;
    }
    if (result > 0) {
        AskRestartAfterUninstall(0);
        return;
    }
    AskRestartAfterUninstall(1);
}

void DrawServerOptions() {
    int gap = sys_height / 40;
    int row_h = sys_height / 12;
    int bw = sys_width - (sys_width / 4);
    int bx = sys_width / 8;
    int m = sys_width / 20;

    SetTextFont(48, 0x000000); 
    DrawTextRect(0, gap, sys_width, 60, servers[current_server_index].name, ALIGN_CENTER);

    int top_y = sys_height / 5;
    DrawButton(bx, top_y, bw, row_h, T("Explorar catálogo"), 0);
    int is_ebiblio = IsEbiblioURL(servers[current_server_index].url);
    DrawButton(bx, top_y + row_h + gap, bw, row_h,
               is_ebiblio ? T("Mis préstamos y reservas") : T("Editar datos"), 0);
    if (is_ebiblio) {
        DrawButton(bx, top_y + (row_h + gap) * 2, bw, row_h, T("Editar datos"), 0);
    }

    /* Mismo espaciado que el resto de botones. */
    int mid_y = top_y + (row_h + gap) * (is_ebiblio ? 3 : 2);
    DrawButton(bx, mid_y, bw, row_h, T("Eliminar servidor"), 2);

    DrawButton(m, sys_height - 110, sys_width - (m * 2), 90, T("Menú principal"), 1);
}

void HandleServerOptionsTouch(int x, int y) {
    int gap = sys_height / 40;
    int row_h = sys_height / 12;
    int top_y = sys_height / 5;
    int mid_y = top_y + (row_h + gap) * (IsEbiblioURL(servers[current_server_index].url) ? 3 : 2);
    int bw = sys_width - (sys_width / 4);
    int bx = sys_width / 8;
    int m = sys_width / 20;

    if (y >= top_y && y <= top_y + row_h) {
        FlashArea(bx, top_y, bw, row_h);
        nav_stack_ptr = 0; current_level_label[0] = '\0'; page_stack_ptr = 0; current_global_offset = 0; current_page = 0; 
        LoadCatalog(servers[current_server_index].url); return; 
    }
    int is_ebiblio = IsEbiblioURL(servers[current_server_index].url);
    if (y >= top_y + row_h + gap && y <= top_y + (row_h * 2) + gap) {
        FlashArea(bx, top_y + row_h + gap, bw, row_h);
        if (is_ebiblio) {
            if (EnsureNetwork() != 0) {
                Repaint();
                return;
            }
            struct MemoryStruct chunk = {NULL, 0};
            char error[MAX_STR_LEN] = {0};
            if (EbiblioFetchFeed(servers[current_server_index].url,
                                 servers[current_server_index].user,
                                 servers[current_server_index].pass, 0,
                                 &chunk, error, sizeof(error)) != 0) {
                Message(ICON_ERROR, "eBiblio", error[0] ? error : T("No se pudo consultar la cuenta."), 5000);
                Repaint();
                return;
            }
            char account_url[MAX_STR_LEN] = {0};
            int found = FindEbiblioAccountURL(chunk.memory,
                                               servers[current_server_index].url,
                                               account_url, sizeof(account_url));
            free(chunk.memory);
            if (found != 0) {
                Message(ICON_ERROR, "eBiblio", T("No se encontró el enlace a préstamos y reservas."), 5000);
                Repaint();
                return;
            }
            snprintf(current_level_label, sizeof(current_level_label), "%s", T("Mis préstamos y reservas"));
            LoadCatalog(account_url);
            return;
        }
        RequirePin(OpenEditServerFormNow); return;
    }
    if (is_ebiblio && y >= top_y + (row_h + gap) * 2 &&
        y <= top_y + (row_h + gap) * 2 + row_h) {
        FlashArea(bx, top_y + (row_h + gap) * 2, bw, row_h);
        RequirePin(OpenEditServerFormNow);
        return;
    }
    if (y >= mid_y && y <= mid_y + row_h) {
        FlashArea(bx, mid_y, bw, row_h);
        for (int i = current_server_index; i < server_count - 1; i++) servers[i] = servers[i + 1];
        server_count--; SaveServers(); current_state = STATE_MAIN_MENU; Repaint(); return;
    }
    if (y >= sys_height - 110 && y <= sys_height - 20) {
        FlashArea(m, sys_height - 110, sys_width - (m * 2), 90);
        current_state = STATE_MAIN_MENU; Repaint(); return; 
    }
}

void DrawServerForm() {
    int gap = sys_height / 40, row_h = sys_height / 12, box_x = sys_width / 4, box_w = sys_width - box_x - (sys_width / 10), y = (sys_height / 10);
    SetTextFont(48, 0x000000); 
    DrawTextRect(0, gap, sys_width, 60, (editing_server_index >= 0) ? T("Editar servidor") : T("Agregar servidor"), ALIGN_CENTER);
    SetTextFont(36, 0x000000); 
    
    const char *labels[] = {T("Nombre:"), "URL:", T("Usuario:"), T("Contraseña:"), T("Miniaturas:"), T("Filas:")};
    char *vals[] = {temp_server.name, temp_server.url, temp_server.user, temp_server.pass};
    
    for(int i=0; i<6; i++) {
        DrawTextRect(20, y + (gap / 2), box_x - 30, 40, labels[i], ALIGN_LEFT); 
        if (i < 4) {
            DrawRect(box_x, y, box_w, row_h, 0x000000);
            if (i == 3) {
                char m[256] = {0}; for(size_t j=0; j<strlen(vals[i]); j++) m[j] = '*';
                DrawTextRect(box_x + 15, y + (gap / 2), box_w - 30, 40, m, ALIGN_LEFT);
            } else { DrawTextRect(box_x + 15, y + (gap / 2), box_w - 30, 40, vals[i], ALIGN_LEFT); }
        } else if (i == 4) {
            DrawRect(box_x, y + (row_h / 2) - 20, 40, 40, 0x000000);
            if (temp_server.fetch_thumbs) FillArea(box_x + 5, y + (row_h / 2) - 15, 30, 30, 0x555555); 
        } else if (i == 5) {
            DrawRect(box_x, y, 60, row_h, 0x000000);
            SetTextFont(36, 0x000000);
            DrawTextRect(box_x, y + (row_h / 2) - 20, 60, 40, "-", ALIGN_CENTER);
            
            char r_buf[16]; snprintf(r_buf, sizeof(r_buf), "%d", temp_server.catalog_rows);
            DrawTextRect(box_x + 70, y + (row_h / 2) - 20, box_w - 140, 40, r_buf, ALIGN_CENTER);
            
            DrawRect(box_x + box_w - 60, y, 60, row_h, 0x000000);
            DrawTextRect(box_x + box_w - 60, y + (row_h / 2) - 20, 60, 40, "+", ALIGN_CENTER);
        }
        y += row_h + gap;
    }
    y += gap; 
    DrawButton(20, y, (sys_width / 2) - 40, row_h, T("CANCELAR"), 0);
    DrawButton(sys_width / 2 + 20, y, (sys_width / 2) - 40, row_h, T("GUARDAR"), 1);
}

void HandleServerFormTouch(int x, int y) {
    int gap = sys_height / 40, row_h = sys_height / 12, ly = (sys_height / 10);
    int box_x = sys_width / 4, box_w = sys_width - box_x - (sys_width / 10);
    
    if (y >= ly && y <= ly + row_h) { FlashArea(box_x, ly, box_w, row_h); OpenKeyboard(T("Nombre"), temp_server.name, MAX_STR_LEN - 1, 0, KbdCallback); return; } ly += row_h + gap;
    if (y >= ly && y <= ly + row_h) { FlashArea(box_x, ly, box_w, row_h); OpenKeyboard("URL", temp_server.url, MAX_STR_LEN - 1, 0, KbdCallback); return; } ly += row_h + gap;
    if (y >= ly && y <= ly + row_h) { FlashArea(box_x, ly, box_w, row_h); OpenKeyboard(T("Usuario"), temp_server.user, MAX_STR_LEN - 1, 0, KbdCallback); return; } ly += row_h + gap;
    if (y >= ly && y <= ly + row_h) { FlashArea(box_x, ly, box_w, row_h); OpenKeyboard(T("Contraseña"), temp_server.pass, MAX_STR_LEN - 1, 0, KbdCallback); return; } ly += row_h + gap;
    
    if (y >= ly && y <= ly + row_h) { FlashArea(box_x, ly + (row_h / 2) - 20, 40, 40); temp_server.fetch_thumbs = !temp_server.fetch_thumbs; Repaint(); return; } ly += row_h + gap;
    
    if (y >= ly && y <= ly + row_h) { 
        if (x >= box_x && x <= box_x + 60) {
            FlashArea(box_x, ly, 60, row_h);
            if (temp_server.catalog_rows > 4) temp_server.catalog_rows--; // Lower limit is now 4
            Repaint(); return;
        } else if (x >= box_x + box_w - 60 && x <= box_x + box_w) {
            FlashArea(box_x + box_w - 60, ly, 60, row_h);
            if (temp_server.catalog_rows < 10) temp_server.catalog_rows++;
            Repaint(); return;
        }
    }
    ly += row_h + (gap * 2);

    if (y >= ly && y <= ly + row_h) {
        if (x < sys_width / 2) {
            FlashArea(20, ly, (sys_width / 2) - 40, row_h);
        } else {
            FlashArea(sys_width / 2 + 20, ly, (sys_width / 2) - 40, row_h);
            if (strlen(temp_server.name) > 0) {
                temp_server.pass_locked = 0;
                if (editing_server_index >= 0) servers[editing_server_index] = temp_server;
                else if (server_count < MAX_SERVERS) servers[server_count++] = temp_server;
                credentials_warning[0] = '\0';
                SaveServers();
            }
        }
        current_state = STATE_MAIN_MENU; Repaint();
    }
}

void DrawBrowsingView() {
    int is_dark = IsDarkMode();
    int top_bg = is_dark ? 0xDDDDDD : 0x000000;
    int top_fg = is_dark ? 0x000000 : 0xFFFFFF;
    int top_sub_fg = is_dark ? 0x555555 : 0xCCCCCC;

    int entries_per_page = GetCatalogRows();
    int hh = sys_height / 14, row_h = sys_height / 13, gap = sys_height / 60;
    int footer_y = sys_height - row_h - 10; 

    FillArea(0, 0, sys_width, hh, top_bg);

    /* El título ocupa toda la cabecera; tocarlo vuelve a las opciones del servidor. */
    int title_x = 20, title_w = sys_width - 40;

    /* Arriba el servidor; debajo, la ruta hasta la lista actual. */
    {
        char breadcrumb[MAX_STR_LEN];
        SetTextFont(24, top_sub_fg);
        BuildBreadcrumb(breadcrumb, sizeof(breadcrumb), title_w);
        DrawTextRect(title_x, (hh / 2) + 5, title_w, 30, breadcrumb, ALIGN_LEFT | DOTS);
    }

    SetTextFont(36, top_fg); 
    DrawTextRect(title_x, hh / 6, title_w, 40, servers[current_server_index].name, ALIGN_LEFT | DOTS);

    /* Barra de navegación abajo, con botones grandes. */
    {
        static const char *labels[BROWSE_NAV_BUTTONS] = { "ATRÁS", "BUSCAR", "INICIO" };
        int nav_y = BrowseNavY(), nav_h = BrowseNavH();
        for (int i = 0; i < BROWSE_NAV_BUTTONS; i++) {
            int bx, bw;
            BrowseNavButtonRect(i, &bx, &bw);
            int disabled = (i == 2 && !HomeButtonUseful());
            DrawButton(bx, nav_y, bw, nav_h, T(labels[i]), disabled ? BUTTON_DISABLED : 0);
        }
    }

    int list_start_y = hh + gap;
    int list_avail_h = BrowseListBottom() - list_start_y;
    int dynamic_row_h = list_avail_h / entries_per_page;
    int y = list_start_y; 
    
    if (entry_count == 0) {
        if (catalog_error[0]) {
            SetTextFont(32, 0x000000);
            DrawTextRect(30, sys_height / 3 - 40, sys_width - 60, 240,
                         catalog_error, ALIGN_CENTER);
        } else {
            SetTextFont(36, 0x000000);
            DrawTextRect(0, sys_height / 3, sys_width, 60,
                         T("No se encontraron elementos."), ALIGN_CENTER);
        }
    } else {
        int start = current_page * entries_per_page, end = start + entries_per_page;
        if (end > entry_count) end = entry_count;
        
        // Calculate font sizes using a dampened scale so they don't blow up at low rows
        float scale = (float)dynamic_row_h / (float)row_h;
        float font_scale = 1.0f + ((scale - 1.0f) * 0.4f);
        
        int title_font_size = (int)(36 * font_scale);
        if (title_font_size > 44) title_font_size = 44; // Cap the maximum font size
        
        int author_font_size = (int)(24 * font_scale);
        if (author_font_size > 32) author_font_size = 32; // Cap the maximum font size

        int icon_h = dynamic_row_h - 10, book_w = (int)(icon_h / 1.4); 
        int text_x = 20 + icon_h + 15, text_w = sys_width - text_x - 20;

        for (int i = start; i < end; i++) {
            if (y + dynamic_row_h > BrowseListBottom()) break;
            int icon_y = y + 5;

            if (current_entries[i].is_book) {
                int b_x = 20 + ((icon_h - book_w) / 2);
                if (servers[current_server_index].fetch_thumbs && list_thumbs[i]) DrawSmartBitmap(b_x, icon_y, book_w, icon_h, list_thumbs[i], STRETCH, 1);
                else if (book_fallback_bmp) DrawSmartBitmap(b_x, icon_y, book_w, icon_h, book_fallback_bmp, STRETCH, 0);
                else { FillArea(b_x, icon_y, book_w, icon_h, 0xDDDDDD); DrawRect(b_x, icon_y, book_w, icon_h, 0x000000); }
            } else {
                if (folder_icon_bmp) {
                    int folder_w = (icon_h * folder_icon_bmp->width) / folder_icon_bmp->height;
                    int f_x = 20 + ((icon_h - folder_w) / 2); 
                    DrawSmartBitmap(f_x, icon_y, folder_w, icon_h, folder_icon_bmp, STRETCH, 0);
                } else { 
                    FillArea(20, icon_y, icon_h, icon_h, 0x999999); DrawRect(20, icon_y, icon_h, icon_h, 0x000000); 
                }
            }

            if (current_entries[i].is_book) {
                SetTextFont(title_font_size, 0x000000); 
                
                // Allow the title to dynamically wrap up to a maximum height
                int max_title_h = dynamic_row_h - author_font_size - 25; // Reserve room at the bottom for the author
                int actual_title_h = TextRectHeight(text_w, current_entries[i].title, ALIGN_LEFT);
                if (actual_title_h > max_title_h) actual_title_h = max_title_h;
                
                // Draw wrapped title, adding DOTS only if it hits the maximum allowed height
                DrawTextRect(text_x, y + 10, text_w, actual_title_h, current_entries[i].title, ALIGN_LEFT | DOTS);
                
                if (strlen(current_entries[i].author) > 0) { 
                    SetTextFont(author_font_size, 0x555555); 
                    // Pin author right below the title text
                    DrawTextRect(text_x, y + 10 + actual_title_h + 5, text_w, author_font_size + 10, current_entries[i].author, ALIGN_LEFT | DOTS); 
                }
            } else {
                SetTextFont(title_font_size, 0x000000); 
                
                // For folders, use the entire height to allow deep wrapping
                int max_title_h = dynamic_row_h - 20;
                int actual_title_h = TextRectHeight(text_w, current_entries[i].title, ALIGN_LEFT);
                if (actual_title_h > max_title_h) actual_title_h = max_title_h;
                
                // Vertically center the folder text
                int text_y = y + (dynamic_row_h - actual_title_h) / 2;
                DrawTextRect(text_x, text_y, text_w, actual_title_h, current_entries[i].title, ALIGN_LEFT | DOTS);
            }

            DrawLine(20, y + dynamic_row_h, sys_width - 20, y + dynamic_row_h, 0xAAAAAA);
            y += dynamic_row_h;
        }

        char page_txt[128]; 
        int absolute_page = current_global_offset + current_page + 1;
        int local_max_pages = (entry_count + entries_per_page - 1) / entries_per_page;
        
        if (total_results > 0) {
            int absolute_max_pages = (total_results + entries_per_page - 1) / entries_per_page;
            snprintf(page_txt, sizeof(page_txt), T("Página %d / %d   |   Catálogo total: %d"), absolute_page, absolute_max_pages, total_results);
        } else {
            int known_max = current_global_offset + local_max_pages;
            if (strlen(next_page_url) == 0) {
                snprintf(page_txt, sizeof(page_txt), T("Página %d / %d"), absolute_page, known_max);
            } else {
                snprintf(page_txt, sizeof(page_txt), T("Página %d / %d+"), absolute_page, known_max);
            }
        }
        
        SetTextFont(24, 0x555555); 
        DrawTextRect(0, BrowseNavY() - 45, sys_width, 30, page_txt, ALIGN_CENTER);
        
        /* Siempre visibles; en gris cuando no hay página a la que ir. */
        DrawButton(20, footer_y, sys_width/2 - 30, row_h, T("<< ANTERIOR"),
                   PreviousPageAvailable() ? 0 : BUTTON_DISABLED);
        if (end >= entry_count && strlen(next_page_url) > 0)
            DrawButton(sys_width/2 + 10, footer_y, sys_width/2 - 30, row_h, T("MÁS >>"), 0);
        else
            DrawButton(sys_width/2 + 10, footer_y, sys_width/2 - 30, row_h, T("SIGUIENTE >>"),
                       NextPageAvailable() ? 0 : BUTTON_DISABLED);
    }
}

void HandleHardwareButtons(int key) {
    if (current_state == STATE_BOOK_DETAILS) {
        int scroll_amount = 150, moved = 0;
        if (key == IV_KEY_NEXT || key == 0x207) { summary_scroll_y += scroll_amount; moved = 1; } 
        else if (key == IV_KEY_PREV || key == 0x206) { summary_scroll_y -= scroll_amount; if (summary_scroll_y < 0) summary_scroll_y = 0; moved = 1; }
        
        if (moved) {
            FillArea(view_x, view_y, view_w, view_h, 0xFFFFFF);
            char detail_summary[MAX_STR_LEN * 3];
            BuildBookDetailSummary(&current_entries[selected_entry_index],
                                   detail_summary, sizeof(detail_summary));
            SetTextFont(30, 0x000000); SetClip(view_x, view_y, view_w, view_h);
            DrawTextRect(view_x, view_y - summary_scroll_y, view_w, 2000,
                         detail_summary, ALIGN_LEFT);
            SetClip(0, 0, sys_width, sys_height); PartialUpdate(view_x, view_y, view_w, view_h);
        }
        return; 
    }

    if (current_state != STATE_BROWSING) return;
    
    int entries_per_page = GetCatalogRows();
    
    if (key == IV_KEY_NEXT || key == 0x207) {
        if ((current_page + 1) * entries_per_page < entry_count) {
            current_page++; JIT_DecodePageThumbnails(); Repaint(); FetchThumbnailsForPage();   
        } else if (strlen(next_page_url) > 0) {
            if (page_stack_ptr < MAX_PAGE_STACK) {
                strncpy(page_stack[page_stack_ptr], last_loaded_url, MAX_STR_LEN - 1);
                page_offset_stack[page_stack_ptr] = current_global_offset;
                page_stack_ptr++;
            }
            current_global_offset += (entry_count + entries_per_page - 1) / entries_per_page;
            LoadCatalog(next_page_url);
        }
    } else if (key == IV_KEY_PREV || key == 0x206) {
        if (current_page > 0) {
            current_page--; JIT_DecodePageThumbnails(); Repaint(); FetchThumbnailsForPage();
        } else if (page_stack_ptr > 0) {
            page_stack_ptr--; 
            return_to_last_page = 1; 
            current_global_offset = page_offset_stack[page_stack_ptr];
            LoadCatalog(page_stack[page_stack_ptr]);
        }
    } else if (key == IV_KEY_MENU) {
        static char page_input_buffer[16] = {0};
        memset(page_input_buffer, 0, sizeof(page_input_buffer));
        OpenKeyboard(T("Ir a la página:"), page_input_buffer, 15, 0, JumpToPageCallback);
    }
}

void HandleBrowsingTouch(int x, int y) {
    int entries_per_page = GetCatalogRows();
    int hh = sys_height / 14, row_h = sys_height / 13, gap = sys_height / 60, footer_y = sys_height - row_h - 10;

    if (y <= hh) {
        /* Tocar el título vuelve a las opciones del servidor. */
        FlashArea(0, 0, sys_width, hh);
        nav_stack_ptr = 0; current_level_label[0] = '\0'; page_stack_ptr = 0; current_global_offset = 0; current_state = STATE_SERVER_OPTIONS; Repaint();
        return;
    }

    int nav_y = BrowseNavY(), nav_h = BrowseNavH();
    if (y >= nav_y && y <= nav_y + nav_h) {
        for (int i = 0; i < BROWSE_NAV_BUTTONS; i++) {
            int bx, bw;
            BrowseNavButtonRect(i, &bx, &bw);
            if (x < bx || x > bx + bw) continue;
            if (i == 2 && !HomeButtonUseful()) return;   /* Inicio en gris */
            FlashArea(bx, nav_y, bw, nav_h);
            switch (i) {
                case 0: /* Atrás */
                    GoBackInHierarchy();
                    break;
                case 1: /* Buscar */
                    memset(search_input_buffer, 0, sizeof(search_input_buffer));
                    OpenKeyboard(T("Buscar en el catálogo:"), search_input_buffer, 255, 0, PerformSearch);
                    break;
                case 2: /* Inicio */
                    nav_stack_ptr = 0; current_level_label[0] = '\0'; page_stack_ptr = 0; current_global_offset = 0; current_page = 0;
                    LoadCatalog(servers[current_server_index].url);
                    break;
            }
            return;
        }
        return;
    }
    
    if (y >= footer_y) {
        if (x < sys_width / 2 && !PreviousPageAvailable()) return;
        if (x >= sys_width / 2 && !NextPageAvailable()) return;
        if (x < sys_width / 2) { 
            FlashArea(20, footer_y, sys_width/2 - 30, row_h);
            if (current_page > 0) { 
                current_page--; JIT_DecodePageThumbnails(); Repaint(); FetchThumbnailsForPage(); 
            } else if (page_stack_ptr > 0) { 
                page_stack_ptr--; return_to_last_page = 1; current_global_offset = page_offset_stack[page_stack_ptr]; LoadCatalog(page_stack[page_stack_ptr]); 
            }
        } else if (x >= sys_width / 2) {
            FlashArea(sys_width/2 + 10, footer_y, sys_width/2 - 30, row_h);
            if ((current_page + 1) * entries_per_page < entry_count) { 
                current_page++; JIT_DecodePageThumbnails(); Repaint(); FetchThumbnailsForPage(); 
            } else if (strlen(next_page_url) > 0) {
                if (page_stack_ptr < MAX_PAGE_STACK) {
                    strncpy(page_stack[page_stack_ptr], last_loaded_url, MAX_STR_LEN - 1);
                    page_offset_stack[page_stack_ptr] = current_global_offset;
                    page_stack_ptr++;
                }
                current_global_offset += (entry_count + entries_per_page - 1) / entries_per_page;
                LoadCatalog(next_page_url);
            }
        } 
        return;
    }
    
    int list_start_y = hh + gap;
    int list_avail_h = BrowseListBottom() - list_start_y;
    int dynamic_row_h = list_avail_h / entries_per_page;
    int start = current_page * entries_per_page, ly = list_start_y;
    for (int i = start; i < start + entries_per_page && i < entry_count; i++) {
        if (y >= ly && y <= ly + dynamic_row_h) {
            OPDSEntry *e = &current_entries[i];

            // Visual e-ink feedback flash
            FlashArea(0, ly, sys_width, dynamic_row_h);

            if (strlen(e->nav_url) > 0 && e->is_book == 0) {
                PushNavigationLevel(e->title);
                page_stack_ptr = 0; current_global_offset = 0; return_to_last_page = 0; LoadCatalog(e->nav_url);
            } else if (e->is_book) {
                selected_entry_index = i; UnloadGlobalCover();
                if (IsEbiblioURL(servers[current_server_index].url) &&
                    e->ebiblio_self_url[0] && !e->ebiblio_details_loaded) {
                    FillArea(0, 0, sys_width, sys_height, 0xFFFFFF);
                    SetTextFont(36, 0x000000);
                    DrawTextRect(0, sys_height / 2 - 30, sys_width, 60,
                                 T("Consultando ficha de eBiblio..."), ALIGN_CENTER);
                    FullUpdate();
                    if (EnsureNetwork() == 0) {
                        char details_error[MAX_STR_LEN] = {0};
                        if (EbiblioLoadPublicationDetails(
                                e, servers[current_server_index].user,
                                servers[current_server_index].pass,
                                details_error, sizeof(details_error)) != 0) {
                            Message(ICON_WARNING, "eBiblio",
                                    details_error[0] ? details_error :
                                    T("No se pudieron cargar las acciones del préstamo."),
                                    5000);
                        }
                    }
                }
                if (strlen(e->cover_url) > 10) {
                    char c_path[MAX_STR_LEN]; snprintf(c_path, sizeof(c_path), "%scache_%lu.jpg", IMAGES_DIR, hash_str(e->cover_url));
                    if (EnsureNetwork() == 0) {
                        struct stat st; 
                        int image_result = stat(c_path, &st) == 0 ? 0 :
                            (IsEbiblioURL(servers[current_server_index].url)
                                ? EbiblioDownload(e->cover_url, c_path,
                                                  servers[current_server_index].user,
                                                  servers[current_server_index].pass, NULL, 0)
                                : DownloadImage(e->cover_url, c_path,
                                                servers[current_server_index].user,
                                                servers[current_server_index].pass));
                        if (image_result == 0) {
                            current_cover_bmp = LoadCoverSTB(c_path);
                        }
                    }
                }
                summary_scroll_y = 0;
                details_list_scroll = 0;
                current_state = STATE_BOOK_DETAILS;
                Repaint();
            } 
            return;
        } 
        ly += dynamic_row_h; if (ly + dynamic_row_h > BrowseListBottom()) break;
    }
}

static int DetailsListTop(void) {
    int m = sys_width / 20;
    int thumb_w = sys_width / 3;
    int thumb_h = (int)(thumb_w * 1.4);
    int top = m + 180 + thumb_h + 40;
    int bottom = sys_height - 120;
    if (top + 90 > bottom) top = bottom - 90;
    return top;
}

static int DetailsListVisibleCount(int list_top) {
    int available = sys_height - 120 - list_top;
    if (available < 90) return 1;
    return 1 + (available - 90) / 100;
}

static int DetailsListItemCount(const OPDSEntry *entry) {
    return EbiblioActionCount(entry) + entry->format_count;
}

static const char *KnownEbiblioExtension(const OPDSEntry *entry, int format_index,
                                         const char *filename,
                                         const char *content_type) {
    if (content_type) {
        if (strstr(content_type, "lcp")) return "lcpl";
        if (strstr(content_type, "adept") || strstr(content_type, "acsm")) return "acsm";
        if (strstr(content_type, "epub")) return "epub";
        if (strstr(content_type, "pdf")) return "pdf";
    }

    const char *sources[] = {filename, entry->formats[format_index].url};
    const char *extensions[] = {"lcpl", "acsm", "epub", "pdf"};
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        if (!sources[i]) continue;
        const char *dot = strrchr(sources[i], '.');
        if (!dot) continue;
        const char *query = strchr(dot, '?');
        size_t length = query ? (size_t)(query - dot - 1) : strlen(dot + 1);
        for (size_t j = 0; j < sizeof(extensions) / sizeof(extensions[0]); j++) {
            if (length == strlen(extensions[j]) &&
            strncasecmp(dot + 1, extensions[j], length) == 0) {
                return extensions[j];
            }
        }
    }

    const char *label = entry->formats[format_index].label;
    if (strcmp(label, "LCP") == 0) return "lcpl";
    if (strcmp(label, "ACSM") == 0) return "acsm";
    if (strcmp(label, "EPUB") == 0) return "epub";
    if (strcmp(label, "PDF") == 0) return "pdf";
    return NULL;
}

static int HasKnownExtension(const char *filename) {
    if (!filename) return 0;
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    return strcasecmp(dot + 1, "lcpl") == 0 ||
           strcasecmp(dot + 1, "acsm") == 0 ||
           strcasecmp(dot + 1, "epub") == 0 ||
           strcasecmp(dot + 1, "pdf") == 0;
}

static void BuildBookDetailSummary(const OPDSEntry *entry, char *output,
                                   size_t output_size) {
    if (!output || output_size == 0) return;
    output[0] = '\0';
    if (!entry) return;

    char availability_info[MAX_STR_LEN] = {0};
    if (entry->ebiblio_availability_until[0]) {
        const char *label = strcmp(entry->ebiblio_status, "ready") == 0
            ? T("Confirmar antes de")
            : strcmp(entry->ebiblio_status, "available") == 0
            ? T("Préstamo hasta")
            : T("Disponibilidad estimada");
        snprintf(availability_info, sizeof(availability_info), "%s: %s",
                 label, entry->ebiblio_availability_until);
    }

    size_t used = strlen(availability_info);
    if (entry->ebiblio_holds_total > 0) {
        snprintf(availability_info + used, sizeof(availability_info) - used,
                 T("%sReservas en cola: %d"),
                 used ? "\n" : "", entry->ebiblio_holds_total);
        used = strlen(availability_info);
    }
    if (entry->ebiblio_hold_position > 0) {
        snprintf(availability_info + used, sizeof(availability_info) - used,
                 T("%sTu posición: %d"),
                 used ? "\n" : "", entry->ebiblio_hold_position);
    }

    if (availability_info[0]) {
        snprintf(output, output_size, "%s%s%s",
                 availability_info,
                 entry->summary[0] ? "\n\n" : "",
                 entry->summary);
    } else {
        strncpy(output, entry->summary, output_size - 1);
        output[output_size - 1] = '\0';
    }
}

void DrawBookDetails() {
    OPDSEntry *e = &current_entries[selected_entry_index];
    int m = sys_width / 20, thumb_w = sys_width / 3, thumb_h = (int)(thumb_w * 1.4);
    
    SetTextFont(48, 0x000000); DrawTextRect(m, m, sys_width - (m * 2), 100, e->title, ALIGN_LEFT);
    SetTextFont(36, 0x555555); DrawTextRect(m, m + 110, sys_width - (m * 2), 50, e->author, ALIGN_LEFT);
    
    int thumb_y = m + 180; FillArea(m, thumb_y, thumb_w, thumb_h, 0xFFFFFF); 
    if (current_cover_bmp) { DrawSmartBitmap(m, thumb_y, thumb_w, thumb_h, current_cover_bmp, STRETCH, 1); } 
    else { FillArea(m, thumb_y, thumb_w, thumb_h, 0xDDDDDD); SetTextFont(24, 0x000000); DrawTextRect(m, thumb_y + (thumb_h / 2) - 20, thumb_w, 40, "[SIN PORTADA]", ALIGN_CENTER); }
    
    SetTextFont(30, 0x000000); 
    view_x = m * 2 + thumb_w; view_y = thumb_y; view_w = sys_width - (m * 3 + thumb_w); view_h = thumb_h;
    SetClip(view_x, view_y, view_w, view_h);
    char detail_summary[MAX_STR_LEN * 3];
    BuildBookDetailSummary(e, detail_summary, sizeof(detail_summary));
    DrawTextRect(view_x, view_y - summary_scroll_y, view_w, 2000,
                 detail_summary, ALIGN_LEFT);
    SetClip(0, 0, sys_width, sys_height);
    
    int action_count = EbiblioActionCount(e);
    int item_count = DetailsListItemCount(e);
    int list_top = DetailsListTop();
    int visible_count = DetailsListVisibleCount(list_top);
    int max_scroll = item_count - visible_count;
    if (max_scroll < 0) max_scroll = 0;
    if (details_list_scroll > max_scroll) details_list_scroll = max_scroll;

    for (int i = details_list_scroll;
         i < item_count && i < details_list_scroll + visible_count; i++) {
        int by = list_top + (i - details_list_scroll) * 100;
        if (i < action_count) {
            DrawButton(m, by, sys_width - (m * 2), 90,
                       EbiblioActionLabel(e, i), 0);
        } else {
            char b[64];
            snprintf(b, sizeof(b), T("Descargar %s"),
                     e->formats[i - action_count].label);
            DrawButton(m, by, sys_width - (m * 2), 90, b, 0);
        }
    }

    if (item_count > visible_count) {
        int gap = 8;
        int button_w = (sys_width - (m * 2) - (gap * 2)) / 3;
        int footer_y = sys_height - 110;
        DrawButton(m, footer_y, button_w, 90, T("Anterior"), 0);
        DrawButton(m + button_w + gap, footer_y, button_w, 90, T("Atrás"), 1);
        DrawButton(m + (button_w + gap) * 2, footer_y, button_w, 90,
                   T("Siguiente"), 0);
    } else {
        DrawButton(m, sys_height - 110, sys_width - (m * 2), 90, T("Atrás"), 1);
    }
    SoftUpdate();
}

void HandleBookDetailsTouch(int x, int y) {
    OPDSEntry *e = &current_entries[selected_entry_index];
    int m = sys_width / 20;
    int action_count = EbiblioActionCount(e);
    int item_count = DetailsListItemCount(e);
    int list_top = DetailsListTop();
    int visible_count = DetailsListVisibleCount(list_top);
    int max_scroll = item_count - visible_count;
    if (max_scroll < 0) max_scroll = 0;
    if (details_list_scroll > max_scroll) details_list_scroll = max_scroll;

    for (int i = details_list_scroll;
         i < item_count && i < details_list_scroll + visible_count; i++) {
        int by = list_top + (i - details_list_scroll) * 100;
        if (y >= by && y <= by + 90) {
            FlashArea(m, by, sys_width - (m * 2), 90);
            if (i < action_count) {
                const char *action_url = EbiblioActionURL(e, i);
                int use_delete = e->ebiblio_cancel_uses_delete &&
                    strcmp(action_url, e->ebiblio_cancel_url) == 0;
                int use_put = e->ebiblio_return_uses_put &&
                    strcmp(action_url, e->ebiblio_return_url) == 0;
                RequestEbiblioAction(action_url, EbiblioActionLabel(e, i),
                                     use_delete, use_put);
                return;
            }

            int format_index = i - action_count;
            if (EnsureNetwork() != 0) { Repaint(); return; }

            char tmp_fname[MAX_STR_LEN], server_fname[256] = {0};
            char response_filename[256] = {0};
            char response_content_type[128] = {0};
            snprintf(tmp_fname, sizeof(tmp_fname), "dl_%ld.tmp", (long)time(NULL));
            char tmp_path[MAX_STR_LEN]; snprintf(tmp_path, sizeof(tmp_path), "%s%s", BOOKS_DIR, tmp_fname);
            Repaint(); 
            
            char download_error[MAX_STR_LEN] = {0};
            int is_ebiblio = IsEbiblioURL(servers[current_server_index].url);
            int res = is_ebiblio
                ? EbiblioDownloadWithInfo(e->formats[format_index].url, tmp_path,
                                          servers[current_server_index].user,
                                          servers[current_server_index].pass,
                                          response_filename, sizeof(response_filename),
                                          response_content_type, sizeof(response_content_type),
                                          download_error, sizeof(download_error))
                : DownloadBook(e->formats[format_index].url, tmp_path, server_fname,
                               servers[current_server_index].user,
                               servers[current_server_index].pass);
            if (res == 0) { 
                char license_error[MAX_STR_LEN] = {0};
                int is_lcp_license = is_ebiblio &&
                    (strcmp(e->formats[format_index].label, "LCP") == 0 ||
                     EbiblioValidateLicenseFile(tmp_path, license_error,
                                                sizeof(license_error)) == 0);
                int unknown_format = 0;
                if (is_lcp_license) {
                    snprintf(server_fname, sizeof(server_fname), "%s.%s",
                             e->title, "lcpl");
                } else if (is_ebiblio && response_filename[0]) {
                    strncpy(server_fname, response_filename, sizeof(server_fname) - 1);
                    server_fname[sizeof(server_fname) - 1] = '\0';
                    if (!HasKnownExtension(server_fname)) {
                        const char *extension = KnownEbiblioExtension(
                            e, format_index, response_filename, response_content_type);
                        if (extension) {
                            size_t used = strlen(server_fname);
                            snprintf(server_fname + used, sizeof(server_fname) - used,
                                     ".%s", extension);
                        } else {
                            unknown_format = 1;
                        }
                    }
                }
                if (server_fname[0] == '\0') {
                    const char *extension = is_ebiblio
                        ? KnownEbiblioExtension(e, format_index, response_filename,
                                                response_content_type)
                        : e->formats[format_index].label;
                    if (!extension) {
                        extension = "bin";
                        unknown_format = 1;
                    }
                    snprintf(server_fname, sizeof(server_fname), "%s.%s",
                             e->title, extension);
                }
                for(size_t j=0; server_fname[j]; j++) { if(server_fname[j] == '/' || server_fname[j] == '\\' || server_fname[j] == ':') server_fname[j] = '-'; }
                char final_path[MAX_STR_LEN]; snprintf(final_path, sizeof(final_path), "%s%s", BOOKS_DIR, server_fname);
                if (rename(tmp_path, final_path) != 0) {
                    remove(tmp_path);
                    Message(ICON_ERROR, "Error", T("El libro se descargó, pero no se pudo guardar."), 4000);
                } else if (is_lcp_license) {
                    if (EbiblioValidateLicenseFile(final_path, license_error,
                                                   sizeof(license_error)) != 0) {
                        char message[MAX_STR_LEN * 2];
                        snprintf(message, sizeof(message), "%s\n\nGuardado en:\n%s",
                                 license_error[0] ? license_error : T("Licencia LCP no válida."),
                                 final_path);
                        Message(ICON_ERROR, T("Archivo LCP"), message, 7000);
                        Repaint();
                        return;
                    }
                    if (TriggerLibraryScan() < 0) {
                        Message(ICON_WARNING, T("Licencia LCP guardada"),
                                T("No se pudo iniciar el escaneo. Busca la licencia en Descargas y solicita actualizar la biblioteca."),
                                7000);
                    } else {
                        char message[MAX_STR_LEN * 2];
                        snprintf(message, sizeof(message),
                                 T("Se solicitó escanear la licencia:\n%s\nCuando aparezca en la biblioteca, pulsa su icono para que el lector la procese."),
                                 final_path);
                        Message(ICON_INFORMATION, T("Licencia LCP guardada"), message, 7000);
                    }
                    Repaint();
                    return;
                } else if (unknown_format) {
                    Message(ICON_WARNING, T("Descarga guardada"),
                            T("eBiblio no indicó el formato del archivo. Se guardó, pero no se solicitó su indexación."),
                            6000);
                } else if (TriggerLibraryScan() < 0) {
                    Message(ICON_WARNING, T("Descarga completada"),
                            T("El libro se guardó, pero no se pudo iniciar el escaneo de la biblioteca."), 5000);
                } else {
                    Message(ICON_INFORMATION, T("Descarga completada"),
                            T("Se solicitó actualizar la biblioteca; puede tardar unos segundos."), 3000);
                }
            } else {
                remove(tmp_path);
                Message(ICON_ERROR, "Error",
                        download_error[0] ? download_error : T("Error al descargar."), 5000);
            }
            Repaint(); return;
        }
    }

    if (y >= sys_height - 110) {
        int footer_y = sys_height - 110;
        if (item_count > visible_count) {
            int gap = 8;
            int button_w = (sys_width - (m * 2) - (gap * 2)) / 3;
            int page_step = visible_count;
            if (x >= m && x < m + button_w) {
                if (details_list_scroll > 0) {
                    details_list_scroll -= page_step;
                    if (details_list_scroll < 0) details_list_scroll = 0;
                    Repaint();
                }
                return;
            }
            if (x >= m + (button_w + gap) * 2 &&
                x <= m + (button_w + gap) * 2 + button_w) {
                if (details_list_scroll < max_scroll) {
                    details_list_scroll += page_step;
                    if (details_list_scroll > max_scroll) details_list_scroll = max_scroll;
                    Repaint();
                }
                return;
            }
            if (x < m + button_w + gap ||
                x > m + (button_w + gap) * 2) return;
        }
        FlashArea(m, footer_y, sys_width - (m * 2), 90);
        UnloadGlobalCover(); current_state = STATE_BROWSING; Repaint(); 
    }
}

static int EbiblioActionCount(const OPDSEntry *entry) {
    if (!entry || !IsEbiblioURL(servers[current_server_index].url)) return 0;
    return (entry->ebiblio_accept_url[0] != '\0') +
           EbiblioHasBorrowAction(entry) +
           (entry->ebiblio_cancel_url[0] != '\0') +
           (entry->ebiblio_return_url[0] != '\0');
}

static int EbiblioHasBorrowAction(const OPDSEntry *entry) {
    if (!entry || !entry->ebiblio_borrow_url[0]) return 0;
    return strcmp(entry->ebiblio_status, "reserved") != 0 &&
           strcmp(entry->ebiblio_status, "ready") != 0 &&
           entry->ebiblio_cancellable <= 0 &&
           !entry->ebiblio_cancel_url[0] &&
           entry->ebiblio_hold_position <= 0;
}

static const char *EbiblioActionURL(const OPDSEntry *entry, int index) {
    if (!entry) return "";
    const char *actions[] = {
        entry->ebiblio_accept_url,
        EbiblioHasBorrowAction(entry) ? entry->ebiblio_borrow_url : "",
        entry->ebiblio_cancel_url,
        entry->ebiblio_return_url
    };
    for (int i = 0; i < 4; i++) {
        if (!actions[i][0]) continue;
        if (index-- == 0) return actions[i];
    }
    return "";
}

static const char *EbiblioActionLabel(const OPDSEntry *entry, int index) {
    if (!entry) return T("Acción");
    const char *labels[] = {
        T("Aceptar préstamo"),
        strcmp(entry->ebiblio_status, "unavailable") == 0 ? T("Reservar") : T("Pedir préstamo"),
        T("Cancelar reserva"),
        T("Devolver préstamo")
    };
    const char *actions[] = {
        entry->ebiblio_accept_url,
        EbiblioHasBorrowAction(entry) ? entry->ebiblio_borrow_url : "",
        entry->ebiblio_cancel_url,
        entry->ebiblio_return_url
    };
    for (int i = 0; i < 4; i++) {
        if (!actions[i][0]) continue;
        if (index-- == 0) return labels[i];
    }
    return T("Acción");
}

static void ConfirmEbiblioAction(int button) {
    if (button != 1 || !pending_ebiblio_action[0]) {
        pending_ebiblio_action[0] = '\0';
        pending_ebiblio_action_uses_delete = 0;
        pending_ebiblio_action_uses_put = 0;
        Repaint();
        return;
    }

    char error[MAX_STR_LEN] = {0};
    int result = pending_ebiblio_action_uses_delete
        ? EbiblioDeleteAction(pending_ebiblio_action,
                              servers[current_server_index].user,
                              servers[current_server_index].pass,
                              error, sizeof(error))
        : pending_ebiblio_action_uses_put
        ? EbiblioPutAction(pending_ebiblio_action,
                           servers[current_server_index].user,
                           servers[current_server_index].pass,
                           error, sizeof(error))
        : EbiblioPostAction(pending_ebiblio_action,
                            servers[current_server_index].user,
                            servers[current_server_index].pass,
                            error, sizeof(error));
    pending_ebiblio_action[0] = '\0';
    pending_ebiblio_action_uses_delete = 0;
    pending_ebiblio_action_uses_put = 0;
    if (result != 0) {
        Message(ICON_ERROR, "eBiblio",
                error[0] ? error : T("No se pudo completar la operación."), 5000);
        Repaint();
        return;
    }
    LoadCatalog(last_loaded_url);
}

static void RequestEbiblioAction(const char *url, const char *label,
                                 int use_delete, int use_put) {
    if (!url || !url[0] || strlen(url) >= sizeof(pending_ebiblio_action)) {
        Message(ICON_ERROR, "eBiblio", T("El enlace de la operación no es válido."), 4000);
        Repaint();
        return;
    }
    strncpy(pending_ebiblio_action, url, sizeof(pending_ebiblio_action) - 1);
    pending_ebiblio_action[sizeof(pending_ebiblio_action) - 1] = '\0';
    pending_ebiblio_action_uses_delete = use_delete;
    pending_ebiblio_action_uses_put = use_put;
    char text[256];
    snprintf(text, sizeof(text), T("¿Confirmas esta operación?\n\n%s"), label ? label : T("Acción"));
    Dialog(ICON_QUESTION, T("Confirmar en eBiblio"), text,
           T("Confirmar"), T("Cancelar"), ConfirmEbiblioAction);
}
