/* String -> String Map data structure optimized for size.
 * This file implements a data structure mapping strings to other strings
 * implementing an O(n) lookup data structure designed to be very memory
 * efficient.
 *
 * The Redis Hash type uses this data structure for hashes composed of a small
 * number of elements, to switch to a hash table once a given number of
 * elements is reached.
 *
 * Given that many times Redis Hashes are used to represent objects composed
 * of few fields, this is a very big win in terms of used memory.
 *
 * --------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Memory layout of a zipmap, for the map "foo" => "bar", "hello" => "world":
 *
 * <zmlen><len>"foo"<len><free>"bar"<len>"hello"<len><free>"world"
 *
 * <zmlen> is 1 byte length that holds the current size of the zipmap.
 * When the zipmap length is greater than or equal to 254, this value
 * is not used and the zipmap needs to be traversed to find out the length.
 * zmlen占1byte 8bit 能够表示的数值上限是2^-1 存储着zipmap实例中存储的键值对个数
 * 当zipmap键值对个数<254个 就直接通过zmlen这个字段查询出来zipmap存储的键值对个数
 * 当zipmap键值对个数>=254个 需要通过遍历方式计算出zipmap存储的键值对个数
 *
 * <len> is the length of the following string (key or value).
 * <len> lengths are encoded in a single value or in a 5 bytes value.
 * If the first byte value (as an unsigned 8 bit value) is between 0 and
 * 253, it's a single-byte length. If it is 254 then a four bytes unsigned
 * integer follows (in the host byte ordering). A value of 255 is used to
 * signal the end of the hash.
 * len表示key或者value字符串长度 len被编码成1个byte或者5个byte 根据字符串长度而定
 * 要表示的长度[0...253] len就用占1个字节 这个字节存储字符串长度
 * 要表示的长度>=254 len占用5个字节
 *
 * <free> is the number of free unused bytes after the string, resulting
 * from modification of values associated to a key. For instance if "foo"
 * is set to "bar", and later "foo" will be set to "hi", it will have a
 * free byte to use if the value will enlarge again later, or even in
 * order to add a key/value pair if it fits.
 *
 * <free> is always an unsigned 8 bit number, because if after an
 * update operation there are more than a few free bytes, the zipmap will be
 * reallocated to make sure it is as small as possible.
 * free字段占用1个byte 表示的是value长度的空闲值 因为value可能经常变 所以申请好了的内存可能有空闲空间
 *
 * The most compact representation of the above two elements hash is actually:
 *
 * "\x02\x03foo\x03\x00bar\x05hello\x05\x00world\xff"
 *
 * Note that because keys and values are prefixed length "objects",
 * the lookup will take O(N) where N is the number of elements
 * in the zipmap and *not* the number of bytes needed to represent the zipmap.
 * This lowers the constant times considerably.
 */

#include <stdio.h>
#include <string.h>
#include "zmalloc.h"
#include "endianconv.h"

#define ZIPMAP_BIGLEN 254
#define ZIPMAP_END 255

/* The following defines the max value for the <free> field described in the
 * comments above, that is, the max number of trailing bytes in a value. */
#define ZIPMAP_VALUE_MAX_FREE 4

/* The following macro returns the number of bytes needed to encode the length
 * for the integer value _l, that is, 1 byte for lengths < ZIPMAP_BIGLEN and
 * 5 bytes for all the other lengths. */
#define ZIPMAP_LEN_BYTES(_l) (((_l) < ZIPMAP_BIGLEN) ? 1 : sizeof(unsigned int)+1)

/* Create a new empty zipmap. */
// @return zm实例的地址
unsigned char *zipmapNew(void) {
    // 申请2个字节 给zipmap填充两个字
    // zmlen end
    // \0x00\0xff
    unsigned char *zm = zmalloc(2);
    // zmlen字段 0个键值对
    zm[0] = 0; /* Length */
    // zipmap结束符 0xff
    zm[1] = ZIPMAP_END;
    return zm;
}

