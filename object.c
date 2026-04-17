#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTATION ─────────────────────────────────────────────────────────

// Write object
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    char header[64];
    const char *type_str;

    if (type == OBJ_BLOB)        type_str = "blob";
    else if (type == OBJ_TREE)   type_str = "tree";
    else                         type_str = "commit";

    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    header[header_len] = '\0';
    header_len += 1;  /* include the null byte */

    size_t total_len = header_len + len;
    char *full = malloc(total_len);
    if (!full) return -1;

    memcpy(full, header, header_len);
    memcpy(full + header_len, data, len);

    ObjectID id;
    compute_hash(full, total_len, &id);

    if (object_exists(&id)) {
        *id_out = id;
        free(full);
        return 0;
    }

    /* Build the object path */
    char path[512];
    object_path(&id, path, sizeof(path));

    /* Create the shard directory (.pes/objects/XX/) */
    char dir[512];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);   /* parent (.pes/objects/) must already exist via pes init */
    }

    /* Use a separate, larger buffer for the tmp path to avoid truncation */
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full);
        return -1;
    }

    if (write(fd, full, total_len) != (ssize_t)total_len) {
        close(fd);
        unlink(tmp_path);
        free(full);
        return -1;
    }

    fsync(fd);
    close(fd);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        free(full);
        return -1;
    }

    *id_out = id;
    free(full);
    return 0;
}

// Read object
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size <= 0) {
        fclose(f);
        return -1;
    }

    char *buf = malloc(file_size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t got = fread(buf, 1, file_size, f);
    fclose(f);

    if ((long)got != file_size) {
        free(buf);
        return -1;
    }

    /* Verify integrity */
    ObjectID check;
    compute_hash(buf, file_size, &check);
    if (memcmp(check.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    char *null_pos = memchr(buf, '\0', file_size);
    if (!null_pos) {
        free(buf);
        return -1;
    }

    if      (strncmp(buf, "blob",   4) == 0) *type_out = OBJ_BLOB;
    else if (strncmp(buf, "tree",   4) == 0) *type_out = OBJ_TREE;
    else if (strncmp(buf, "commit", 6) == 0) *type_out = OBJ_COMMIT;
    else {
        free(buf);
        return -1;
    }

    size_t header_len = (null_pos - buf) + 1;
    size_t data_len   = file_size - header_len;

    void *data = malloc(data_len + 1);  /* +1 safety byte */
    if (!data) {
        free(buf);
        return -1;
    }
    memcpy(data, buf + header_len, data_len);

    *data_out = data;
    *len_out  = data_len;

    free(buf);
    return 0;
}
