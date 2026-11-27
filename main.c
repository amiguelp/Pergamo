#include "opds_app.h"
#include "secrets.h"
#include "updater.h"
#include "launcher_icon.h"
#include <inkview.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h> // Included for dynamic library loading (dlsym)

// --- Dark Mode / Capabilities Definitions ---
// We define these here just in case the older SDK headers don't have them
#ifndef APP_CAPABILITY_SUPPORT_SCREEN_INVERSION
#define APP_CAPABILITY_SUPPORT_SCREEN_INVERSION (1 << 0)
#endif

typedef void (*IvSetAppCapability_t)(int);
static int app_instance_lock_fd = -1;
static int startup_display_ready = 0;
static int startup_setup_complete = 0;
static int startup_restart_dialog_active = 0;

#define STARTUP_SETUP_TIMER "PergamoSetup"

// Global State Variables
enum AppState current_state = STATE_MAIN_MENU; 
int server_count = 0;
int current_server_index = -1;
int editing_server_index = -1;
int selected_entry_index = -1;
OPDSServer servers[MAX_SERVERS];
OPDSServer temp_server;
OPDSEntry current_entries[MAX_ENTRIES];

// Temporary struct required to safely migrate users with legacy binary files 
// without crashing due to mismatched struct sizes
typedef struct {
    char name[MAX_STR_LEN];
    char url[MAX_STR_LEN];
    char user[MAX_STR_LEN];
    char pass[MAX_STR_LEN];
    int fetch_thumbs; 
} OldOPDSServer;

int entry_count = 0;
char current_host[MAX_STR_LEN];
int sys_width, sys_height;

// Debug logging mechanism (triggered only if LOGTRIGGER.TXT is present)
void LogDebug(const char *msg) {
    char trigger_file[MAX_STR_LEN];
    snprintf(trigger_file, sizeof(trigger_file), "%sLOGTRIGGER.TXT", APP_ROOT_DIR);

    // Exit early if the trigger file is absent to preserve device performance
    if (access(trigger_file, F_OK) != 0) {
        return; 
    }

    char log_file[MAX_STR_LEN];
    snprintf(log_file, sizeof(log_file), "%sopds_client.log", APP_ROOT_DIR);

    FILE *f = fopen(log_file, "a");
    if (f) {
        time_t now;
        time(&now);
        char *date = ctime(&now);
        date[strlen(date) - 1] = '\0'; // Strip the default newline from ctime
        fprintf(f, "[%s] %s\n", date, msg);
        fclose(f);
    }
}

// External UI Handlers
extern void DrawMainMenu();
extern void HandleMainMenuTouch(int x, int y);
extern void DrawServerOptions();
extern void HandleServerOptionsTouch(int x, int y);
extern void DrawServerForm();
extern void HandleServerFormTouch(int x, int y);
extern void DrawBrowsingView();
extern void HandleBrowsingTouch(int x, int y);
extern void DrawBookDetails();
extern void HandleBookDetailsTouch(int x, int y);
extern void HandleHardwareButtons(int key);
extern void InitNetwork();
extern void CleanupNetwork();

static int AcquireAppInstanceLock(void) {
    if (app_instance_lock_fd >= 0) return 1;

    int fd = open("/tmp/Pergamo.lock", O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        LogDebug("Could not open application instance lock; continuing without it.");
        return 1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int lock_error = errno;
        close(fd);
        if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) return 0;
        LogDebug("Could not acquire application instance lock; continuing without it.");
        return 1;
    }

    app_instance_lock_fd = fd;
    return 1;
}

static int SetRestartPromptPending(int pending) {
    if (!pending) {
        if (unlink(RESTART_PENDING_FILE) != 0 && errno != ENOENT) {
            LogDebug("Could not clear pending launcher restart prompt.");
            return -1;
        }
        return 0;
    }

    if (mkdir(APP_ROOT_DIR, 0777) != 0 && errno != EEXIST) {
        LogDebug("Could not create application directory for restart prompt state.");
        return -1;
    }

    FILE *file = fopen(RESTART_PENDING_FILE, "w");
    if (!file) {
        LogDebug("Could not save pending launcher restart prompt.");
        return -1;
    }
    if (fclose(file) != 0) {
        unlink(RESTART_PENDING_FILE);
        LogDebug("Could not finish saving pending launcher restart prompt.");
        return -1;
    }
    return 0;
}