/* Decode the encoded length pointed by 'p' */
// 读取zipmap中entry节点的的len字段值
// key\value的长度使用len表示 len要么1 byte 要么5 bytes
// @param p 指向entry的指针
// @return entry中的key或者value的len的值
static unsigned int zipmapDecodeLength(unsigned char *p) {
    unsigned int len = *p;
    // 字符串长度[0...253]通过1个byte表示
    if (len < ZIPMAP_BIGLEN) return len;
    // len字段5个byte 第1个byte标识符填充254 字符串长度通过后4 bytes表示
    memcpy(&len,p+1,sizeof(unsigned int)); // 读取后4个bytes上的值
    memrev32ifbe(&len);
    return len;
}

static unsigned int zipmapGetEncodedLengthSize(unsigned char *p) {
    return (*p < ZIPMAP_BIGLEN) ? 1: 5;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. */
// 函数有2个功能
//             除非返回编码len需要几个byte之外
//             如果给定了entry节点p 还需要对entry的len字段进行编码写入
// @param p entry节点
// @param len len字段的值
// @return len字段几个byte 需要多大内存编码len字段
static unsigned int zipmapEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return ZIPMAP_LEN_BYTES(len);
    } else {
        if (len < ZIPMAP_BIGLEN) {
            p[0] = len;
            return 1;
        } else {
            p[0] = ZIPMAP_BIGLEN;
            memcpy(p+1,&len,sizeof(len));
            memrev32ifbe(p+1);
            return 1+sizeof(len);
        }
    }
}

/* Search for a matching key, returning a pointer to the entry inside the
 * zipmap. Returns NULL if the key is not found.
 *
 * If NULL is returned, and totlen is not NULL, it is set to the entire
 * size of the zipmap, so that the calling function will be able to
 * reallocate the original zipmap to make room for more entries. */
// 遍历zipmap
// 不传key 传totlen 则读取出zipmap的zmlen字段 也就是zipmap有多少个字节
// 传key 不传totlen 则搜索key 找到了就返回entry节点
// 传key 传totlen 则搜索key 找到了就返回entry节点 并且计算zipmap占多少个字节 放到totlen上
// @param zm zipmap实例
// @param key key字符串
// @param klen key的长度
// @param totlen 遍历完zipmap可以将zipmap的zmlen值记录在totlen上
// @return key所代表的entry的地址
static unsigned char *zipmapLookupRaw(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned int *totlen) {
    // zm实例地址+1 如果zm中是空的也就是没有entry节点那么p指向end节点 如果zm中有entry节点那么p指向首个entry节点
    unsigned char *p = zm+1, *k = NULL;
    unsigned int l,llen;

    // 遍历整个zipmap所有的entry节点
    // 遍历entry节点过程中记录搜索到的key对应的节点
    while(*p != ZIPMAP_END) {
        unsigned char free;

        /* Match or skip the key */
        // entry节点的key的len字段值 key的字符串长度
        l = zipmapDecodeLength(p);
        // key的len字段几个byte 要么1要么5
        llen = zipmapEncodeLength(NULL,l);
        // memcmp函数比较两个地址开始的l长度 都相同返回0
        // p现在指向entry len字段占用llen个byte
        // p+llen指向字符串key
        if (key != NULL && k == NULL && l == klen && !memcmp(p+llen,key,l)) {
            /* Only return when the user doesn't care
             * for the total length of the zipmap. */
            // 找到了key的键值对
            if (totlen != NULL) {
                k = p;
            } else {
                return p;
            }
        }
        p += llen+l; // p指向value的len字段
        /* Skip the value as well */
        l = zipmapDecodeLength(p); // value的长度 value的len的值
        p += zipmapEncodeLength(NULL,l); // value的len的编码需要几个byte p指针后移到value的free字段
        free = p[0]; // value的free占1byte 读取free字段的值
        p += l+1+free; /* +1 to skip the free byte */ // p后移至下一个entry节点
    }
    if (totlen != NULL) *totlen = (unsigned int)(p-zm)+1; // 整个zipmap占多少个字节 记录在totlen上
    return k;
}

