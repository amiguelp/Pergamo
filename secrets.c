/*
 * Almacenamiento cifrado de las contraseñas de los servidores.
 *
 * Las contraseñas se guardan en opds_client.cfg cifradas con AES-256-GCM.
 * La clave se deriva con PBKDF2-HMAC-SHA256 de:
 *   - datos propios de este lector (número de serie y modelo), y
 *   - opcionalmente, un PIN que el usuario escribe al abrir un servidor.
 *
 * Formatos guardados:
 *   enc1:<base64(iv|tag|texto cifrado)>  -> clave del dispositivo
 *   enc2:<base64(iv|tag|texto cifrado)>  -> clave del dispositivo + PIN
 *   (cualquier otro valor)               -> texto plano antiguo; se migra
 *
 * Limitación conocida: sin PIN, quien tenga el propio lector puede obtener
 * su número de serie y descifrar las contraseñas. El cifrado del dispositivo
 * protege frente a copias del archivo en otro sitio; el PIN protege también
 * frente a quien tenga el lector.
 */
#include "secrets.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

#ifndef SECRETS_NO_INKVIEW
#include <inkview.h>
#endif

extern void LogDebug(const char *msg);

#define SALT_LEN 16
#define KEY_LEN 32
#define IV_LEN 12
#define TAG_LEN 16
#define DEVICE_ITERATIONS 10000
#define PIN_ITERATIONS 100000
#define CHECK_PLAINTEXT "pergamo-pin-ok"

static unsigned char salt[SALT_LEN];
static int salt_ready = 0;
static int pin_enabled = 0;
static char pin_check[256] = {0};      /* enc2 de CHECK_PLAINTEXT */

static unsigned char device_key[KEY_LEN];
static int device_key_ready = 0;
static unsigned char pin_key[KEY_LEN];
static int pin_key_ready = 0;

/* --- base64 ------------------------------------------------------------- */

static int B64Encode(const unsigned char *in, int len, char *out, size_t out_size) {
    if ((size_t)(4 * ((len + 2) / 3) + 1) > out_size) return -1;
    return EVP_EncodeBlock((unsigned char *)out, in, len);
}

static int B64Decode(const char *in, unsigned char *out, size_t out_size) {
    size_t len = strlen(in);
    if (len == 0 || len % 4 != 0 || (len / 4) * 3 > out_size) return -1;
    int n = EVP_DecodeBlock(out, (const unsigned char *)in, (int)len);
    if (n < 0) return -1;
    if (in[len - 1] == '=') n--;
    if (in[len - 2] == '=') n--;
    return n;
}

/* --- claves ------------------------------------------------------------- */

#ifdef SECRETS_NO_INKVIEW
const char *secrets_test_serial = "TEST-SERIAL";
static const char *DeviceSerial(void) { return secrets_test_serial; }
static const char *DeviceModel(void) { return "TEST-MODEL"; }
#else
static const char *DeviceSerial(void) { const char *s = GetSerialNumber(); return s ? s : ""; }
static const char *DeviceModel(void) { const char *s = GetDeviceModel(); return s ? s : ""; }
#endif

static void DeviceSecret(char *out, size_t out_size) {
    snprintf(out, out_size, "Pergamo-v1|%s|%s", DeviceSerial(), DeviceModel());
}

static int Derive(const char *secret, int iterations, unsigned char *key) {
    if (!salt_ready) return -1;
    return PKCS5_PBKDF2_HMAC(secret, (int)strlen(secret), salt, SALT_LEN,
                             iterations, EVP_sha256(), KEY_LEN, key) == 1 ? 0 : -1;
}

static int EnsureDeviceKey(void) {
    if (device_key_ready) return 0;
    char secret[512];
    DeviceSecret(secret, sizeof(secret));
    if (strlen(DeviceSerial()) == 0) {
        LogDebug("Secrets: el lector no devolvió número de serie; la clave del dispositivo es más débil.");
    }
    int r = Derive(secret, DEVICE_ITERATIONS, device_key);
    OPENSSL_cleanse(secret, sizeof(secret));
    if (r == 0) device_key_ready = 1;
    return r;
}

static int DerivePinKey(const char *pin, unsigned char *key) {
    char secret[768];
    DeviceSecret(secret, sizeof(secret));
    size_t used = strlen(secret);
    snprintf(secret + used, sizeof(secret) - used, "|pin|%s", pin);
    int r = Derive(secret, PIN_ITERATIONS, key);
    OPENSSL_cleanse(secret, sizeof(secret));
    return r;
}

/* --- AES-256-GCM -------------------------------------------------------- */

static int Seal(const unsigned char *key, const char *prefix, const char *plain,
                char *out, size_t out_size) {
    int plain_len = (int)strlen(plain);
    unsigned char buf[IV_LEN + TAG_LEN + 1024];
    if (plain_len > 1024) return -1;
    unsigned char *iv = buf, *tag = buf + IV_LEN, *ct = buf + IV_LEN + TAG_LEN;
    if (RAND_bytes(iv, IV_LEN) != 1) return -1;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int ok = ctx != NULL, len = 0, total = 0;
    ok = ok && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) == 1;
    ok = ok && EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) == 1;
    ok = ok && EVP_EncryptUpdate(ctx, ct, &len, (const unsigned char *)plain, plain_len) == 1;
    total = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, ct + total, &len) == 1;
    total += len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag) == 1;
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    if (!ok) return -1;

    size_t prefix_len = strlen(prefix);
    if (prefix_len >= out_size) return -1;
    memcpy(out, prefix, prefix_len);
    int r = B64Encode(buf, IV_LEN + TAG_LEN + total, out + prefix_len, out_size - prefix_len);
    OPENSSL_cleanse(buf, sizeof(buf));
    return r < 0 ? -1 : 0;
}

