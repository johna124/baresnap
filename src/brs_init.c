/* brs_init.c — inicialización de repositorio (VFS-aware, stderr-only output) */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "brs_init.h"
#include "brs_config.h"
#include "brs_fsutil.h"
#include "brs_crypto.h"
#include "brs_util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int brs_repo_init(const char *repo_path, int encrypt, int cipher_algo, int compression, int zstd_level)
{
    if (!repo_path || repo_path[0] == '\0') {
        fprintf(stderr, "error: repo path is empty\n");
        return -1;
    }

    /* Crear estructura de directorios */
    char dir[BRS_PATH_MAX];
    if (brs_mkdir_p(repo_path) != 0) {
        fprintf(stderr, "error: cannot create repo directory: %s\n", repo_path);
        return -1;
    }
    if (brs_path_join(dir, sizeof dir, repo_path, "snapshots") != 0) return -1;
    if (brs_mkdir_p(dir) != 0) {
        fprintf(stderr, "error: cannot create snapshots directory\n");
        return -1;
    }
    if (brs_path_join(dir, sizeof dir, repo_path, "packs") != 0) return -1;
    if (brs_mkdir_p(dir) != 0) {
        fprintf(stderr, "error: cannot create packs directory\n");
        return -1;
    }
    if (brs_path_join(dir, sizeof dir, repo_path, "index") != 0) return -1;
    if (brs_mkdir_p(dir) != 0) {
        fprintf(stderr, "error: cannot create index directory\n");
        return -1;
    }
    if (brs_path_join(dir, sizeof dir, repo_path, "tmp") != 0) return -1;
    if (brs_mkdir_p(dir) != 0) {
        fprintf(stderr, "error: cannot create tmp directory\n");
        return -1;
    }

    /* Verificar que no exista ya */
    char config_path[BRS_PATH_MAX];
    if (brs_path_join(config_path, sizeof config_path, repo_path, "config") != 0)
        return -1;
    if (brs_path_exists(config_path)) {
        fprintf(stderr, "error: repository already exists at '%s'\n", repo_path);
        return -1;
    }

    /* Config por defecto */
    BrsRepoConfig cfg;
    brs_repo_config_default(&cfg);

    brs_make_uuid(cfg.uuid);
    cfg.created_ns = brs_now_ns();

    /* Aplicar parámetros del usuario */
    cfg.compression = compression ? BRS_COMPRESSION_ZSTD : BRS_COMPRESSION_LZ4;
    cfg.zstd_level = (uint8_t)zstd_level;

    /* Encriptación: brs_init_crypto hace todo (salt + KVV + KDF params) */
    if (encrypt) {
        char pass[BRS_PASSPHRASE_MAX + 1];
        const char *env_pass = getenv("BARESNAP_PASSPHRASE");
        
        if (env_pass && env_pass[0] != '\0') {
            /* Usar passphrase del entorno (automatización) */
            size_t len = strlen(env_pass);
            if (len >= sizeof(pass)) {
                fprintf(stderr, "error: BARESNAP_PASSPHRASE too long (max %d)\n", BRS_PASSPHRASE_MAX);
                return -1;
            }
            memcpy(pass, env_pass, len + 1);
        } else {
            /* Pedir interactivamente con confirmación */
            if (brs_read_passphrase(pass, sizeof pass, "New passphrase: ") != 0) {
                fprintf(stderr, "error: failed to read passphrase\n");
                return -1;
            }
            char pass2[BRS_PASSPHRASE_MAX + 1];
            if (brs_read_passphrase(pass2, sizeof pass2, "Confirm passphrase: ") != 0) {
                brs_secure_wipe(pass, sizeof pass);
                fprintf(stderr, "error: failed to read passphrase confirmation\n");
                return -1;
            }
            if (strcmp(pass, pass2) != 0) {
                brs_secure_wipe(pass, sizeof pass);
                brs_secure_wipe(pass2, sizeof pass2);
                fprintf(stderr, "error: passphrases do not match\n");
                return -1;
            }
            brs_secure_wipe(pass2, sizeof pass2);
        }

        /* brs_init_crypto: genera salt aleatorio, KVV, y setea KDF params */
    
        cfg.cipher_algo = (BrsCipherAlgo)cipher_algo;
        if (brs_init_crypto(&cfg, pass) != 0) {
            brs_secure_wipe(pass, sizeof pass);
            fprintf(stderr, "error: crypto initialization failed\n");
            return -1;
        }
        brs_secure_wipe(pass, sizeof pass);
        cfg.encrypted = 1;
    cfg.cipher_algo = (BrsCipherAlgo)cipher_algo;
        cfg.flags |= BRS_FLAG_ENCRYPTED;
    }

    /* Escribir config */
    if (brs_write_config(repo_path, &cfg) != 0) {
        fprintf(stderr, "error: cannot write config\n");
        return -1;
    }
    fprintf(stderr, "Repository initialized at '%s'\n", repo_path);
    if (encrypt)
        fprintf(stderr, "  Encryption: %s + Argon2id\n",
            (cfg.cipher_algo == BRS_CIPHER_AES256_GCM) ? "AES-256-GCM" : "XChaCha20-Poly1305"); /* FIX P10: print cipher_algo */
    fprintf(stderr, "  Compression: %s", compression ? "ZSTD" : "LZ4");
    if (compression)
        fprintf(stderr, " (level %d)", zstd_level);
    fprintf(stderr, "\n");
    return 0;
}