// 一个entry节点内存布局
// \len\key\len\free\value
// len->1个byte或者4个byte 具体根据key和value长度而定
// free->1个byte 并且初始化的时候free上存储的值为0x00
// @param klen key的字符串长度
// @param vlen value的字符串长度
// @return 编码[key, value]键值对需要多大内存
static unsigned long zipmapRequiredLength(unsigned int klen, unsigned int vlen) {
    unsigned int l;
    // key长度+value长度+len至少1个byte+len至少1个byte+free 1个byte
    l = klen+vlen+3;
    // 字符串长度>=0xfe时 len字段需要5个byte
    if (klen >= ZIPMAP_BIGLEN) l += 4;
    if (vlen >= ZIPMAP_BIGLEN) l += 4;
    return l;
}

/* Return the total amount used by a key (encoded length + payload) */
// @param p entry节点
// @return entry节点中编码key需要几个字节 编码key的len字段+key本身
static unsigned int zipmapRawKeyLength(unsigned char *p) {
    // entry节点中key的len值
    unsigned int l = zipmapDecodeLength(p);
    // 编码len需要几个byte
    // l是key这个string需要几个byte
    return zipmapEncodeLength(NULL,l) + l;
}

/* Return the total amount used by a value
 * (encoded length + single byte free count + payload) */
// @param p entry节点
// @return entry节点编码value需要的字节数(value的len需要的字节数+value的free需要的字节数+value本身需要的字节数+value空闲的字节数)
static unsigned int zipmapRawValueLength(unsigned char *p) {
    // value的len的值
    unsigned int l = zipmapDecodeLength(p);
    unsigned int used;

    used = zipmapEncodeLength(NULL,l); // 编码value的len需要的字节数
    // p[used]就是读取了free字段的值
    // 空闲字节数+free字段占1字节+value字符串所需字节数
    used += p[used] + 1 + l;
    return used;
}

/* If 'p' points to a key, this function returns the total amount of
 * bytes used to store this entry (entry = key + associated value + trailing
 * free space if any). */
// @param p entry节点
// @return entry节点需要的字节数
static unsigned int zipmapRawEntryLength(unsigned char *p) {
    // entry中key需要的字节数(key的len字段需要的字节数+key本身字节数)
    unsigned int l = zipmapRawKeyLength(p);
    // entry中value需要的字节数(value的len字段需要的字节数+free字段需要的字节数+value本身需要的字节数+空闲的字节数)
    return l + zipmapRawValueLength(p+l);
}

static inline unsigned char *zipmapResize(unsigned char *zm, unsigned int len) {
    zm = zrealloc(zm, len);
    zm[len-1] = ZIPMAP_END;
    return zm;
}

/* Set key to value, creating the key if it does not already exist.
 * If 'update' is not NULL, *update is set to 1 if the key was
 * already preset, otherwise to 0. */
