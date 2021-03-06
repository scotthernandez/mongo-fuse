#define FUSE_USE_VERSION 26

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <bson.h>
#include <mongo.h>
#include "mongo-fuse.h"

extern char * extents_name;

struct elist * init_elist() {
	const size_t malloc_size = sizeof(struct elist) +
		(sizeof(struct enode) * BLOCKS_PER_EXTENT);
	struct elist * out = malloc(malloc_size);
	if(!out)
		return NULL;
	memset(out, 0, sizeof(struct elist));
	out->nslots = BLOCKS_PER_EXTENT;
	return out;
}

int ensure_elist(struct elist ** pout) {
	struct elist * out = *pout;
	if(out == NULL) {
		out = init_elist();
		if(!out) {
			*pout = NULL;
			return -ENOMEM;
		}
		*pout = out;
	}
	if(out->nnodes + 1 < out->nslots)
		return 0;
	out->nslots += BLOCKS_PER_EXTENT;
	out = realloc(out, sizeof(struct elist) +
		(sizeof(struct enode) * out->nslots));
	if(!out)
		return -ENOMEM;
	*pout = out;
	return 0;
}

int insert_hash(struct elist ** pout, off_t off, size_t len,
	uint8_t hash[HASH_LEN]) {
	int res, idx;
	if((res = ensure_elist(pout)) != 0)
		return res;

	struct elist * out = *pout;

	idx = out->nnodes++;
	out->list[idx].off = off;
	out->list[idx].len = len;
	out->list[idx].empty = 0;
	out->list[idx].seq = idx;
	memcpy(out->list[idx].hash, hash, HASH_LEN);

	return 0;
}

int insert_empty(struct elist ** pout, off_t off, size_t len) {
	int res, idx;
	if((res = ensure_elist(pout)) != 0)
		return res;

	struct elist * out = *pout;

	idx = out->nnodes++;
	out->list[idx].off = off;
	out->list[idx].len = len;
	out->list[idx].empty = 1;
	out->list[idx].seq = idx;
	
	return 0;
}

int enode_cmp(const void * ra, const void * rb) {
	struct enode * a = (struct enode *)ra;
	struct enode * b = (struct enode *)rb;

	int res = (a->seq > b->seq) - (a->seq < b->seq);
	if(res == 0)
		res = (a->off > b->off) - (a->off < b->off);
	return res;
}

int serialize_extent(struct inode * e, struct elist * list) {
	mongo * conn = get_conn();
	bson doc, cond;
	int res, idx, towrite = 0;

	if(list->nnodes == 0)
		return 0;
	qsort(list->list, list->nnodes, sizeof(struct enode), enode_cmp);

	for(idx = 0; idx < list->nnodes;) {
		bson_oid_t docid;
		off_t last_end = 0;
		struct enode * cur = &list->list[idx];
		const off_t cur_start = cur->off;
		int nhashes = 0;

		bson_oid_gen(&docid);
		bson_init(&doc);
		bson_append_oid(&doc, "_id", &docid);
		bson_append_oid(&doc, "inode", &e->oid);
		bson_append_long(&doc, "start", cur->off);
		bson_append_start_array(&doc, "blocks");
		towrite = 0;
		for(; idx < list->nnodes; idx++) {
			cur = &list->list[idx];
			char idxstr[10];

			if(last_end > 0 && cur->off != last_end)
				break;
			
			bson_numstr(idxstr, nhashes++);
			bson_append_start_object(&doc, idxstr);
			if(cur->empty)
				bson_append_null(&doc, "hash");
			else
				bson_append_binary(&doc, "hash", 0,
					(const char*)cur->hash, HASH_LEN);
			bson_append_int(&doc, "len", cur->len);
			bson_append_finish_object(&doc);

			last_end = cur->off + cur->len;
			towrite++;
		}
		
		bson_append_finish_array(&doc);
		bson_append_long(&doc, "end", last_end);
		bson_finish(&doc);

		res = mongo_insert(conn, extents_name, &doc, NULL);
		bson_destroy(&doc);
		if(res != MONGO_OK) {
			fprintf(stderr, "Error inserting extent\n");
			return -EIO;
		}

		bson_init(&cond);
		bson_append_oid(&cond, "inode", &e->oid);
		bson_append_start_object(&cond, "start");
		bson_append_long(&cond, "$gte", cur_start);
		bson_append_finish_object(&cond);
		bson_append_start_object(&cond, "end");
		bson_append_long(&cond, "$lte", last_end);
		bson_append_finish_object(&cond);
		bson_append_start_object(&cond, "_id");
		bson_append_oid(&cond, "$lt", &docid);
		bson_append_finish_object(&cond);
		bson_finish(&cond);

		res = mongo_remove(conn, extents_name, &cond, NULL);
		bson_destroy(&cond);

		if(res != MONGO_OK) {
			fprintf(stderr, "Error cleaning up extents\n");
			return -EIO;
		}
	}

	list->nnodes = 0;
	return 0;
}

