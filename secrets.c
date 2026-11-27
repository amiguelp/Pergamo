/*
 * Almacenamiento cifrado de las contraseñas de los servidores.
 *
 * No depende de ninguna librería del lector: usa Monocypher (monocypher.c,
 * incluido en el repositorio). Una versión anterior usaba el libcrypto del
 * sistema y la app no arrancaba en firmwares sin libcrypto.so.1.0.0.
 *
 *   - Cifrado: XChaCha20-Poly1305 (crypto_aead_lock), nonce aleatorio de 24 B.
 *   - Clave del dispositivo: BLAKE2b(sal, "Pergamo-v2|serie|modelo").
 *   - Clave con PIN: Argon2id(PIN, sal, clave del dispositivo como secreto),
 *     4 MiB y 3 pasadas, para encarecer probar PIN uno a uno.
 *
 * Formatos guardados:
 *   pd1:<base64(nonce|mac|texto cifrado)>  -> clave del dispositivo
 *   pp1:<base64(nonce|mac|texto cifrado)>  -> clave con PIN
 *   (cualquier otro valor)                 -> texto plano antiguo; se migra
 *
 * Limitación conocida: sin PIN, quien tenga el propio lector puede obtener
 * su número de serie y descifrar las contraseñas. El cifrado del dispositivo
 * protege frente a copias del archivo en otro sitio; el PIN protege también
 * frente a quien tenga el lector.
 */
#include "secrets.h"
#include "monocypher.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SECRETS_NO_INKVIEW
#include <inkview.h>
#endif

extern void LogDebug(const char *msg);

#define SALT_LEN 16
#define KEY_LEN 32
#define NONCE_LEN 24
#define MAC_LEN 16
#define MAX_PLAIN 1024
#define ARGON2_BLOCKS 4096   /* 4 MiB */
#define ARGON2_PASSES 3
#define CHECK_PLAINTEXT "pergamo-pin-ok"
#define PREFIX_DEVICE "pd1:"
#define PREFIX_PIN "pp1:"
#define PREFIX_LEN 4

static uint8_t salt[SALT_LEN];
static int salt_ready = 0;
static int pin_enabled = 0;
static char pin_check[256] = {0};

static uint8_t device_key[KEY_LEN];
static int device_key_ready = 0;
static uint8_t pin_key[KEY_LEN];
static int pin_key_ready = 0;

/* --- utilidades ---------------------------------------------------------- */

static int RandomBytes(uint8_t *out, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, out + got, len - got);
        if (n <= 0) { close(fd); return -1; }
        got += (size_t)n;
    }
    close(fd);
    return 0;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int B64Encode(const uint8_t *in, size_t len, char *out, size_t out_size) {
    size_t need = 4 * ((len + 2) / 3) + 1;
    if (need > out_size) return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? B64[v & 63] : '=';
    }
    out[o] = '\0';
    return (int)o;
}

static int B64Value(char c) {
    const char *p = c ? strchr(B64, c) : NULL;
    return p ? (int)(p - B64) : -1;
}

/* Devuelve el número de bytes decodificados o -1 si no es base64 válido. */
static int B64Decode(const char *in, uint8_t *out, size_t out_size) {
    size_t len = strlen(in);
    if (len == 0 || len % 4 != 0) return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4) {
        int a = B64Value(in[i]), b = B64Value(in[i + 1]);
        int c = in[i + 2] == '=' ? -2 : B64Value(in[i + 2]);
        int d = in[i + 3] == '=' ? -2 : B64Value(in[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1) return -1;
        if ((c == -2 || d == -2) && i + 4 != len) return -1;
        if (c == -2 && d != -2) return -1;
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                     ((uint32_t)(c < 0 ? 0 : c) << 6) | (uint32_t)(d < 0 ? 0 : d);
        if (o >= out_size) return -1;
        out[o++] = (uint8_t)(v >> 16);
        if (c >= 0) { if (o >= out_size) return -1; out[o++] = (uint8_t)(v >> 8); }
        if (d >= 0) { if (o >= out_size) return -1; out[o++] = (uint8_t)v; }
    }
    return (int)o;
}