static int IsRestartPromptPending(void) {
    struct stat st;
    if (stat(RESTART_PENDING_FILE, &st) == 0) return S_ISREG(st.st_mode);
    if (errno != ENOENT) LogDebug("Could not inspect pending launcher restart prompt.");
    return 0;
}

static void RequestDeviceRestart(void) {
    iv_ipc_request(MSG_REBOOT, 1, NULL, 0, 0);
}

static void RestartAfterFirstIconSetup(int button) {
    startup_restart_dialog_active = 0;
    if (button == 1) {
        SetRestartPromptPending(0);
        RequestDeviceRestart();
    } else if (button == 2 || button == -1) {
        SetRestartPromptPending(0);
    }
}

static void RestartAfterUninstall(int button) {
    if (button == 1) RequestDeviceRestart();
    CloseApp();
}

static void ConfigureLauncherIcon(void) {
    if (!startup_display_ready) {
        ClearTimerByName(STARTUP_SETUP_TIMER);
        SetHardTimer(STARTUP_SETUP_TIMER, ConfigureLauncherIcon, 100);
        return;
    }

    startup_setup_complete = 1;
    int icon_setup = InstallLauncherIcon();
    if (icon_setup < 0) {
        Message(ICON_ERROR, T("Icono del launcher"),
                T("No se pudo configurar el icono personalizado."), 4000);
    } else if (icon_setup > 0) {
        if (SetRestartPromptPending(1) != 0) {
            LogDebug("Launcher icon changed, but restart prompt state could not be saved.");
        }
    }

    if (icon_setup > 0 || IsRestartPromptPending()) {
        startup_restart_dialog_active = 1;
        Dialog(ICON_QUESTION, T("Actualizar el icono"),
               T("El launcher puede conservar el menú anterior en caché. Reinicia el lector para que cargue el nuevo icono. ¿Reiniciar ahora?"),
               T("Reiniciar"), T("Después"), RestartAfterFirstIconSetup);
        return;
    }

    /* Comprobar si hay una versión nueva (como mucho una vez al día). */
    SetHardTimer("PergamoUpdateCheck", AutoCheckForUpdates, 1500);
}

void AskRestartAfterUninstall(int cleanup_complete) {
    const char *reason = cleanup_complete
        ? T("El launcher debe recargar el menú para quitar el acceso de Pergamo. ¿Reiniciar ahora?")
        : T("Se quitó el acceso, pero quedaron archivos. Reiniciar actualiza el menú. ¿Reiniciar ahora?");
    Dialog(ICON_QUESTION, T("Reiniciar el lector"), reason, T("Reiniciar"), T("Después"),
           RestartAfterUninstall);
}

void Repaint() {
    ClearScreen();
    switch (current_state) {
        case STATE_MAIN_MENU:      DrawMainMenu(); break;
        case STATE_SERVER_OPTIONS: DrawServerOptions(); break;
        case STATE_SERVER_FORM:    DrawServerForm(); break;
        case STATE_BROWSING:       DrawBrowsingView(); break;
        case STATE_BOOK_DETAILS:   DrawBookDetails(); break;
        case STATE_UNINSTALL_CONFIRM: DrawUninstallConfirmation(); break;
        case STATE_SETTINGS:       DrawSettings(); break;
    }
    FullUpdate();
}

