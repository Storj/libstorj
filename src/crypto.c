#include "crypto.h"

int calculate_file_id_by_name(const char *bucket, const char *file_name, char **buffer)
{
    // Combine bucket and file_name
    int name_len = strlen(bucket) + strlen(file_name);
    char name[name_len];
    strcpy(name, bucket);
    strcat(name, file_name);
    name[name_len] = '\0';

    // Get the sha256 of the file_name + bucket+id
    uint8_t sha256_digest[SHA256_DIGEST_SIZE];
    sha256_of_str((uint8_t *)name, name_len, sha256_digest);

    // Get the ripemd160 of the sha256
    uint8_t ripemd160_digest[RIPEMD160_DIGEST_SIZE];
    ripemd160_of_str(sha256_digest, SHA256_DIGEST_SIZE, ripemd160_digest);

    // Convert ripemd160 hex to character array
    char ripemd160_str[RIPEMD160_DIGEST_SIZE*2+1];
    memset(ripemd160_str, '\0', RIPEMD160_DIGEST_SIZE*2+1);
    hex2str(RIPEMD160_DIGEST_SIZE, ripemd160_digest, ripemd160_str);

    //Copy the result into buffer
    memcpy(*buffer, ripemd160_str, FILE_ID_SIZE);

    return 0;
}

int ripmd160sha256_as_string(uint8_t *data, uint64_t data_size, char **digest)
{
    uint8_t *ripemd160_digest = calloc(RIPEMD160_DIGEST_SIZE, sizeof(char));
    if (!ripemd160_digest) {
        return 1;
    }
    ripmd160sha256(data, data_size, &ripemd160_digest);

    // Convert ripemd160 hex to character array
    char ripemd160_str[RIPEMD160_DIGEST_SIZE*2+1];
    memset(ripemd160_str, '\0', RIPEMD160_DIGEST_SIZE*2+1);
    hex2str(RIPEMD160_DIGEST_SIZE, ripemd160_digest, ripemd160_str);

    //Copy the result into buffer
    memcpy(*digest, ripemd160_str, RIPEMD160_DIGEST_SIZE * 2);

    free(ripemd160_digest);

    return 0;
}

int ripmd160sha256(uint8_t *data, uint64_t data_size, uint8_t **digest)
{
    // Get the sha256 of the data
    uint8_t sha256_digest[SHA256_DIGEST_SIZE];
    sha256_of_str(data, data_size, sha256_digest);

    // Get the ripemd160 of the sha256
    uint8_t ripemd160_digest[RIPEMD160_DIGEST_SIZE];
    ripemd160_of_str(sha256_digest, SHA256_DIGEST_SIZE, ripemd160_digest);

    //Copy the result into buffer
    memcpy(*digest, ripemd160_digest, RIPEMD160_DIGEST_SIZE);

    return 0;
}

int double_ripmd160sha256(uint8_t *data, uint64_t data_size, uint8_t **digest)
{
    uint8_t *first_ripemd160_digest = calloc(RIPEMD160_DIGEST_SIZE, sizeof(char));
    if (!first_ripemd160_digest) {
        return 1;
    }
    ripmd160sha256(data, data_size, &first_ripemd160_digest);

    uint8_t *second_ripemd160_digest = calloc(RIPEMD160_DIGEST_SIZE, sizeof(char));
    if (!second_ripemd160_digest) {
        return 1;
    }
    ripmd160sha256(first_ripemd160_digest, RIPEMD160_DIGEST_SIZE,
                   &second_ripemd160_digest);


    //Copy the result into buffer
    memcpy(*digest, second_ripemd160_digest, RIPEMD160_DIGEST_SIZE);

    free(first_ripemd160_digest);
    free(second_ripemd160_digest);

    return 0;
}

