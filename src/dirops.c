#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <mongo.h>
#include <stdlib.h>
#include <search.h>
#include <sys/stat.h>
#include "mongo-fuse.h"
#include <osxfuse/fuse.h>
#include <limits.h>

extern const char * inodes_name;
extern const char * dbname;
extern const char * inodes_coll;

struct readdir_data {
    fuse_fill_dir_t filler;
    void * buf;
};

int read_dirents(const char * directory,
    int (*dirent_cb)(struct inode *e, void * p,
    const char * parent, size_t parentlen), void * p) {
    bson query, fields;
    mongo_cursor curs;
    size_t pathlen = strlen(directory);
    char regexp[PATH_MAX + 10];
    int res;
    mongo * conn = get_conn();

    sprintf(regexp, "^%s/[^/]+$", pathlen == 1 ? directory + 1 : directory);
    bson_init(&query);
    bson_append_regex(&query, "dirents", regexp, "");
    bson_finish(&query);

    bson_init(&fields);
    bson_append_int(&fields, "data", 0);
    bson_finish(&fields);

    mongo_cursor_init(&curs, conn, inodes_name);
    mongo_cursor_set_query(&curs, &query);
    mongo_cursor_set_fields(&curs, &fields);

    while((res = mongo_cursor_next(&curs)) == MONGO_OK) {
        struct inode e;
        struct dirent * cde, *last = NULL;
        const char * filename;
        res = read_inode(mongo_cursor_bson(&curs), &e);
        if(res != 0) {
            fprintf(stderr, "Error in read_inode\n");
            break;
        }

        cde = e.dirents;
        while(cde) {
            if(strncmp(cde->path, directory, pathlen) != 0) {
                last = cde;
                cde = cde->next;
                continue;
            }

            if(last) {
                last->next = cde->next;
                cde->next = e.dirents;
                e.dirents = cde;
            }
            break;
        }

        filename = cde->path + cde->len;
        while(*(filename - 1) != '/')
            filename--;
        if(!(strcmp(filename, ".snapshot") == 0 && e.mode & S_IFDIR))
            res = dirent_cb(&e, p, directory, pathlen);
        free_inode(&e);
        if(res != 0)
            break;
    }
    bson_destroy(&query);
    bson_destroy(&fields);
    mongo_cursor_destroy(&curs);

    if(curs.err != MONGO_CURSOR_EXHAUSTED) {
        fprintf(stderr, "Error listing directory contents\n");
        return -EIO;
    }
    return 0;
}

int readdir_cb(struct inode * e, void * p,
    const char * parent, size_t parentlen) {
    struct readdir_data * rd = (struct readdir_data*)p;
    struct stat stbuf;
    size_t printlen = parentlen;
    if(parentlen > 1)
        printlen++;

    stbuf.st_nlink = 1;
    stbuf.st_mode = e->mode;
    if(stbuf.st_mode & S_IFDIR)
        stbuf.st_nlink++;
    stbuf.st_uid = e->owner;
    stbuf.st_gid = e->group;
    stbuf.st_size = e->size;
    stbuf.st_ctime = e->created;
    stbuf.st_mtime = e->modified;
    stbuf.st_atime = e->modified;
    stbuf.st_dev = e->dev;

    rd->filler(rd->buf, e->dirents->path + printlen, &stbuf, 0);
    return 0;
}

int mongo_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *fi) {

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    struct readdir_data rb = {
        .filler = filler,
        .buf = buf
    };

    return read_dirents(path, readdir_cb, &rb);
}

int mongo_mkdir(const char * path, mode_t mode) {
    char snapshotdir[PATH_MAX];
    int res, isroot = (*(path + 1) == '\0');

    sprintf(snapshotdir, "%s/.snapshot", isroot ? path + 1 : path);
    if((res = create_inode(path, mode | S_IFDIR, NULL)) != 0)
        return res;

    return create_inode(snapshotdir, mode | S_IFDIR, NULL);
}

int orphan_snapshot(struct inode *e, void * p,
    const char * parent, size_t parentlen) {
    const char * topparent = (const char*)p;
    const char * shortname = e->dirents->path + e->dirents->len;
    int rootlen = strlen(topparent);
    while(topparent[--rootlen] != '/');
    int res, nslashes = 1;
    struct dirent * save = e->dirents;

    if(e->mode & S_IFDIR) {
            nslashes--;
        if((res = read_dirents(e->dirents->path,
            orphan_snapshot, (void*)topparent)) != 0)
            return res;
    }

    while(nslashes > 0 || *(shortname - 1) != '/') {
        if(*(shortname - 1) == '/')
            nslashes--;
        shortname--;
    }

    struct dirent * nd = malloc(sizeof(struct dirent) + PATH_MAX);
    if(strcmp(shortname, ".snapshot") == 0) {
        nd->len = sprintf(nd->path, "/%*.s.snapshot/orphaned-%s",
            rootlen, topparent, topparent + rootlen + 1);
    } else {
        nd->len = sprintf(nd->path, "/%*.s.snapshot/orphaned-%s/%s",
            rootlen, topparent, topparent + rootlen + 1, shortname);
    }
    free(nd);
    nd->next = e->dirents->next;
    e->dirents = nd;
    commit_inode(e);
    e->dirents = save;

    return 0;
}