// [key, value]键值对的设置 可能是新增 可能是更新
// @param zm zipmap实例
// @param key 字符串key
// @param klen 字符串key的长度
// @param value 字符串value
// @param vlen 字符串value的长度
// @param update 标识符 通知给调用方 update不为null就要把key是否存在的情况汇报出去
//                                key已经存在 update->1
//                                key不存在 update->0
unsigned char *zipmapSet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char *val, unsigned int vlen, int *update) {
    // zmlen记录zm实例占多少个字节
    unsigned int zmlen, offset;
    // reqlen 一个新的entry需要多少个byte
    unsigned int freelen, reqlen = zipmapRequiredLength(klen,vlen);
    unsigned int empty, vempty;
    unsigned char *p;

    freelen = reqlen;
    if (update) *update = 0;
    // 在zipmap上搜索key的entry节点 并把zipmap的大小记录在zmlen上(zm实例占多少字节)
    p = zipmapLookupRaw(zm,key,klen,&zmlen);
    if (p == NULL) { // zm实例中没有key 则属于新增entry场景
        /* Key not found: enlarge */
        // 新增一个entry 对zm实例进行扩容 新节点挂到最后
        zm = zipmapResize(zm, zmlen+reqlen);
        // 新增节点在新zm实例中地址
        p = zm+zmlen-1;
        zmlen = zmlen+reqlen; // 新zm多大 占用多少字节

        /* Increase zipmap length (this is an insert) */
        if (zm[0] < ZIPMAP_BIGLEN) zm[0]++; // 新增了1个entry节点 更新zm实例的zmlen字段值 计数加1
    } else { // zm实例中已经存在key 属于更新entry场景
        /* Key found. Is there enough space for the new value? */
        /* Compute the total length: */
        if (update) *update = 1; // 汇报给调用 标识存在key 属于更新行为
        // entry节点p占用的字节数
        freelen = zipmapRawEntryLength(p);
        // 当前entry节点p已经占用了freelen个字节 现在要对节点进行更新 更新后的节点需要占用reqlen个字节
        if (freelen < reqlen) { // 需要扩容
            /* Store the offset of this key within the current zipmap, so
             * it can be resized. Then, move the tail backwards so this
             * pair fits at the current position. */
            // 目标节点在zm实例中相对位置
            offset = p-zm;
            // zm实例扩容
            zm = zipmapResize(zm, zmlen-freelen+reqlen);
            // 重新定位到目标节点
            p = zm+offset;

            /* The +1 in the number of bytes to be moved is caused by the
             * end-of-zipmap byte. Note: the *original* zmlen is used. */
            // 目标节点上内容复制
            memmove(p+reqlen, p+freelen, zmlen-(offset+freelen+1));
            zmlen = zmlen-freelen+reqlen; // 更新zm实例占用的字节数
            freelen = reqlen;
        }
    }

    /* We now have a suitable block where the key/value entry can
     * be written. If there is too much free space, move the tail
     * of the zipmap a few bytes to the front and shrink the zipmap,
     * as we want zipmaps to be very space efficient. */
    empty = freelen-reqlen; // entry节点的空闲字节数
    // zipmap的空闲字节数是有优化的 不允许过大的空闲字节 有过大的空闲字节会回收
    if (empty >= ZIPMAP_VALUE_MAX_FREE) {
        /* First, move the tail <empty> bytes to the front, then resize
         * the zipmap to be <empty> bytes smaller. */
        offset = p-zm;
        memmove(p+reqlen, p+freelen, zmlen-(offset+freelen+1));
        zmlen -= empty;
        zm = zipmapResize(zm, zmlen);
        p = zm+offset;
        vempty = 0;
    } else {
        vempty = empty;
    }

    /* Just write the key + value and we are done. */
    /* Key: */
    // 至此 不管是新增还是更新的场景 现在节点的内存已经准备好 就逐个写字段即可
    // \len\key\len\free\value
    // 向entry中写入key的len字段
    p += zipmapEncodeLength(p,klen);
    // 向entry中写入key
    memcpy(p,key,klen);
    p += klen;
    /* Value: */
    // 向entry中写入value的len字段
    p += zipmapEncodeLength(p,vlen);
    // 向entry中写入free字段
    *p++ = vempty;
    // 向entry中写入value
    memcpy(p,val,vlen);
    return zm;
}

/* Remove the specified key. If 'deleted' is not NULL the pointed integer is
 * set to 0 if the key was not found, to 1 if it was found and deleted. */