/* --- claves -------------------------------------------------------------- */

#ifdef SECRETS_NO_INKVIEW
const char *secrets_test_serial = "TEST-SERIAL";
static const char *DeviceSerial(void) { return secrets_test_serial; }
static const char *DeviceModel(void) { return "TEST-MODEL"; }
#else
static const char *DeviceSerial(void) { const char *s = GetSerialNumber(); return s ? s : ""; }
static const char *DeviceModel(void) { const char *s = GetDeviceModel(); return s ? s : ""; }
#endif

static int EnsureDeviceKey(void) {
    if (device_key_ready) return 0;
    if (!salt_ready) return -1;
    char secret[512];
    int n = snprintf(secret, sizeof(secret), "Pergamo-v2|%s|%s", DeviceSerial(), DeviceModel());
    if (n < 0 || (size_t)n >= sizeof(secret)) n = (int)strlen(secret);
    if (!DeviceSerial()[0]) {
        LogDebug("Secrets: el lector no devolvió número de serie; la clave del dispositivo es más débil.");
    }
    crypto_blake2b_keyed(device_key, KEY_LEN, salt, SALT_LEN, (const uint8_t *)secret, (size_t)n);
    crypto_wipe(secret, sizeof(secret));
    device_key_ready = 1;
    return 0;
}

static int DerivePinKey(const char *pin, uint8_t *key) {
    if (!pin || EnsureDeviceKey() != 0) return -1;
    void *work = malloc((size_t)ARGON2_BLOCKS * 1024);
    if (!work) {
        LogDebug("Secrets: no hay memoria para derivar la clave del PIN.");
        return -1;
    }
    crypto_argon2_config config = { CRYPTO_ARGON2_ID, ARGON2_BLOCKS, ARGON2_PASSES, 1 };
    crypto_argon2_inputs inputs = { (const uint8_t *)pin, salt, (uint32_t)strlen(pin), SALT_LEN };
    crypto_argon2_extras extras = { device_key, NULL, KEY_LEN, 0 };
    crypto_argon2(key, KEY_LEN, work, config, inputs, extras);
    free(work);   /* crypto_argon2 borra el área de trabajo al terminar */
    return 0;
}

/* --- cifrado autenticado -------------------------------------------------- */

static int Seal(const uint8_t *key, const char *prefix, const char *plain,
                char *out, size_t out_size) {
    size_t plain_len = strlen(plain);
    if (plain_len > MAX_PLAIN) return -1;
    uint8_t buf[NONCE_LEN + MAC_LEN + MAX_PLAIN];
    uint8_t *nonce = buf, *mac = buf + NONCE_LEN, *ct = buf + NONCE_LEN + MAC_LEN;
    if (RandomBytes(nonce, NONCE_LEN) != 0) return -1;
    crypto_aead_lock(ct, mac, key, nonce, NULL, 0, (const uint8_t *)plain, plain_len);

    size_t prefix_len = strlen(prefix);
    if (prefix_len >= out_size) return -1;
    memcpy(out, prefix, prefix_len);
    int r = B64Encode(buf, NONCE_LEN + MAC_LEN + plain_len, out + prefix_len, out_size - prefix_len);
    crypto_wipe(buf, sizeof(buf));
    if (r < 0) { out[0] = '\0'; return -1; }
    return 0;
}

static int Open(const uint8_t *key, const char *b64, char *out, size_t out_size) {
    uint8_t buf[NONCE_LEN + MAC_LEN + MAX_PLAIN];
    int n = B64Decode(b64, buf, sizeof(buf));
    if (n < NONCE_LEN + MAC_LEN) return -1;
    size_t ct_len = (size_t)n - NONCE_LEN - MAC_LEN;
    if (ct_len + 1 > out_size) return -1;
    int r = crypto_aead_unlock((uint8_t *)out, buf + NONCE_LEN, key, buf, NULL, 0,
                               buf + NONCE_LEN + MAC_LEN, ct_len);
    crypto_wipe(buf, sizeof(buf));
    if (r != 0) {
        crypto_wipe(out, out_size);
        out[0] = '\0';
        return -1;
    }
    out[ct_len] = '\0';
    return 0;
}

