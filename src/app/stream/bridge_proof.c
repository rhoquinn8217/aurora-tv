/* The app's half of the pairing proof a bridge command carries.
 *
 * ⭐ WHY (code review, 2026-10-05). The listener's control port is forwarded
 * to the internet for remote play, and a BRIDGE_START from anyone made
 * Windows see a USB keyboard the caller could type on. The listener now lets
 * in a TV calling from the internet only with a proof that it is paired with
 * the PC's streaming host (rhoquinn8217's choice, 2026-10-07): a signature by
 * the key it paired with, over a challenge the listener gave it and the
 * command. The core asks for the signature (its shared/agent_proof.inl) and
 * never sees a key; this file holds it.
 *
 * ⭐ THE KEY IS THE ONE THIS TV PAIRED WITH. libgamestream keeps it in
 * key_dir as key.pem, with its certificate as client.pem, and the host keeps
 * that certificate for every device it paired: the listener reads it there.
 * ⓘ Read from the files rather than out of libgamestream's client, whose
 * structure is its own.
 *
 * ⓘ What a signature is: RSA PKCS#1 v1.5 over SHA-256, in base64 -- what
 * mbedtls_pk_sign makes with an RSA key, and what the listener checks. The
 * fingerprint is the first 16 bytes of the SHA-256 of the certificate's DER,
 * in hex: the name the listener looks the certificate up by. */

#include "bridge_proof.h"

#include "ctm_bridge_glue.h"
#include "logging.h"

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ⓘ One lock for all of it: two pads bridging at once sign at once, and the
 * random generator a signature draws from is not safe to share without one. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_tried;
static bool g_ready;
static mbedtls_pk_context g_key;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static char g_fingerprint[33];

static int sha256_of(const unsigned char *input, size_t len, unsigned char out[32])
{
#if MBEDTLS_VERSION_NUMBER >= 0x03020100
    return mbedtls_sha256(input, len, out, 0);
#else
    return mbedtls_sha256_ret(input, len, out, 0);
#endif
}

static bool load_locked(const char *key_dir)
{
    char key_path[512];
    char cert_path[512];
    snprintf(key_path, sizeof(key_path), "%s/key.pem", key_dir);
    snprintf(cert_path, sizeof(cert_path), "%s/client.pem", key_dir);

    mbedtls_pk_init(&g_key);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, NULL, 0) != 0) {
        commons_log_warn("Bridge", "pairing proof: no random source; bridge commands go without a proof");
        return false;
    }
#if MBEDTLS_VERSION_NUMBER >= 0x03020100
    const int key_rc = mbedtls_pk_parse_keyfile(&g_key, key_path, NULL, mbedtls_ctr_drbg_random, &g_drbg);
#else
    const int key_rc = mbedtls_pk_parse_keyfile(&g_key, key_path, NULL);
#endif
    if (key_rc != 0 || mbedtls_pk_get_type(&g_key) != MBEDTLS_PK_RSA) {
        commons_log_warn("Bridge", "pairing proof: no paired RSA key at %s (%d); bridge commands go without a proof",
                         key_path, key_rc);
        return false;
    }

    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);
    unsigned char digest[32];
    const bool cert_ok = mbedtls_x509_crt_parse_file(&cert, cert_path) == 0 &&
                         sha256_of(cert.raw.p, cert.raw.len, digest) == 0;
    mbedtls_x509_crt_free(&cert);
    if (!cert_ok) {
        commons_log_warn("Bridge", "pairing proof: no certificate at %s; bridge commands go without a proof",
                         cert_path);
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        snprintf(g_fingerprint + 2 * i, 3, "%02x", digest[i]);
    }
    commons_log_info("Bridge", "pairing proof: signing as %s", g_fingerprint);
    return true;
}

static bool sign(const unsigned char *message, size_t len, char *fingerprint,
                 char *signature, size_t signature_len)
{
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    if (g_ready) {
        unsigned char hash[32];
        unsigned char sig[1024];
        size_t sig_len = 0;
        size_t written = 0;
        ok = sha256_of(message, len, hash) == 0;
        if (ok) {
#if MBEDTLS_VERSION_NUMBER >= 0x03020100
            ok = mbedtls_pk_sign(&g_key, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sizeof(sig),
                                 &sig_len, mbedtls_ctr_drbg_random, &g_drbg) == 0;
#else
            ok = mbedtls_pk_sign(&g_key, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, &sig_len,
                                 mbedtls_ctr_drbg_random, &g_drbg) == 0;
#endif
        }
        ok = ok && mbedtls_base64_encode((unsigned char *) signature, signature_len, &written, sig, sig_len) == 0;
        if (ok) {
            memcpy(fingerprint, g_fingerprint, sizeof(g_fingerprint));
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void bridge_proof_register(const char *key_dir)
{
    pthread_mutex_lock(&g_lock);
    if (!g_tried && key_dir != NULL && key_dir[0] != '\0') {
        g_tried = true;
        g_ready = load_locked(key_dir);
    }
    const bool ready = g_ready;
    pthread_mutex_unlock(&g_lock);
    ctm_bridge_use_pairing_signer(ready ? sign : NULL);
}