int mongo_rmdir(const char * path) {
    struct inode e;
    int res;
    double dres;
    bson cond;
    char regexp[PATH_MAX + 25];
    mongo * conn = get_conn();

    sprintf(regexp, "^%s/[^/]+(?<!\\.snapshot)$", path);
    bson_init(&cond);
    bson_append_regex(&cond, "dirents", regexp, "");
    bson_finish(&cond);

    dres = mongo_count(conn, dbname, inodes_coll, &cond);
    bson_destroy(&cond);

    if(dres > 0)
        return -ENOTEMPTY;

    sprintf(regexp, "%s/.snapshot", path);
    if((res = get_inode(regexp, &e)) != 0)
        return res;

    res = orphan_snapshot(&e, (void*)path, NULL, 0);
    free_inode(&e);
    if(res != 0)
        return res;

    if((res = get_inode(path, &e)) != 0)
        return res;

    bson_init(&cond);
    bson_append_oid(&cond, "_id", &e.oid);
    bson_finish(&cond);

    res = mongo_remove(conn, inodes_name, &cond, NULL);
    bson_destroy(&cond);
    if(res != MONGO_OK) {
        fprintf(stderr, "Error removing inode entry for %s\n", path);
        return -EIO;
    }
    return 0;
}

int create_snapshot(struct inode * e, void * p, const char * parent, size_t plen) {
    int generation = *(int*)p;
    bson_oid_t newid;
    int res;
    const char * path = e->dirents->path;
    size_t pathlen = e->dirents->len;
    char * filename = (char*)path + pathlen;
    uint64_t off = 0;

    if(e->mode & S_IFDIR)
        return 0;

    bson_oid_gen(&newid);
    while(*(filename-1) != '/') filename--;

    while(off < e->size) {
        if((res = get_blockmap(e, off)) < 0)
            return res;
        struct block_map * map = e->maps[res];
        memcpy(&map->inode, &newid, sizeof(bson_oid_t));
        map->updated = 0;
        memset(map->changed, 1, sizeof(map->changed));
        if((res = commit_blockmap(e, e->maps[res])) != 0)
            return res;
        off += BLOCKS_PER_MAP * e->blocksize;
    }

    memcpy(&e->oid, &newid, sizeof(bson_oid_t));

    struct dirent * d = malloc(sizeof(struct dirent) + pathlen + 21);
    d->len = sprintf(d->path, "%s/.snapshot/%d/%s", parent, generation, filename);
    d->next = NULL;

    struct dirent * freeme = e->dirents;
    while(freeme) {
        struct dirent * freeme_next = freeme->next;
        free(freeme);
        freeme = freeme_next;
    }
    e->dirents = d;
    e->direntcount = 1;

    res = commit_inode(e);
    return res;
}

int snapshot_dir(const char * path, size_t pathlen, mode_t mode) {
    char dirpath[PATH_MAX + 1], regexp[PATH_MAX + 1];
    bson cond;
    mongo * conn = get_conn();
    double count;
    int icount, res;

    strcpy(dirpath, path);
    while(dirpath[pathlen] != '/') pathlen--;
    dirpath[pathlen] = '\0';

    sprintf(regexp, "^%s/.snapshot/\\d+$",
        pathlen == 1 ? dirpath + 1 : dirpath);
    bson_init(&cond);
    bson_append_regex(&cond, "dirents", regexp, "");
    bson_finish(&cond);

    count = mongo_count(conn, dbname, "inodes", &cond);
    bson_destroy(&cond);
    icount = count + 1;

    sprintf(regexp, "%s/.snapshot/%d", dirpath, icount);
    if((res = create_inode(regexp, mode, NULL)) != 0)
        return res;

    return read_dirents(dirpath, create_snapshot, &icount);
}

int mongo_rename(const char * path, const char * newpath) {
    mongo * conn = get_conn();
    bson query, doc;
    int res;

    bson_init(&query);
    bson_append_string(&query, "dirents", path);
    bson_finish(&query);

    bson_init(&doc);
    bson_append_start_object(&doc, "$set");
    bson_append_string(&doc, "dirents.$", newpath);
    bson_append_finish_object(&doc);
    bson_finish(&doc);

    res = mongo_update(conn, inodes_name, &query, &doc,
        MONGO_UPDATE_BASIC, NULL);
    return res == MONGO_OK ? 0:-EIO;
}