unsigned char *zipmapDel(unsigned char *zm, unsigned char *key, unsigned int klen, int *deleted) {
    unsigned int zmlen, freelen;
    unsigned char *p = zipmapLookupRaw(zm,key,klen,&zmlen);
    if (p) {
        freelen = zipmapRawEntryLength(p);
        memmove(p, p+freelen, zmlen-((p-zm)+freelen+1));
        zm = zipmapResize(zm, zmlen-freelen);

        /* Decrease zipmap length */
        if (zm[0] < ZIPMAP_BIGLEN) zm[0]--;

        if (deleted) *deleted = 1;
    } else {
        if (deleted) *deleted = 0;
    }
    return zm;
}

/* Call before iterating through elements via zipmapNext() */
unsigned char *zipmapRewind(unsigned char *zm) {
    return zm+1;
}

/* This function is used to iterate through all the zipmap elements.
 * In the first call the first argument is the pointer to the zipmap + 1.
 * In the next calls what zipmapNext returns is used as first argument.
 * Example:
 *
 * unsigned char *i = zipmapRewind(my_zipmap);
 * while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
 *     printf("%d bytes key at $p\n", klen, key);
 *     printf("%d bytes value at $p\n", vlen, value);
 * }
 */
unsigned char *zipmapNext(unsigned char *zm, unsigned char **key, unsigned int *klen, unsigned char **value, unsigned int *vlen) {
    if (zm[0] == ZIPMAP_END) return NULL;
    if (key) {
        *key = zm;
        *klen = zipmapDecodeLength(zm);
        *key += ZIPMAP_LEN_BYTES(*klen);
    }
    zm += zipmapRawKeyLength(zm);
    if (value) {
        *value = zm+1;
        *vlen = zipmapDecodeLength(zm);
        *value += ZIPMAP_LEN_BYTES(*vlen);
    }
    zm += zipmapRawValueLength(zm);
    return zm;
}

/* Search a key and retrieve the pointer and len of the associated value.
 * If the key is found the function returns 1, otherwise 0. */
int zipmapGet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char **value, unsigned int *vlen) {
    unsigned char *p;

    if ((p = zipmapLookupRaw(zm,key,klen,NULL)) == NULL) return 0;
    p += zipmapRawKeyLength(p);
    *vlen = zipmapDecodeLength(p);
    *value = p + ZIPMAP_LEN_BYTES(*vlen) + 1;
    return 1;
}

/* Return 1 if the key exists, otherwise 0 is returned. */
int zipmapExists(unsigned char *zm, unsigned char *key, unsigned int klen) {
    return zipmapLookupRaw(zm,key,klen,NULL) != NULL;
}

/* Return the number of entries inside a zipmap */
unsigned int zipmapLen(unsigned char *zm) {
    unsigned int len = 0;
    if (zm[0] < ZIPMAP_BIGLEN) {
        len = zm[0];
    } else {
        unsigned char *p = zipmapRewind(zm);
        while((p = zipmapNext(p,NULL,NULL,NULL,NULL)) != NULL) len++;

        /* Re-store length if small enough */
        if (len < ZIPMAP_BIGLEN) zm[0] = len;
    }
    return len;
}

/* Return the raw size in bytes of a zipmap, so that we can serialize
 * the zipmap on disk (or everywhere is needed) just writing the returned
 * amount of bytes of the C array starting at the zipmap pointer. */
size_t zipmapBlobLen(unsigned char *zm) {
    unsigned int totlen;
    zipmapLookupRaw(zm,NULL,0,&totlen);
    return totlen;
}

