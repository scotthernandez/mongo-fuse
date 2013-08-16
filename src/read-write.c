#define FUSE_USE_VERSION 26

#include <osxfuse/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <mongo.h>
#include <stdlib.h>
#include <search.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include "mongo-fuse.h"

int mongo_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    struct inode e;
    int res;
    struct extent * o;
    char * block = buf;
    size_t tocopy, start;
    const off_t end = size + offset;

    if((res = get_inode(path, &e)) != 0)
        return res;

    if(e.mode & S_IFDIR)
        return -EISDIR;

    if(offset > e.size)
        return 0;

    while(block - buf != size) {
        start = compute_start(&e, offset);
        fprintf(stderr, "Resolving block %lu\n", start);
        if((res = resolve_extent(&e, start, &o, 1)) != 0)
            return res;

        char * elock;
        size_t curblocksize = e.blocksize, sizediff = offset - start;
        if(offset > start)
            curblocksize -= sizediff;
        if(!o) {
            memset(block, 0, curblocksize);
            block += curblocksize;
            continue;
        } else if(block - buf == 0)
            elock = o->data + sizediff;
        else
            elock = o->data;

        if(end - offset < curblocksize)
            tocopy = end - offset;
        else
            tocopy = curblocksize;

        memcpy(block, elock, tocopy);
        block += tocopy;
        offset += tocopy;
    }

    add_block_stat(path, size, 0);
    return block - buf;
}

int mongo_write(const char *path, const char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
    struct inode e;
    int res;
    char * block = (char*)buf;
    const off_t end = size + offset;
    off_t curblock;
    size_t tocopy;

    if((res = get_inode(path, &e)) != 0)
        return res;

    if(e.mode & S_IFDIR)
        return -EISDIR;

    curblock = compute_start(&e, offset);
    while(offset < end) {
        size_t curblocksize = e.blocksize, sizediff = 0;
        struct extent * cur = NULL;
        int getdata = (offset != curblock || end - curblock < e.blocksize);
        char * elock;

        if((res = resolve_extent(&e, curblock, &cur, getdata)) != 0)
            return res;

        if(!cur) {
            cur = new_extent(&e);
            cur->start = curblock;
        }

        if(curblock < offset) {
            sizediff = offset - curblock;
            elock = cur->data + sizediff;
            curblocksize = e.blocksize - sizediff;
        } else
            elock = cur->data;

        if(end - offset < curblocksize)
            tocopy = end - offset;
        else
            tocopy = curblocksize;

        memcpy(elock, block, tocopy);
        if((res = commit_extent(&e, cur)) != 0)
            goto cleanup;

        block += tocopy;
        offset += tocopy;
        curblock = compute_start(&e, offset);
    }

    if(end > e.size)
        e.size = end;
    res = commit_inode(&e);

cleanup:
    free_inode(&e);
    if(res != 0)
        return res;
    else
        add_block_stat(path, size, 1);
    return size;
}

