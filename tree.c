#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;
    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;
        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = (size_t)tree->count * 296;
    if (max_size == 0) max_size = 1;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;
    Tree sorted = *tree;
    qsort(sorted.entries, sorted.count, sizeof(TreeEntry), compare_tree_entries);
    size_t offset = 0;
    for (int i = 0; i < sorted.count; i++) {
        const TreeEntry *e = &sorted.entries[i];
        int w = sprintf((char *)buffer + offset, "%o %s", e->mode, e->name);
        offset += w + 1;
        memcpy(buffer + offset, e->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }
    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

static int build_tree(IndexEntry *entries, int count, int prefix_len, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;
    int i = 0;
    while (i < count) {
        const char *rel = entries[i].path + prefix_len;
        const char *slash = strchr(rel, '/');
        if (!slash) {
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            i++;
        } else {
            size_t dlen = (size_t)(slash - rel);
            char dname[256];
            if (dlen >= sizeof(dname)) return -1;
            memcpy(dname, rel, dlen); dname[dlen] = '\0';
            int j = i + 1;
            while (j < count) {
                const char *o = entries[j].path + prefix_len;
                if (strncmp(o, dname, dlen) != 0 || o[dlen] != '/') break;
                j++;
            }
            ObjectID sub;
            if (build_tree(entries + i, j - i, prefix_len + (int)dlen + 1, &sub) != 0)
                return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = sub;
            strncpy(te->name, dname, sizeof(te->name) - 1);
            i = j;
        }
    }
    void *data; size_t dlen2;
    if (tree_serialize(&tree, &data, &dlen2) != 0) return -1;
    int ret = object_write(OBJ_TREE, data, dlen2, id_out);
    free(data);
    return ret;
}

static int cmp_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int tree_from_index(ObjectID *id_out) {
    Index *index = malloc(sizeof(Index));  /* heap — avoids 5.6MB stack overflow */
    if (!index) return -1;
    if (index_load(index) != 0) { free(index); return -1; }
    qsort(index->entries, index->count, sizeof(IndexEntry), cmp_path);
    int ret;
    if (index->count == 0) {
        Tree empty; empty.count = 0;
        void *data; size_t dlen;
        tree_serialize(&empty, &data, &dlen);
        ret = object_write(OBJ_TREE, data, dlen, id_out);
        free(data);
    } else {
        ret = build_tree(index->entries, index->count, 0, id_out);
    }
    free(index);
    return ret;
}