int deserialize_extent(struct inode * e, off_t off, size_t len, struct elist ** pout) {
	bson cond;
	mongo * conn = get_conn();
	mongo_cursor curs;
	int res;
	const off_t end = off + len;
	struct elist * out = NULL;

	/* start <= end && end >= start */
	bson_init(&cond);
	bson_append_start_object(&cond, "$query");
	bson_append_oid(&cond, "inode", &e->oid);
	bson_append_start_object(&cond, "start");
	bson_append_long(&cond, "$lte", off + len);
	bson_append_finish_object(&cond);
	bson_append_start_object(&cond, "end");
	bson_append_long(&cond, "$gte", off);
	bson_append_finish_object(&cond);
	bson_append_finish_object(&cond);
	bson_append_start_object(&cond, "$orderby");
	bson_append_int(&cond, "start", 1);
	bson_append_int(&cond, "_id", 1);
	bson_append_finish_object(&cond);
	bson_finish(&cond);

	mongo_cursor_init(&curs, conn, extents_name);
	mongo_cursor_set_query(&curs, &cond);

	while((res = mongo_cursor_next(&curs)) == MONGO_OK) {
		const bson * curdoc = mongo_cursor_bson(&curs);
		bson_iterator topi, i, sub;
		bson_type bt;
		off_t curoff = 0;
		const char * key;
		bson_iterator_init(&topi, curdoc);
		while(bson_iterator_next(&topi) != 0) {
			key = bson_iterator_key(&topi);
			if(strcmp(key, "blocks") == 0)
				bson_iterator_subiterator(&topi, &i);
			else if(strcmp(key, "start") == 0)
				curoff = bson_iterator_long(&topi);
		}

		while(bson_iterator_next(&i) != 0) {
			bson_iterator_subiterator(&i, &sub);
			uint8_t * hash = NULL;
			int curlen = 0;
			off_t curend;
			int empty = 0;
			while((bt = bson_iterator_next(&sub)) != 0) {
				key = bson_iterator_key(&sub);
				if(strcmp(key, "hash") == 0) {
					if(bt == BSON_NULL)
						empty = 1;
					else
						hash = (uint8_t*)bson_iterator_bin_data(&sub);
				}
				else if(strcmp(key, "len") == 0)
					curlen = bson_iterator_int(&sub);
			}

			curend = curoff + curlen;
			if(!(curoff < end && curend > off)) {
				curoff += curlen;
				continue;
			}

			if(empty)
				res = insert_empty(&out, curoff, curlen);
			else
				res = insert_hash(&out, curoff, curlen, hash);
			if(res != 0) {
				fprintf(stderr, "Error adding hash to extent tree\n");
				return res;
			}
			curoff += curlen;
		}
	}
	mongo_cursor_destroy(&curs);
	bson_destroy(&cond);
	*pout = out;

	return 0;
}