/* Validate the integrity of the data structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we scan all the entries one by one. */
int zipmapValidateIntegrity(unsigned char *zm, size_t size, int deep) {
#define OUT_OF_RANGE(p) ( \
        (p) < zm + 2 || \
        (p) > zm + size - 1)
    unsigned int l, s, e;

    /* check that we can actually read the header (or ZIPMAP_END). */
    if (size < 2)
        return 0;

    /* the last byte must be the terminator. */
    if (zm[size-1] != ZIPMAP_END)
        return 0;

    if (!deep)
        return 1;

    unsigned int count = 0;
    unsigned char *p = zm + 1; /* skip the count */
    while(*p != ZIPMAP_END) {
        /* read the field name length encoding type */
        s = zipmapGetEncodedLengthSize(p);
        /* make sure the entry length doesn't rech outside the edge of the zipmap */
        if (OUT_OF_RANGE(p+s))
            return 0;

        /* read the field name length */
        l = zipmapDecodeLength(p);
        p += s; /* skip the encoded field size */
        p += l; /* skip the field */

        /* make sure the entry doesn't rech outside the edge of the zipmap */
        if (OUT_OF_RANGE(p))
            return 0;

        /* read the value length encoding type */
        s = zipmapGetEncodedLengthSize(p);
        /* make sure the entry length doesn't rech outside the edge of the zipmap */
        if (OUT_OF_RANGE(p+s))
            return 0;

        /* read the value length */
        l = zipmapDecodeLength(p);
        p += s; /* skip the encoded value size*/
        e = *p++; /* skip the encoded free space (always encoded in one byte) */
        p += l+e; /* skip the value and free space */
        count++;

        /* make sure the entry doesn't rech outside the edge of the zipmap */
        if (OUT_OF_RANGE(p))
            return 0;
    }

    /* check that the zipmap is not empty. */
    if (count == 0) return 0;

    /* check that the count in the header is correct */
    if (zm[0] != ZIPMAP_BIGLEN && zm[0] != count)
        return 0;

    return 1;
#undef OUT_OF_RANGE
}

#ifdef REDIS_TEST
static void zipmapRepr(unsigned char *p) {
    unsigned int l;

    printf("{status %u}",*p++);
    while(1) {
        if (p[0] == ZIPMAP_END) {
            printf("{end}");
            break;
        } else {
            unsigned char e;

            l = zipmapDecodeLength(p);
            printf("{key %u}",l);
            p += zipmapEncodeLength(NULL,l);
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l;

            l = zipmapDecodeLength(p);
            printf("{value %u}",l);
            p += zipmapEncodeLength(NULL,l);
            e = *p++;
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l+e;
            if (e) {
                printf("[");
                while(e--) printf(".");
                printf("]");
            }
        }
    }
    printf("\n");
}

#define UNUSED(x) (void)(x)
int zipmapTest(int argc, char *argv[], int accurate) {
    unsigned char *zm;

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    zm = zipmapNew();

    zm = zipmapSet(zm,(unsigned char*) "name",4, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "surname",7, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "age",3, (unsigned char*) "foo",3,NULL);
    zipmapRepr(zm);

    zm = zipmapSet(zm,(unsigned char*) "hello",5, (unsigned char*) "world!",6,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "bar",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "!",1,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "12345",5,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "new",3, (unsigned char*) "xx",2,NULL);
    zm = zipmapSet(zm,(unsigned char*) "noval",5, (unsigned char*) "",0,NULL);
    zipmapRepr(zm);
    zm = zipmapDel(zm,(unsigned char*) "new",3,NULL);
    zipmapRepr(zm);

    printf("\nLook up large key:\n");
    {
        unsigned char buf[512];
        unsigned char *value;
        unsigned int vlen, i;
        for (i = 0; i < 512; i++) buf[i] = 'a';

        zm = zipmapSet(zm,buf,512,(unsigned char*) "long",4,NULL);
        if (zipmapGet(zm,buf,512,&value,&vlen)) {
            printf("  <long key> is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }

    printf("\nPerform a direct lookup:\n");
    {
        unsigned char *value;
        unsigned int vlen;

        if (zipmapGet(zm,(unsigned char*) "foo",3,&value,&vlen)) {
            printf("  foo is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }
    printf("\nIterate through elements:\n");
    {
        unsigned char *i = zipmapRewind(zm);
        unsigned char *key, *value;
        unsigned int klen, vlen;

        while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
            printf("  %d:%.*s => %d:%.*s\n", klen, klen, key, vlen, vlen, value);
        }
    }
    zfree(zm);
    return 0;
}
#endif