void SaveServers() {
    // Ensure the application directory exists before attempting to save!
    mkdir(APP_ROOT_DIR, 0777);

    // Touch the file to ensure it exists before OpenConfig tries to read it
    FILE *f = fopen(NEW_CFG_FILE, "a");
    if (f) fclose(f);

    // Use InkView's built-in config system
    iconfig *cfg = OpenConfig(NEW_CFG_FILE, NULL);
    if (!cfg) {
        LogDebug("Failed to open config for saving.");
        return;
    }

    WriteInt(cfg, "server_count", server_count);

    char crypto_salt[64], crypto_check[256];
    int crypto_pin = 0;
    SecretsGetParams(crypto_salt, sizeof(crypto_salt), &crypto_pin,
                     crypto_check, sizeof(crypto_check));
    WriteString(cfg, "crypto_salt", crypto_salt);
    WriteInt(cfg, "crypto_pin", crypto_pin);
    WriteString(cfg, "crypto_check", crypto_check);
    
    for (int i = 0; i < server_count; i++) {
        char key[64];
        
        snprintf(key, sizeof(key), "server_%d_name", i); 
        WriteString(cfg, key, servers[i].name);
        
        snprintf(key, sizeof(key), "server_%d_url", i);  
        WriteString(cfg, key, servers[i].url);
        
        snprintf(key, sizeof(key), "server_%d_user", i); 
        WriteString(cfg, key, servers[i].user);
        
        /* Nunca en texto plano: si sigue bloqueada por PIN se guarda tal
           cual estaba; si no, se cifra de nuevo. */
        if (!servers[i].pass_locked) {
            if (SecretsEncrypt(servers[i].pass, servers[i].pass_stored,
                               sizeof(servers[i].pass_stored)) != 0) {
                LogDebug("Secrets: no se pudo cifrar una contraseña; no se guarda.");
                servers[i].pass_stored[0] = '\0';
            }
        }
        snprintf(key, sizeof(key), "server_%d_pass", i); 
        WriteString(cfg, key, servers[i].pass_stored);
        
        snprintf(key, sizeof(key), "server_%d_fetch_thumbs", i); 
        WriteInt(cfg, key, servers[i].fetch_thumbs);
        
        snprintf(key, sizeof(key), "server_%d_catalog_rows", i); 
        WriteInt(cfg, key, servers[i].catalog_rows);
    }
    
    SaveConfig(cfg);
    CloseConfig(cfg);
}

char credentials_warning[MAX_STR_LEN] = {0};

/* Descifra la contraseña guardada de un servidor. Devuelve 1 si estaba en
   texto plano (hay que volver a guardar). */
static int ApplyStoredPassword(OPDSServer *server) {
    server->pass_locked = 0;
    int result = SecretsDecrypt(server->pass_stored, server->pass, sizeof(server->pass));
    if (result == SECRET_LOCKED) {
        server->pass_locked = 1;
        server->pass[0] = '\0';
    } else if (result == SECRET_ERROR) {
        LogDebug("Secrets: no se pudo descifrar una contraseña (¿otro lector o archivo dañado?).");
        server->pass[0] = '\0';
        server->pass_stored[0] = '\0';
        size_t used = strlen(credentials_warning);
        snprintf(credentials_warning + used, sizeof(credentials_warning) - used,
                 "%s%s", used ? ", " : "", server->name);
    }
    return result == SECRET_PLAINTEXT && server->pass[0];
}

int AnyServerPasswordLocked(void) {
    for (int i = 0; i < server_count; i++) if (servers[i].pass_locked) return 1;
    return 0;
}

void UnlockLockedServerPasswords(void) {
    for (int i = 0; i < server_count; i++) {
        if (servers[i].pass_locked) ApplyStoredPassword(&servers[i]);
    }
}

void ForgetLockedServerPasswords(void) {
    for (int i = 0; i < server_count; i++) {
        if (servers[i].pass_locked) {
            servers[i].pass_locked = 0;
            servers[i].pass[0] = '\0';
            servers[i].pass_stored[0] = '\0';
        }
    }
}