int double_ripmd160sha256_as_string(uint8_t *data, uint64_t data_size,
                                    char **digest)
{
    uint8_t *ripemd160_digest = calloc(RIPEMD160_DIGEST_SIZE, sizeof(char));
    if (!ripemd160_digest) {
        return 1;
    }
    if (double_ripmd160sha256(data, data_size, &ripemd160_digest)) {
        return 1;
    }

    // Convert ripemd160 hex to character array
    char ripemd160_str[RIPEMD160_DIGEST_SIZE*2+1];
    memset(ripemd160_str, '\0', RIPEMD160_DIGEST_SIZE*2+1);
    hex2str(RIPEMD160_DIGEST_SIZE, ripemd160_digest, ripemd160_str);

    //Copy the result into buffer
    memcpy(*digest, ripemd160_str, RIPEMD160_DIGEST_SIZE * 2);

    free(ripemd160_digest);

    return 0;
}

int generate_bucket_key(const char *mnemonic, const char *bucket_id,
                        char **bucket_key)
{
    char *seed = calloc(128 + 1, sizeof(char));
    if (!seed) {
        return 1;
    }
    mnemonic_to_seed(mnemonic, "", &seed);
    seed[128] = '\0';
    if (get_deterministic_key(seed, 128, bucket_id, bucket_key)) {
        return 1;
    }

    memset_zero(seed, 128 + 1);
    free(seed);

    return 0;
}

int generate_file_key(const char *mnemonic, const char *bucket_id,
                      const char *file_id, char **file_key)
{
    char *bucket_key = calloc(DETERMINISTIC_KEY_SIZE + 1, sizeof(char));
    if (!bucket_key) {
        return 1;
    }
    if (generate_bucket_key(mnemonic, bucket_id, &bucket_key)) {
        return 1;
    }
    bucket_key[DETERMINISTIC_KEY_SIZE] = '\0';

    get_deterministic_key(bucket_key, 64, file_id, file_key);

    memset_zero(bucket_key, DETERMINISTIC_KEY_SIZE + 1);
    free(bucket_key);

    return 0;
}

int get_deterministic_key(const char *key, int key_len,
                          const char *id, char **buffer)
{
    int input_len = key_len + strlen(id);
    char *sha512input = calloc(input_len + 1, sizeof(char));
    if (!sha512input) {
        return 1;
    }

    // Combine key and id
    memcpy(sha512input, key, key_len);
    memcpy(sha512input + key_len, id, strlen(id));
    sha512input[input_len] = '\0';

    // Convert input to hexdata
    uint8_t sha512input_as_hex[input_len / 2 + 1];
    memset(sha512input_as_hex, '\0', input_len / 2 + 1);
    if (str2hex(input_len, sha512input, sha512input_as_hex)) {
        return 1;
    }

    // Sha512 of hexdata
    uint8_t sha512_digest[SHA512_DIGEST_SIZE];
    sha512_of_str(sha512input_as_hex, input_len / 2, sha512_digest);

    // Convert Sha512 hex to character array
    char sha512_str[SHA512_DIGEST_SIZE * 2 + 1];
    memset(sha512_str, '\0', RIPEMD160_DIGEST_SIZE * 2 + 1);
    hex2str(SHA512_DIGEST_SIZE, sha512_digest, sha512_str);

    //First 64 bytes of sha512
    memcpy(*buffer, sha512_str, DETERMINISTIC_KEY_SIZE);

    memset_zero(sha512_str, SHA512_DIGEST_SIZE * 2 + 1);
    memset_zero(sha512_digest, SHA512_DIGEST_SIZE);
    memset_zero(sha512input_as_hex, input_len / 2 + 1);
    memset_zero(sha512input, input_len + 1);

    free(sha512input);

    return 0;
}

int sha256_of_str(const uint8_t *str, int str_len, uint8_t *digest)
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, str_len, str);
    sha256_digest(&ctx, SHA256_DIGEST_SIZE, digest);

    return 0;
}

int ripemd160_of_str(const uint8_t *str, int str_len, uint8_t *digest)
{
    struct ripemd160_ctx ctx;
    ripemd160_init(&ctx);
    ripemd160_update(&ctx, str_len, str);
    ripemd160_digest(&ctx, RIPEMD160_DIGEST_SIZE, digest);

    return 0;
}

