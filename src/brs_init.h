/* brs_init.h — inicializacion de repositorios */
#ifndef BRS_INIT_H
#define BRS_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

/* 0 ok, 1 error (mensajes por stderr, como el CLI original). */
int brs_repo_init(const char *repo_path, int encrypt, int cipher_algo, int compression, int zstd_level, int hash_algo);


#ifdef __cplusplus
}
#endif

#endif /* BRS_INIT_H */