void LoadServers() {
    // Ensure the app directory is available immediately on boot
    mkdir(APP_ROOT_DIR, 0777);
    int needs_resave = 0;
    credentials_warning[0] = '\0';
    SecretsLoadParams("", 0, "");

    // --- NORMAL TEXT LOAD FROM NEW LOCATION ---
    // Explicitly check if the file exists on the filesystem first.
    if (access(NEW_CFG_FILE, F_OK) == 0) {
        iconfig *cfg = OpenConfig(NEW_CFG_FILE, NULL);
        if (cfg) {
            server_count = ReadInt(cfg, "server_count", 0);
            if (server_count > MAX_SERVERS) server_count = MAX_SERVERS;
            SecretsLoadParams(ReadString(cfg, "crypto_salt", ""),
                              ReadInt(cfg, "crypto_pin", 0),
                              ReadString(cfg, "crypto_check", ""));
            if (!ReadString(cfg, "crypto_salt", "")[0]) needs_resave = 1;

            for (int i = 0; i < server_count; i++) {
                char key[64];
                
                snprintf(key, sizeof(key), "server_%d_name", i);
                strncpy(servers[i].name, ReadString(cfg, key, ""), MAX_STR_LEN - 1);
                
                snprintf(key, sizeof(key), "server_%d_url", i);
                strncpy(servers[i].url, ReadString(cfg, key, ""), MAX_STR_LEN - 1);
                
                snprintf(key, sizeof(key), "server_%d_user", i);
                strncpy(servers[i].user, ReadString(cfg, key, ""), MAX_STR_LEN - 1);
                
                snprintf(key, sizeof(key), "server_%d_pass", i);
                snprintf(servers[i].pass_stored, sizeof(servers[i].pass_stored), "%s",
                         ReadString(cfg, key, ""));
                if (ApplyStoredPassword(&servers[i])) needs_resave = 1;
                
                snprintf(key, sizeof(key), "server_%d_fetch_thumbs", i);
                servers[i].fetch_thumbs = ReadInt(cfg, key, 1); 
                
                snprintf(key, sizeof(key), "server_%d_catalog_rows", i);
                servers[i].catalog_rows = ReadInt(cfg, key, 10); 
                if (servers[i].catalog_rows < 4) servers[i].catalog_rows = 4;
                if (servers[i].catalog_rows > 10) servers[i].catalog_rows = 10;
            }

            CloseConfig(cfg);
        }
        // Migrate plain-text passwords from older versions to encrypted storage
        if (needs_resave) SaveServers();
        return; // Successfully loaded from new location
    }

    // --- MIGRATION CHECK FROM LEGACY LOCATION ---
    if (access(LEGACY_CFG_FILE, F_OK) == 0) {
        FILE *fp = fopen(LEGACY_CFG_FILE, "rb");
        if (fp) {
            int magic_check = -1;
            fread(&magic_check, sizeof(int), 1, fp);
            fclose(fp);

            if (magic_check >= 0 && magic_check <= MAX_SERVERS) {
                LogDebug("Detected old binary config format in legacy location. Migrating...");
                fp = fopen(LEGACY_CFG_FILE, "rb");
                if (fread(&server_count, sizeof(int), 1, fp) == 1) {
                    if (server_count > MAX_SERVERS) server_count = MAX_SERVERS;
                    OldOPDSServer old_servers[MAX_SERVERS];
                    fread(old_servers, sizeof(OldOPDSServer), server_count, fp);
                    for(int i = 0; i < server_count; i++) {
                        strncpy(servers[i].name, old_servers[i].name, MAX_STR_LEN);
                        strncpy(servers[i].url, old_servers[i].url, MAX_STR_LEN);
                        strncpy(servers[i].user, old_servers[i].user, MAX_STR_LEN);
                        strncpy(servers[i].pass, old_servers[i].pass, MAX_STR_LEN);
                        servers[i].fetch_thumbs = old_servers[i].fetch_thumbs;
                        servers[i].catalog_rows = 10; // Default new property
                    }
                }
                fclose(fp);
            } else {
                LogDebug("Detected intermediate text config in legacy location. Migrating...");
                iconfig *old_cfg = OpenConfig(LEGACY_CFG_FILE, NULL);
                if (old_cfg) {
                    server_count = ReadInt(old_cfg, "server_count", 0);
                    if (server_count > MAX_SERVERS) server_count = MAX_SERVERS;

                    for (int i = 0; i < server_count; i++) {
                        char key[64];
                        snprintf(key, sizeof(key), "server_%d_name", i);
                        strncpy(servers[i].name, ReadString(old_cfg, key, ""), MAX_STR_LEN - 1);
                        snprintf(key, sizeof(key), "server_%d_url", i);
                        strncpy(servers[i].url, ReadString(old_cfg, key, ""), MAX_STR_LEN - 1);
                        snprintf(key, sizeof(key), "server_%d_user", i);
                        strncpy(servers[i].user, ReadString(old_cfg, key, ""), MAX_STR_LEN - 1);
                        snprintf(key, sizeof(key), "server_%d_pass", i);
                        strncpy(servers[i].pass, ReadString(old_cfg, key, ""), MAX_STR_LEN - 1);
                        snprintf(key, sizeof(key), "server_%d_fetch_thumbs", i);
                        servers[i].fetch_thumbs = ReadInt(old_cfg, key, 1); 
                        snprintf(key, sizeof(key), "server_%d_catalog_rows", i);
                        servers[i].catalog_rows = ReadInt(old_cfg, key, 10); 
                        if (servers[i].catalog_rows < 4) servers[i].catalog_rows = 4;
                        if (servers[i].catalog_rows > 10) servers[i].catalog_rows = 10;
                    }
                    CloseConfig(old_cfg);
                }
            }
            
            // Backup the old file so we don't read it again
            char bak_file[MAX_STR_LEN];
            snprintf(bak_file, sizeof(bak_file), "%s.bak", LEGACY_CFG_FILE);
            rename(LEGACY_CFG_FILE, bak_file); 
            
            // Save immediately in the new robust text format at the NEW location
            SaveServers(); 
        }
    } else {
        server_count = 0;
    }
}