int sha512_of_str(const uint8_t *str, int str_len, uint8_t *digest)
{
    struct sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, str_len, str);
    sha512_digest(&ctx, SHA512_DIGEST_SIZE, digest);

    return 0;
}

void pbkdf2_hmac_sha512 (
    unsigned key_length,
    const uint8_t *key,
    unsigned iterations,
    unsigned salt_length, const uint8_t *salt,
    unsigned length, uint8_t *dst)
{
    struct hmac_sha512_ctx sha512ctx;

    hmac_sha512_set_key (&sha512ctx, key_length, key);
    PBKDF2 (&sha512ctx, hmac_sha512_update, hmac_sha512_digest,
    SHA512_DIGEST_SIZE, iterations, salt_length, salt, length, dst);
}

int increment_ctr_aes_iv(uint8_t *iv, uint64_t bytes_position)
{
    if (bytes_position % AES_BLOCK_SIZE != 0) {
        return 1;
    }

    uint64_t times = bytes_position / AES_BLOCK_SIZE;

    while (times) {
        unsigned int i = AES_BLOCK_SIZE - 1;
        if (++(iv)[i] == 0) {
            while (i > 0 && ++(iv)[--i] == 0);
        }
        times--;
    }

    return 0;
}

int calculate_file_id(FILE *fp, char *salt, int salt_len, char **digest)
{
    if (NULL == fp) {
        return 1;
    }

    int read_bytes = 0;

    // Calculate sha256 of file data
    uint8_t sha256_data[SHA256_DIGEST_SIZE];
    struct sha256_ctx sha_ctx;
    sha256_init(&sha_ctx);
    char read_data[16];

    memset_zero(read_data, 16);
    memset_zero(sha256_data, SHA256_DIGEST_SIZE);

    if (salt && salt_len > 0) {
        sha256_update(&sha_ctx, salt_len, salt);
    }

    while ((read_bytes = fread(read_data, 1, 16, fp)) > 0)
    {
        sha256_update(&sha_ctx, read_bytes, read_data);
    }

    sha256_digest(&sha_ctx, SHA256_DIGEST_SIZE, sha256_data);

    // Calculate ripemd160 of sha256
    struct ripemd160_ctx ripemd_ctx;
    uint8_t ripemd160_data[RIPEMD160_DIGEST_SIZE];

    memset_zero(ripemd160_data, RIPEMD160_DIGEST_SIZE);

    ripemd160_init(&ripemd_ctx);
    ripemd160_update(&ripemd_ctx, SHA256_DIGEST_SIZE, sha256_data);
    ripemd160_digest(&ripemd_ctx, RIPEMD160_DIGEST_SIZE, ripemd160_data);

    // Convert ripemd160 to string
    *digest = calloc((RIPEMD160_DIGEST_SIZE * 2) + 2, sizeof(char));
    if (!*digest) {
        return 1;
    }

    hex2str(RIPEMD160_DIGEST_SIZE, ripemd160_data, *digest);

    memset_zero(sha256_data, SHA256_DIGEST_SIZE);
    memset_zero(ripemd160_data, RIPEMD160_DIGEST_SIZE);

    return 0;
}

uint8_t *key_from_passphrase(const char *passphrase, const char *salt)
{
    uint8_t passphrase_len = strlen(passphrase);
    uint8_t salt_len = strlen(salt);
    uint8_t *key = calloc(SHA256_DIGEST_SIZE + 1, sizeof(uint8_t));
    if (!key) {
        return NULL;
    }
    int rounds = 200000;
    pbkdf2_hmac_sha256(passphrase_len, (uint8_t *)passphrase, rounds, salt_len,
                       (uint8_t *)salt, SHA256_DIGEST_SIZE, key);

    return key;
}