static int Open(const unsigned char *key, const char *b64, char *out, size_t out_size) {
    unsigned char buf[IV_LEN + TAG_LEN + 1100];
    int n = B64Decode(b64, buf, sizeof(buf));
    if (n < IV_LEN + TAG_LEN) return -1;
    int ct_len = n - IV_LEN - TAG_LEN;
    if ((size_t)ct_len + 1 > out_size) return -1;
    unsigned char *iv = buf, *tag = buf + IV_LEN, *ct = buf + IV_LEN + TAG_LEN;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int ok = ctx != NULL, len = 0, total = 0;
    ok = ok && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) == 1;
    ok = ok && EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) == 1;
    ok = ok && EVP_DecryptUpdate(ctx, (unsigned char *)out, &len, ct, ct_len) == 1;
    total = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, tag) == 1;
    ok = ok && EVP_DecryptFinal_ex(ctx, (unsigned char *)out + total, &len) == 1;
    total += ok ? len : 0;
    if (ctx) EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(buf, sizeof(buf));
    if (!ok) {
        OPENSSL_cleanse(out, out_size);
        out[0] = '\0';
        return -1;
    }
    out[total] = '\0';
    return 0;
}

/* --- API ---------------------------------------------------------------- */

void SecretsLoadParams(const char *salt_b64, int pin_flag, const char *check) {
    salt_ready = 0;
    device_key_ready = 0;
    pin_key_ready = 0;
    unsigned char decoded[SALT_LEN + 3];
    if (salt_b64 && salt_b64[0] && B64Decode(salt_b64, decoded, sizeof(decoded)) == SALT_LEN) {
        memcpy(salt, decoded, SALT_LEN);
        salt_ready = 1;
    } else if (RAND_bytes(salt, SALT_LEN) == 1) {
        salt_ready = 1;           /* primera vez: sal nueva */
    }
    pin_enabled = pin_flag && check && check[0];
    snprintf(pin_check, sizeof(pin_check), "%s", pin_enabled ? check : "");
}

void SecretsGetParams(char *salt_b64, size_t salt_size, int *pin_flag,
                      char *check, size_t check_size) {
    if (salt_b64 && salt_size) {
        salt_b64[0] = '\0';
        if (salt_ready) B64Encode(salt, SALT_LEN, salt_b64, salt_size);
    }
    if (pin_flag) *pin_flag = pin_enabled;
    if (check && check_size) snprintf(check, check_size, "%s", pin_enabled ? pin_check : "");
}

int SecretsPinEnabled(void) { return pin_enabled; }
int SecretsPinUnlocked(void) { return !pin_enabled || pin_key_ready; }

int SecretsIsEncrypted(const char *stored) {
    return stored && (strncmp(stored, "enc1:", 5) == 0 || strncmp(stored, "enc2:", 5) == 0);
}

int SecretsNeedsPin(const char *stored) {
    return stored && strncmp(stored, "enc2:", 5) == 0;
}

int SecretsDecrypt(const char *stored, char *out, size_t out_size) {
    if (!out || out_size == 0) return SECRET_ERROR;
    out[0] = '\0';
    if (!stored || !stored[0]) return SECRET_OK;
    if (strncmp(stored, "enc1:", 5) == 0) {
        if (EnsureDeviceKey() != 0) return SECRET_ERROR;
        return Open(device_key, stored + 5, out, out_size) == 0 ? SECRET_OK : SECRET_ERROR;
    }
    if (strncmp(stored, "enc2:", 5) == 0) {
        if (!pin_key_ready) return SECRET_LOCKED;
        return Open(pin_key, stored + 5, out, out_size) == 0 ? SECRET_OK : SECRET_ERROR;
    }
    snprintf(out, out_size, "%s", stored);   /* texto plano antiguo */
    return SECRET_PLAINTEXT;
}

int SecretsEncrypt(const char *plain, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    if (!plain || !plain[0]) return 0;
    if (pin_enabled) {
        if (!pin_key_ready) return -1;
        return Seal(pin_key, "enc2:", plain, out, out_size);
    }
    if (EnsureDeviceKey() != 0) return -1;
    return Seal(device_key, "enc1:", plain, out, out_size);
}

int SecretsUnlock(const char *pin) {
    if (!pin_enabled) return 0;
    unsigned char key[KEY_LEN];
    if (!pin || DerivePinKey(pin, key) != 0) return -1;
    char plain[64];
    int ok = strncmp(pin_check, "enc2:", 5) == 0 &&
             Open(key, pin_check + 5, plain, sizeof(plain)) == 0 &&
             strcmp(plain, CHECK_PLAINTEXT) == 0;
    if (ok) {
        memcpy(pin_key, key, KEY_LEN);
        pin_key_ready = 1;
    }
    OPENSSL_cleanse(key, sizeof(key));
    return ok ? 0 : -1;
}

int SecretsEnablePin(const char *pin) {
    if (!pin || !pin[0]) return -1;
    unsigned char key[KEY_LEN];
    if (DerivePinKey(pin, key) != 0) return -1;
    char check[256];
    if (Seal(key, "enc2:", CHECK_PLAINTEXT, check, sizeof(check)) != 0) {
        OPENSSL_cleanse(key, sizeof(key));
        return -1;
    }
    memcpy(pin_key, key, KEY_LEN);
    OPENSSL_cleanse(key, sizeof(key));
    pin_key_ready = 1;
    pin_enabled = 1;
    snprintf(pin_check, sizeof(pin_check), "%s", check);
    return 0;
}

void SecretsDisablePin(void) {
    pin_enabled = 0;
    pin_check[0] = '\0';
    OPENSSL_cleanse(pin_key, sizeof(pin_key));
    pin_key_ready = 0;
}
