#ifndef OPDS_APP_H
#define OPDS_APP_H

#include <inkview.h>
#include <stddef.h> 

// libxml2 headers for OPDS parsing
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

// --- Constants ---
#define MAX_SERVERS 10
#define MAX_STR_LEN 512
#define MAX_ENTRIES 1500
#define MAX_FORMATS 5

#define APP_ROOT_DIR "/mnt/ext1/applications/Pergamo/"
#define RESTART_PENDING_FILE APP_ROOT_DIR "launcher_restart_pending"
#define IMAGES_DIR APP_ROOT_DIR "images/"
#define BOOKS_DIR "/mnt/ext1/Downloads/"

// New and Legacy Config File Locations
#define NEW_CFG_FILE APP_ROOT_DIR "opds_client.cfg"
#define LEGACY_CFG_FILE "/mnt/ext1/system/config/opds_client.cfg"

#define FOLDER_ICON IMAGES_DIR "folder.png"
#define BOOK_ICON IMAGES_DIR "book.png"

#define PERGAMO_VERSION "1.1"
#define DEBUG_LOG_TRIGGER APP_ROOT_DIR "LOGTRIGGER.TXT"
#define APP_USER_AGENT "PocketBook-OPDS-Expert/1.0 (PocketBook Era; Linux)"

typedef enum AppState {
    STATE_MAIN_MENU,
    STATE_SERVER_OPTIONS,
    STATE_SERVER_FORM,
    STATE_BROWSING,
    STATE_BOOK_DETAILS,
    STATE_UNINSTALL_CONFIRM,
    STATE_SETTINGS
} AppState;

typedef struct {
    char name[MAX_STR_LEN];
    char url[MAX_STR_LEN];
    char user[MAX_STR_LEN];
    char pass[MAX_STR_LEN];
    int fetch_thumbs; 
    int catalog_rows;
    /* Contraseña tal como está guardada (cifrada) y si sigue bloqueada por PIN. */
    char pass_stored[MAX_STR_LEN * 2];
    int pass_locked;
} OPDSServer;

typedef struct {
    char label[64];
    char url[MAX_STR_LEN];
} BookFormat;

typedef struct {
    char title[MAX_STR_LEN];
    char author[MAX_STR_LEN];
    char summary[1024];
    char cover_url[MAX_STR_LEN];
    char thumb_url[MAX_STR_LEN];
    char nav_url[MAX_STR_LEN];
    char ebiblio_borrow_url[MAX_STR_LEN];
    char ebiblio_cancel_url[MAX_STR_LEN];
    char ebiblio_return_url[MAX_STR_LEN];
    char ebiblio_accept_url[MAX_STR_LEN];
    char ebiblio_self_url[MAX_STR_LEN];
    char ebiblio_status[64];
    char ebiblio_availability_until[64];
    BookFormat formats[MAX_FORMATS];
    int format_count;
    int is_book;
    int ebiblio_available;
    int ebiblio_holds_total;
    int ebiblio_hold_position;
    int ebiblio_cancellable;
    int ebiblio_cancel_uses_delete;
    int ebiblio_return_uses_put;
    int ebiblio_details_loaded;
} OPDSEntry;

// --- Global Variables (Externs) ---
extern AppState current_state;
extern int server_count;
extern int current_server_index;
extern int editing_server_index;
extern int selected_entry_index;
extern OPDSServer servers[MAX_SERVERS];
extern OPDSServer temp_server;
extern OPDSEntry current_entries[MAX_ENTRIES];
extern int entry_count;
extern char current_host[MAX_STR_LEN];
extern int sys_width, sys_height;
extern char current_search_url[MAX_STR_LEN];
extern char next_page_url[MAX_STR_LEN];
extern char current_feed_title[MAX_STR_LEN];
extern int total_results;

// --- Function Prototypes ---
struct MemoryStruct { char *memory; size_t size; };
int FetchFeed(const char *url, const char *user, const char *pass,
              struct MemoryStruct *chunk, char *error, size_t error_size);
int IsEbiblioURL(const char *url);
int EbiblioFetchFeed(const char *url, const char *user, const char *pass,
                     int require_auth, struct MemoryStruct *chunk,
                     char *error, size_t error_size);
int EbiblioPostAction(const char *url, const char *user, const char *pass,
                      char *error, size_t error_size);
int EbiblioDeleteAction(const char *url, const char *user, const char *pass,
                        char *error, size_t error_size);
int EbiblioPutAction(const char *url, const char *user, const char *pass,
                     char *error, size_t error_size);
int EbiblioDownload(const char *url, const char *filepath,
                    const char *user, const char *pass,
                    char *error, size_t error_size);
int EbiblioDownloadWithInfo(const char *url, const char *filepath,
                            const char *user, const char *pass,
                            char *filename, size_t filename_size,
                            char *content_type, size_t content_type_size,
                            char *error, size_t error_size);
int EbiblioValidateLicenseFile(const char *filepath, char *error, size_t error_size);
int ParseEbiblioFeed(const char *json_data, const char *base_url);
int EbiblioLoadPublicationDetails(OPDSEntry *entry, const char *user, const char *pass,
                                  char *error, size_t error_size);
int FindEbiblioAccountURL(const char *json_data, const char *base_url,
                          char *account_url, size_t account_url_size);
int DownloadImage(const char *url, const char *filepath, const char *user, const char *pass);
int DownloadBook(const char *url, const char *tmp_path, char *out_filename, const char *user, const char *pass);

int ParseOPDSFeed(const char *xml_data, const char *base_url);

void DrawMainMenu();
void HandleMainMenuTouch(int x, int y);
void DrawServerOptions();
void HandleServerOptionsTouch(int x, int y);
void DrawServerForm();
void HandleServerFormTouch(int x, int y);
void DrawBrowsingView();
void HandleBrowsingTouch(int x, int y);
void DrawBookDetails();
void HandleBookDetailsTouch(int x, int y);
void DrawSettings();
void HandleSettingsTouch(int x, int y);
void DrawUninstallConfirmation();
void HandleUninstallConfirmationTouch(int x, int y);
void AskRestartAfterUninstall(int cleanup_complete);
void HandleHardwareButtons(int key);
void LoadCatalog(const char *url);
void Repaint();
void SaveServers();
void LoadServers();
void UnlockLockedServerPasswords(void);
void ForgetLockedServerPasswords(void);
int AnyServerPasswordLocked(void);
extern char credentials_warning[MAX_STR_LEN];

#include "i18n.h"

#endif