/* --- API ----------------------------------------------------------------- */

void SecretsLoadParams(const char *salt_b64, int pin_flag, const char *check) {
    salt_ready = 0;
    device_key_ready = 0;
    pin_key_ready = 0;
    crypto_wipe(device_key, sizeof(device_key));
    crypto_wipe(pin_key, sizeof(pin_key));
    uint8_t decoded[SALT_LEN + 3];
    if (salt_b64 && salt_b64[0] && B64Decode(salt_b64, decoded, sizeof(decoded)) == SALT_LEN) {
        memcpy(salt, decoded, SALT_LEN);
        salt_ready = 1;
    } else if (RandomBytes(salt, SALT_LEN) == 0) {
        salt_ready = 1;           /* primera vez: sal nueva */
    } else {
        LogDebug("Secrets: no se pudo generar la sal (/dev/urandom).");
    }
    pin_enabled = pin_flag && check && strncmp(check, PREFIX_PIN, PREFIX_LEN) == 0;
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
    return stored && (strncmp(stored, PREFIX_DEVICE, PREFIX_LEN) == 0 ||
                      strncmp(stored, PREFIX_PIN, PREFIX_LEN) == 0);
}

int SecretsNeedsPin(const char *stored) {
    return stored && strncmp(stored, PREFIX_PIN, PREFIX_LEN) == 0;
}

int SecretsDecrypt(const char *stored, char *out, size_t out_size) {
    if (!out || out_size == 0) return SECRET_ERROR;
    out[0] = '\0';
    if (!stored || !stored[0]) return SECRET_OK;
    if (strncmp(stored, PREFIX_DEVICE, PREFIX_LEN) == 0) {
        if (EnsureDeviceKey() != 0) return SECRET_ERROR;
        return Open(device_key, stored + PREFIX_LEN, out, out_size) == 0 ? SECRET_OK : SECRET_ERROR;
    }
    if (strncmp(stored, PREFIX_PIN, PREFIX_LEN) == 0) {
        if (!pin_key_ready) return SECRET_LOCKED;
        return Open(pin_key, stored + PREFIX_LEN, out, out_size) == 0 ? SECRET_OK : SECRET_ERROR;
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
        return Seal(pin_key, PREFIX_PIN, plain, out, out_size);
    }
    if (EnsureDeviceKey() != 0) return -1;
    return Seal(device_key, PREFIX_DEVICE, plain, out, out_size);
}

int SecretsUnlock(const char *pin) {
    if (!pin_enabled) return 0;
    uint8_t key[KEY_LEN];
    if (!pin || DerivePinKey(pin, key) != 0) return -1;
    char plain[64];
    int ok = strncmp(pin_check, PREFIX_PIN, PREFIX_LEN) == 0 &&
             Open(key, pin_check + PREFIX_LEN, plain, sizeof(plain)) == 0 &&
             strcmp(plain, CHECK_PLAINTEXT) == 0;
    if (ok) {
        memcpy(pin_key, key, KEY_LEN);
        pin_key_ready = 1;
    }
    crypto_wipe(key, sizeof(key));
    crypto_wipe(plain, sizeof(plain));
    return ok ? 0 : -1;
}

int SecretsEnablePin(const char *pin) {
    if (!pin || !pin[0]) return -1;
    uint8_t key[KEY_LEN];
    if (DerivePinKey(pin, key) != 0) return -1;
    char check[256];
    if (Seal(key, PREFIX_PIN, CHECK_PLAINTEXT, check, sizeof(check)) != 0) {
        crypto_wipe(key, sizeof(key));
        return -1;
    }
    memcpy(pin_key, key, KEY_LEN);
    crypto_wipe(key, sizeof(key));
    pin_key_ready = 1;
    pin_enabled = 1;
    snprintf(pin_check, sizeof(pin_check), "%s", check);
    return 0;
}

void SecretsDisablePin(void) {
    pin_enabled = 0;
    pin_check[0] = '\0';
    crypto_wipe(pin_key, sizeof(pin_key));
    pin_key_ready = 0;
}