static int main_handler(int event, int a, int b) {
    switch (event) {

        case EVT_INIT:
            sys_width = ScreenWidth();
            sys_height = ScreenHeight();
            if (!AcquireAppInstanceLock()) {
                CloseApp();
                return 0;
            }
            LogDebug("--- App Started ---");
            LoadLanguageSetting();
            UpdateCleanupPreviousVersion();

            LoadServers();

            // --- Dynamic Loading for Dark Mode (FW 6.8+) ---
            // Try to open the inkview library to see if it supports native screen inversion
            void *lib_handle = dlopen("libinkview.so", RTLD_LAZY);
            if (!lib_handle) lib_handle = dlopen(NULL, RTLD_LAZY); // Fallback to global symbol space

            if (lib_handle) {
                IvSetAppCapability_t set_cap = (IvSetAppCapability_t)dlsym(lib_handle, "IvSetAppCapability");
                if (set_cap) {
                    LogDebug("Firmware 6.8+ detected. Enabling native dark mode.");
                    set_cap(APP_CAPABILITY_SUPPORT_SCREEN_INVERSION);
                } else {
                    LogDebug("Firmware < 6.8. Native dark mode unsupported.");
                }
                // Not calling dlclose(lib_handle) intentionally, as inkview is a core system
                // dependency permanently linked to our app's lifetime anyway.
            }
            ClearTimerByName(STARTUP_SETUP_TIMER);
            SetHardTimer(STARTUP_SETUP_TIMER, ConfigureLauncherIcon, 500);
            break;

        case EVT_SHOW:
        case EVT_FOREGROUND:
            // Handle app un-minimization or device waking from sleep
            SetPanelType(0); 
            startup_display_ready = 1;
            if (!startup_restart_dialog_active) Repaint();
            if (!startup_setup_complete) {
                ClearTimerByName(STARTUP_SETUP_TIMER);
                SetHardTimer(STARTUP_SETUP_TIMER, ConfigureLauncherIcon, 500);
            }
            break;
            
        case EVT_BACKGROUND:
            // Handle device sleep or app minimization
            break;

        case EVT_CONFIGCHANGED:
        case EVT_OBREEY_CONFIG_CHANGED:
            // Triggers a redraw if the user toggles Dark Mode from the system drop-down menu
            if (!startup_restart_dialog_active) Repaint();
            break;

        case EVT_KEYPRESS:
            if (a == IV_KEY_BACK) { 
                if (current_state == STATE_MAIN_MENU) CloseApp();
                else {
                    current_state = current_state == STATE_UNINSTALL_CONFIRM
                        ? STATE_SETTINGS : STATE_MAIN_MENU;
                    Repaint();
                }
            } else {
                HandleHardwareButtons(a);
            }
            break;

        case EVT_POINTERUP:
            switch (current_state) {
                case STATE_MAIN_MENU:      HandleMainMenuTouch(a, b); break;
                case STATE_SERVER_OPTIONS: HandleServerOptionsTouch(a, b); break;
                case STATE_SERVER_FORM:    HandleServerFormTouch(a, b); break;
                case STATE_BROWSING:       HandleBrowsingTouch(a, b); break;
                case STATE_BOOK_DETAILS:   HandleBookDetailsTouch(a, b); break;
                case STATE_UNINSTALL_CONFIRM: HandleUninstallConfirmationTouch(a, b); break;
                case STATE_SETTINGS:       HandleSettingsTouch(a, b); break;
            }
            break;

        case EVT_EXIT:
            ClearTimerByName(STARTUP_SETUP_TIMER);
            break;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    InkViewMain(main_handler);
    return 0;
}