int decrypt_data(const char *passphrase, const char *salt, const char *data,
                 char **result)
{

    uint8_t *key = key_from_passphrase(passphrase, salt);
    if (!key) {
        return 1;
    }

    // Convert from hex string
    int len = strlen(data);
    if (len / 2 < SHA256_DIGEST_SIZE + 1) {
        return 1;
    }
    int enc_len = len / 2;
    int data_size = enc_len - SHA256_DIGEST_SIZE;
    uint8_t *enc = calloc(enc_len + 1, sizeof(uint8_t));
    if (str2hex(len, (char *)data, enc)) {
        return 1;
    }

    // Get hash from enc data
    uint8_t *data_hash_ctr = calloc(SHA256_DIGEST_SIZE, sizeof(uint8_t));
    uint8_t *data_hash = calloc(SHA256_DIGEST_SIZE, sizeof(uint8_t));
    memcpy(data_hash_ctr, enc, SHA256_DIGEST_SIZE);
    memcpy(data_hash, enc, SHA256_DIGEST_SIZE);

    // Decrypt data
    *result = calloc(data_size + 1, sizeof(uint8_t));
    struct aes256_ctx *ctx1 = malloc(sizeof(struct aes256_ctx));
    aes256_set_encrypt_key(ctx1, key);

    free(key);

    ctr_crypt(ctx1, (nettle_cipher_func *)aes256_encrypt,
              AES_BLOCK_SIZE, data_hash_ctr,
              data_size, (uint8_t *)*result, enc + SHA256_DIGEST_SIZE);

    free(ctx1);

    // Check that decryption matches expected hash
    struct sha256_ctx *ctx2 = malloc(sizeof(struct sha256_ctx));
    sha256_init(ctx2);
    sha256_update(ctx2, data_size, (uint8_t *)*result);
    sha256_update(ctx2, strlen(salt), (uint8_t *)salt);
    uint8_t *decrypted_hash = calloc(SHA256_DIGEST_SIZE, sizeof(uint8_t));
    sha256_digest(ctx2, SHA256_DIGEST_SIZE, decrypted_hash);

    free(ctx2);

    int sha_match = memcmp(decrypted_hash, data_hash,
                           SHA256_DIGEST_SIZE);
    free(decrypted_hash);
    free(data_hash_ctr);
    free(data_hash);
    free(enc);

    if (sha_match != 0) {
        return 1;
    }

    return 0;
}

int encrypt_data(const char *passphrase, const char *salt, const char *data,
                 char **result)
{
    uint8_t *key = key_from_passphrase(passphrase, salt);
    if (!key) {
        return 1;
    }

    // Hash the data to create the ctr
    uint8_t data_size = strlen(data);
    if (data_size <= 0) {
        return 1;
    }
    int buffer_size = data_size + SHA256_DIGEST_SIZE;
    uint8_t *buffer = calloc(buffer_size, sizeof(char));
    if (!buffer) {
        return 1;
    }

    struct sha256_ctx *ctx1 = malloc(sizeof(struct sha256_ctx));
    if (!ctx1) {
        return 1;
    }
    sha256_init(ctx1);
    sha256_update(ctx1, data_size, (uint8_t *)data);
    sha256_update(ctx1, strlen(salt), (uint8_t *)salt);
    uint8_t *data_hash = calloc(SHA256_DIGEST_SIZE, sizeof(uint8_t));
    if (!data_hash) {
        return 1;
    }
    sha256_digest(ctx1, SHA256_DIGEST_SIZE, data_hash);

    // Copy hash to buffer
    memcpy(buffer, data_hash, SHA256_DIGEST_SIZE);

    // Encrypt data
    struct aes256_ctx *ctx2 = malloc(sizeof(struct aes256_ctx));
    if (!ctx2) {
        return 1;
    }
    aes256_set_encrypt_key(ctx2, key);
    ctr_crypt(ctx2, (nettle_cipher_func *)aes256_encrypt,
              AES_BLOCK_SIZE, data_hash,
              data_size, buffer + SHA256_DIGEST_SIZE, (uint8_t *)data);


    // Convert to hex string
    *result = calloc(buffer_size * 2 + 2, sizeof(char));
    if (!*result) {
        return 1;
    }
    hex2str(buffer_size, buffer, *result);

    free(buffer);
    free(data_hash);
    free(ctx1);
    free(ctx2);
    free(key);

    return 0;
}
