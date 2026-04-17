#include "index.h"
#include "pes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// declare object_write
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─────────────────────────────────────────────

// find entry
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// remove entry
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i],
                        &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// status
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged = 0;

    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged++;
    }

    if (!staged) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged = 0;

    for (int i = 0; i < index->count; i++) {
        struct stat st;

        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged++;
            }
        }
    }

    if (!unstaged) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked = 0;

    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;

        while ((ent = readdir(dir)) != NULL) {
            if (!strcmp(ent->d_name, ".") ||
                !strcmp(ent->d_name, "..") ||
                !strcmp(ent->d_name, ".pes") ||
                !strcmp(ent->d_name, "pes"))
                continue;

            int tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (!strcmp(index->entries[i].path, ent->d_name)) {
                    tracked = 1;
                    break;
                }
            }

            if (!tracked) {
                struct stat st;
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked++;
                }
            }
        }
        closedir(dir);
    }

    if (!untracked) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─────────────────────────────────────────────

// load index
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char hash_hex[HASH_HEX_SIZE + 1];

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];

        if (fscanf(f, "%o %64s %ld %u %255s",
                   &e->mode,
                   hash_hex,
                   &e->mtime_sec,
                   &e->size,
                   e->path) != 5)
            break;

        hex_to_hash(hash_hex, &e->hash);
        index->count++;
    }

    fclose(f);
    return 0;
}

// sort helper
static int cmp_index(const void *a, const void *b) {
    return strcmp(((IndexEntry*)a)->path,
                  ((IndexEntry*)b)->path);
}

// save index
int index_save(const Index *index) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    Index sorted = *index;
    qsort(sorted.entries, sorted.count,
          sizeof(IndexEntry), cmp_index);

    char hex[HASH_HEX_SIZE + 1];

    for (int i = 0; i < sorted.count; i++) {
        hash_to_hex(&sorted.entries[i].hash, hex);

        fprintf(f, "%o %s %ld %u %s\n",
                sorted.entries[i].mode,
                hex,
                sorted.entries[i].mtime_sec,
                sorted.entries[i].size,
                sorted.entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    rename(tmp, INDEX_FILE);
    return 0;
}

// ─────────────────────────────────────────────

// FINAL FIXED index_add
int index_add(Index *index, const char *path) {
    if (!index || !path) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    // 🔥 CRITICAL FIX
    if (size <= 0) {
        fclose(f);
        return -1;
    }

    void *data = malloc(size);
    if (!data) {
        fclose(f);
        return -1;
    }

    if (fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }

    fclose(f);

    ObjectID id;
    if (object_write(OBJ_BLOB, data, size, &id) != 0) {
        free(data);
        return -1;
    }

    free(data);

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "Index full\n");
            return -1;
        }

        e = &index->entries[index->count];
        memset(e, 0, sizeof(IndexEntry));
        index->count++;
    }

    e->mode = st.st_mode;
    e->mtime_sec = st.st_mtime;
    e->size = st.st_size;

    memcpy(&e->hash, &id, sizeof(ObjectID));
    snprintf(e->path, sizeof(e->path), "%s", path);

    return index_save(index);
}
