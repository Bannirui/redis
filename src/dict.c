/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
static int dict_can_resize = 1;
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *ht); // 字典是否需要扩容
static unsigned long _dictNextPower(unsigned long size);
static long _dictKeyIndex(dict *ht, const void *key, uint64_t hash, dictEntry **existing);
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);

/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, int len) {
    return siphash(key,len,dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}

/* ----------------------------- API implementation ------------------------- */

/* Reset a hash table already initialized with ht_init().
 * NOTE: This function should only be called by ht_destroy(). */
// 重置hash表
static void _dictReset(dictht *ht)
{
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

/* Create a new hash table */
/**
 * dict实例化
 * @param type        用来赋值dict实例的type成员 多态函数接口
 * @param privDataPtr 用来赋值privdata成员
 *                    <ul>privdata在源码中的作用应该仅仅是设计预留了功能扩展点
 *                      <li>在keyCompare函数接口中声明了形参 但是函数实现中没有使用</li>
 *                    </ul>
 */
dict *dictCreate(dictType *type,
        void *privDataPtr)
{
    // 分配字典内存 算上填充字节 总共96 bytes
    dict *d = zmalloc(sizeof(*d));
    // 初始化dict
    _dictInit(d,type,privDataPtr);
    return d;
}

/**
 * dict实例成员初始化
 */
int _dictInit(dict *d, dictType *type,
        void *privDataPtr)
{
    /**
     * UDT成员初始化
     * <ul>
     *   <li>初始化2张空的hash表</li>
     *   <li>多态函数接口</li>
     * </ul>
     */
    _dictReset(&d->ht[0]);
    _dictReset(&d->ht[1]);
    d->type = type;
    d->privdata = privDataPtr;
	/**
	 * rehash状态初始化 -1标识没进行rehash
	 */
    d->rehashidx = -1;

	/**
	 * rehash暂停状态初始化 0标识初始状态
	 */
    d->pauserehash = 0;
    return DICT_OK;
}

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1 */
int dictResize(dict *d)
{
    unsigned long minimal;

    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;
    minimal = d->ht[0].used; // hash表中节点数量
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
    // 扩容
    return dictExpand(d, minimal);
}

/* Expand or create the hash table,
 * when malloc_failed is non-NULL, it'll avoid panic if malloc fails (in which case it'll be set to 1).
 * Returns DICT_OK if expand was performed, and DICT_ERR if skipped. */
// dict实例的hash表扩容
// @param d 要扩容的dict实例
// @param size 要扩容到多大(多少个byte)
// @param malloc_failed 内存分配失败标识符 如果调用方传递了该指针 扩容实现内部申请内存失败了 就标识为1通知调用方
//                      目的在于函数的返回值标识的是统一的失败\成功 没有细分失败的详情 这样可以知道如果扩容失败了 是否是因为内存开辟导致的
// @return 操作码 0-成功
//               1-失败
/**
 *
 * @param size          hash表数组期待扩容到多长
 * @param malloc_failed zmalloc的OOM异常标识
 * @return              <ul>请求操作状态
 *                        <li>0 标识扩容成功</li>
 *                        <li>非0 标识扩容失败</li>
 *                      </ul>
 */
int _dictExpand(dict *d, unsigned long size, int* malloc_failed)
{
    /**
     * 取决于调用方是否要自行处理zmalloc的OOM
     * 初始化内存 防止污染
     */
    if (malloc_failed) *malloc_failed = 0;

    /* the size is invalid if it is smaller than the number of
     * elements already inside the hash table */
	/**
	 * 扩容前置校验
	 * <ul>
	 *   <li>首先dict要是已经在rehash中了 上一次的hash桶还没迁移完 就不能再出发扩容</li>
	 *   <li>其次 要检验期待扩容的大小是否合理 扩容是因为当前数组的容量使用达到了某一阈值 也就是扩容时机问题 dict中hash表的数据结构是数组+链表 极致情况下 如果用空间换时间 没有出现hash冲突的时候 hash表退化成数组 那么数组的最小长度就是键值对数量
	 *       那么一次扩容被触发 最优的扩容后长度=min(比size的最小的2的幂次方, >=键值对数量)
	 *   </li>
	 * </ul>
	 * // TODO: 2023/12/8 扩容时机是什么 还没看到
	 */
    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

	// 扩容之后的新表
    dictht n; /* the new hash table */
    // 保证扩容后数组长度是2的幂次方 这样就可以通过位运算计算key的hash桶脚标=hash&(len-1)
    unsigned long realsize = _dictNextPower(size);

    /* Detect overflows */
	/**
	 * 经典的溢出检查
	 * <ul>
	 *   <li>其一要校验的是上面函数2倍计算出来的结果有没有溢出 也就是realsize<size的作用</li>
	 *   <li>其二是防御性的校验 因为平台上实际申请内存malloc系列的参数类型是sizet 本质也是unsigned long类型
	 *       此时已经数组长度realsize 数组元素大小是sizeof(dictEntry*) 那么要给这个数组分配的空间(byte)就是二者乘积
	 *       这个内存容量的表达是否溢出前置到这个地方校验一下
	 *   </li>
	 * </ul>
	 */
    if (realsize < size || realsize * sizeof(dictEntry*) < realsize)
        return DICT_ERR;

    /* Rehashing to the same table size is not useful. */
	// 依然是校验
    if (realsize == d->ht[0].size) return DICT_ERR;

    /* Allocate the new hash table and initialize all pointers to NULL */
    // 初始化hash表成员
    n.size = realsize;
    n.sizemask = realsize-1;
	// 根据实参设定 决定处理OOM的时机
    if (malloc_failed) {
	    // 太try了
        n.table = ztrycalloc(realsize*sizeof(dictEntry*));
        *malloc_failed = n.table == NULL;
        if (*malloc_failed)
            return DICT_ERR;
    } else
        n.table = zcalloc(realsize*sizeof(dictEntry*));

    n.used = 0;

    /* Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys. */
	/**
	 * 扩容的函数暴露给了外面 那么触发扩容的时机就起码有2处
	 * <ul>
	 *   <li></li>
	 *   <li></li>
	 * </ul>
	 */
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return DICT_OK;
    }

    /* Prepare a second hash table for incremental rehashing */
    // 不是hash表的初始化场景 而是扩容->节点迁移的场景
    d->ht[1] = n;
    d->rehashidx = 0; // 标识准备从老表的0号桶开始迁移
    return DICT_OK;
}

/* return DICT_ERR if expand was not performed */
// hash表扩容
// @param d 要扩容的dict实例
// @param size 扩容到多大
// @return 操作标识符
/**
 * hash表扩容 本质就是数组长度的增加
 * zmalloc涉及OOM异常交由zmalloc机制处理
 * @param size 期待将数组扩容到多长
 *             期待是一回事 实际扩容结果是另一回事 源码为了将来使用位元算计算key落在的数组的hash桶脚标 需要确保数组长度是2的幂次方
 *             因此希望数组长度是x 实际会计算得出y(y是2的幂次方 y>=2的最小值)
 * @return
 */
int dictExpand(dict *d, unsigned long size) {
    return _dictExpand(d, size, NULL);
}

/* return DICT_ERR if expand failed due to memory allocation failure */
/**
 * hash表扩容
 * 跟dictExpand的实现几乎一样 唯一区别就是zmalloc涉及OOM异常上抛过来交由这个函数自己处理
 */
int dictTryExpand(dict *d, unsigned long size) {
    // 用于接收zmalloc的OOM异常标识
    int malloc_failed;
    _dictExpand(d, size, &malloc_failed);
    return malloc_failed? DICT_ERR : DICT_OK;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */
// 渐进式rehash实现
// @param n 迁移n个hash桶
// @return 1 标识hash表还有节点待迁移
//         0 标识hash表已经迁移完成
int dictRehash(dict *d, int n) {
    // 最多遍历空桶的数量
    int empty_visits = n*10; /* Max number of empty buckets to visit. */
    if (!dictIsRehashing(d)) return 0;

    // 计划迁移n个桶 现在目标还没完成 hash表中还有节点等待被迁移
    while(n-- && d->ht[0].used != 0) {
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        assert(d->ht[0].size > (unsigned long)d->rehashidx);
        while(d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        // 该hash桶上的节点要迁移走
        de = d->ht[0].table[d->rehashidx];
        /* Move all the keys in this bucket from the old to the new hash HT */
        // de指向hash桶的链表头 遍历链表将节点逐个迁移到新表上
        while(de) {
            uint64_t h;

            nextde = de->next;
            /* Get the index in the new hash table */
            // key在新表的hash桶位置
            h = dictHashKey(d, de->key) & d->ht[1].sizemask; // hash表长度是2的幂次方 通过位运算计算key的hash桶脚标
            // 单链表头插法
            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;
            d->ht[0].used--; // 旧表上每迁移走一个键值对 就更新计数
            d->ht[1].used++; // 新表上每迁过来一个键值对 就更新计数
            de = nextde;
        }
        d->ht[0].table[d->rehashidx] = NULL; // 一个桶上单链表所有节点都迁移完了
        d->rehashidx++; // 一个桶迁移结束 后移待迁移的桶脚标
    }

    /* Check if we already rehashed the whole table... */
    // rehash任务完成后判定一下hash表是否都迁移完成了
    // 迁移完成了就回收老表 把表1指向新表
    if (d->ht[0].used == 0) {
        zfree(d->ht[0].table);
        d->ht[0] = d->ht[1];
        _dictReset(&d->ht[1]);
        d->rehashidx = -1;
        return 0;
    }

    /* More to rehash... */
    return 1;
}

long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

/* Rehash in ms+"delta" milliseconds. The value of "delta" is larger 
 * than 0, and is smaller than 1 in most cases. The exact upper bound 
 * depends on the running time of dictRehash(d,100).*/
int dictRehashMilliseconds(dict *d, int ms) {
    if (d->pauserehash > 0) return 0;

    long long start = timeInMilliseconds();
    int rehashes = 0;

    while(dictRehash(d,100)) {
        rehashes += 100;
        if (timeInMilliseconds()-start > ms) break;
    }
    return rehashes;
}

/* This function performs just a step of rehashing, and only if hashing has
 * not been paused for our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
// 协助迁移1个hash桶
static void _dictRehashStep(dict *d) {
    // 没有迁移暂停
    // 在迭代器安全模式下会暂停rehash
    if (d->pauserehash == 0) dictRehash(d,1);
}

/* Add an element to the target hash table */
// 添加kv键值对
// @param 向哪个dict实例中添加键值对
// @param [key, val]键值对 hash节点
// @return 操作码 0-标识操作成功 1-标识操作失败
int dictAdd(dict *d, void *key, void *val)
{
    // 向字典的hash表添加了一个节点[key, null]
    // 如果已经存在了key就向上返回添加失败
    // 并不关注已经存在的key是谁
    dictEntry *entry = dictAddRaw(d,key,NULL);

    if (!entry) return DICT_ERR; // hash表中已经存在键值对 返回添加失败的标识
    // 上面步骤向字典中添加的节点还没设置value值
    dictSetVal(d, entry, val);
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as they wish.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
// 向字典d中添加一个节点
// hash表中已经存在key了就返回null标识新建节点失败
// 指定该节点的key value留着调用方设置
// @param existing
// @return null标识key已经存在key 不进行节点添加操作 并通过existing指针标识出已经存在的kv键值对节点
//         非null标识新添加到hash表中的[key, null]半成品节点 所谓的半成品指的是entry节点只有key字段 没有value字段
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    long index;
    dictEntry *entry;
    dictht *ht;
    // 字典当前正在rehash 当前线程协助迁移一个桶
    if (dictIsRehashing(d)) _dictRehashStep(d);

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    // 字典d的hash表中存在key
    if ((index = _dictKeyIndex(d, key, dictHashKey(d,key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
    // 字典正在rehash就用新表 没在rehash就用旧表
    // 正在rehash中 也就意味着最终需要将旧标上所有hash桶里面的entry节点都迁移到新表上
    // 就不要往旧表上写数据了 直接一步到位写到新表上
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
    // 分配一个hash节点内存 大小为48 bytes
    entry = zmalloc(sizeof(*entry));
    // 链表 头插法
    entry->next = ht->table[index];
    ht->table[index] = entry;
    ht->used++; // hash表entry键值对节点计数

    /* Set the hash entry fields. */
    // 设置节点的key
    dictSetKey(d, entry, key);
    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
// @param d 向字典d中新增[key, val]hash节点
// @return 1标识新增 0标识更新
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    // 向hash表中添加节点
    entry = dictAddRaw(d,key,&existing);
    // entry为null说明hash表中已经存在键key 存在的节点是existing 要要进行节点value的更新
    // entry不为null说明hash表中原本不存在key
    if (entry) { // 新增
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    auxentry = *existing;
    // hash表中已经存在节点existing 进行节点value的更新
    dictSetVal(d, existing, val);
    dictFreeVal(d, &auxentry);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
// 根据key是否存在情况 对key进行新增或者查询 新增的场景是[key, val]半成品 value字段还没设置留着给调用方进行设置value
// @param d dict实例
// @param key key
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
    // dict中不存在key 新增一个entry节点 entry指向的是半成品 value留给调用方关注
    // dict中已经存在了key entry为null existing指向的是已经存在的entry节点
    entry = dictAddRaw(d,key,&existing);
    return entry ? entry : existing;
}

/* Search and remove an element. This is an helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */
// 删除hash表中节点
// @param nofree 0 标识回收内存
//               1 标识不回收内存
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;

    // 空表 hash表还没初始化
    if (d->ht[0].used == 0 && d->ht[1].used == 0) return NULL;

    if (dictIsRehashing(d)) _dictRehashStep(d); // hash表正在迁移 该线程就协助进行节点迁移
    h = dictHashKey(d, key); // key的hash值

    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask; // hash%len 桶脚标
        he = d->ht[table].table[idx]; // 单链表 链表头
        prevHe = NULL;
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) { // 找到了key
                /* Unlink the element from the list */
                if (prevHe)
                    prevHe->next = he->next; // 当前要删除的链表节点不是链表头
                else
                    d->ht[table].table[idx] = he->next; // 当前要删除的链表节点是链表头
                if (!nofree) { // 根据入参决定内存回收策略
                    dictFreeKey(d, he);
                    dictFreeVal(d, he);
                    zfree(he);
                }
                d->ht[table].used--; // 键值对少了一个
                return he;
            }
            prevHe = he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) break;
    }
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. */
// 删除hash表中key的键值对 回收内存
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 */
// 删除hash表中key键值对 不回收内存
dictEntry *dictUnlink(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,1);
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    zfree(he);
}

/* Destroy an entire dictionary */
// 删除整个hash表
int _dictClear(dict *d, dictht *ht, void(callback)(void *)) {
    unsigned long i;

    /* Free all the elements */
    // hash表所有槽位都要删除直到节点被删除光
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        dictEntry *he, *nextHe;

        if (callback && (i & 65535) == 0) callback(d->privdata);

        if ((he = ht->table[i]) == NULL) continue;
        while(he) {
            nextHe = he->next;
            dictFreeKey(d, he);
            dictFreeVal(d, he);
            zfree(he);
            ht->used--;
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    zfree(ht->table);
    /* Re-initialize the table */
    _dictReset(ht);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
void dictRelease(dict *d)
{
    _dictClear(d,&d->ht[0],NULL);
    _dictClear(d,&d->ht[1],NULL);
    zfree(d);
}

// 根据key查询键值对entry节点
// @param d dict实例
// @param key key
// @return 字典d中key对应的entry节点
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    uint64_t h, idx, table;

    // 字典为空
    if (dictSize(d) == 0) return NULL; /* dict is empty */
    // 字典正在rehash状态 协助迁移1个hash桶
    if (dictIsRehashing(d)) _dictRehashStep(d);
    // key的hash值
    h = dictHashKey(d, key);
    // 轮询字典里面的2张hash表
    // 字典处于rehash的场景下才需要查询两张hash表
    // 字典没有处于rehash的时候 只需要查询一张hash表
    for (table = 0; table <= 1; table++) {
        // 数组长度len=2的幂次方前提下 hash%len == hash&(len-1)
        idx = h & d->ht[table].sizemask; // 数组脚标
        he = d->ht[table].table[idx]; // 链表头
        // 单链表遍历直到找到key对应的节点
        while(he) {
            // 优先比较内存地址 其次使用自定义的比较函数
            if (key==he->key || dictCompareKeys(d, key, he->key))
                return he;
            he = he->next;
        }
        // 没有rehashing 说明数据只可能在旧表上 没有必要继续查新表
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

// 获取key对应的value
void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;
    // 查询key的hash节点
    he = dictFind(d,key);
    return he ? dictGetVal(he) : NULL;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
long long dictFingerprint(dict *d) {
    long long integers[6], hash = 0;
    int j;

    integers[0] = (long) d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (long) d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

// 获取字典的非安全模式迭代器
// @param d dict实例
// @return 迭代器实例
dictIterator *dictGetIterator(dict *d)
{
    // 分配迭代器内存
    dictIterator *iter = zmalloc(sizeof(*iter));
    // 迭代器初始化
    iter->d = d;
    iter->table = 0; // 不管是否在rehash中 从旧表开始准没错的
    iter->index = -1; // 刚初始化出来的迭代器 -1标识还没有遍历过hash桶 也就是说下一次迭代从0号桶开始
    iter->safe = 0; // 迭代过程中不强制要求rehash暂停
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

// 获取字典的安全模式迭代器
// @param d dict实例
// @return 迭代器实例
dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);
    // 安全模式的迭代器 在迭代过程中暂停rehash
    i->safe = 1;
    return i;
}

// 通过迭代器遍历节点
// @param iter 迭代器
// @return entry键值对
dictEntry *dictNext(dictIterator *iter)
{
    while (1) {
        // 什么时候会entry位null
        // 初始化完迭代器后首次调用这个方法的时候
        // 某个hash桶是空的
        // 某个hash桶的链表遍历完之后
        if (iter->entry == NULL) {
            dictht *ht = &iter->d->ht[iter->table]; // hash表
            if (iter->index == -1 && iter->table == 0) { // 初始化刚进来
                if (iter->safe)
                    dictPauseRehashing(iter->d); // 安全迭代模式 暂停rehash
                else
                    iter->fingerprint = dictFingerprint(iter->d); // 非安全模式迭代 给字典内存地址来个签名
            }
            iter->index++; // 推进hash表桶脚标
            if (iter->index >= (long) ht->size) { // 整张hash表的所有hash桶都遍历完了
                if (dictIsRehashing(iter->d) && iter->table == 0) { // 字典在做rehash 也就说还有数据在新表上 继续遍历第二张表
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];
                } else {
                    break; // 字典没有在rehash 所有数据都在第一张hash表上 遍历完了整个hash表 迭代过程也就结束了
                }
            }
            iter->entry = ht->table[iter->index];
        } else {
            iter->entry = iter->nextEntry;
        }
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}

// 回收迭代器
void dictReleaseIterator(dictIterator *iter)
{
    if (!(iter->index == -1 && iter->table == 0)) { // 说明迭代器已经处理工作中 开始了遍历
        if (iter->safe)
            dictResumeRehashing(iter->d); // 删除该线程的暂停rehash
        else
            assert(iter->fingerprint == dictFingerprint(iter->d)); // 非安全迭代模式下 比较迭代器状态签名 不一致说明在迭代过程中用户了尽心改了非法操作
    }
    zfree(iter); // 回收迭代器内存
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
// 随机从字典中获取一个节点
// 随机获取一个hash槽 在链表上随机获取一个节点
dictEntry *dictGetRandomKey(dict *d)
{
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL; // 空表
    if (dictIsRehashing(d)) _dictRehashStep(d); // 协助迁移一个hash桶
    if (dictIsRehashing(d)) { // 字典在rehash 要关注两个表
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            // 字典正在rehash
            // 对于老表而言 [0...rehashidx-1]已经迁移完成了 这部分桶在老表上都是空的了 不必要考察了
            // size是节点数量
            // [0...rehashidx-1] rehashidx [rehashidx+1...size-1]
            // 随机出来一个桶脚标 再看看是在哪张表上
            // 桶里面有节点就找到了链表头
            h = d->rehashidx + (randomULong() % (dictSlots(d) - d->rehashidx));
            he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] :
                                      d->ht[0].table[h];
        } while(he == NULL);
    } else { // 字典没有在rehash 只关注第一张表就行
        do {
            h = randomULong() & d->ht[0].sizemask; // 随机一个桶索引
            he = d->ht[0].table[h]; // 桶里面有节点就找到了链表头
        } while(he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    listlen = 0; // 遍历单链表计算出链表长度
    orighe = he;
    while(he) {
        he = he->next;
        listlen++;
    }
    listele = random() % listlen;
    he = orighe;
    while(listele--) he = he->next; // 在链表上随机找一个节点
    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
// 随机采集指定数量的节点
// 可能返回的数量达不到指定要求的个数
// @param des 随机返回的节点 一维数组首地址
// @pram count 随机采集多少个节点
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j; /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps; // 采集次数上限

    // 最多也就将整个dict的所有的节点都遍历出来
    if (dictSize(d) < count) count = dictSize(d);
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) { // 字典在rehash中 尝试协助迁移count个桶
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }
    // dict在rehash就要遍历2张hash表
    tables = dictIsRehashing(d) ? 2 : 1;
    // dict在rehash时 掩码=max{老表掩码, 新表掩码}
    maxsizemask = d->ht[0].sizemask;
    if (tables > 1 && maxsizemask < d->ht[1].sizemask)
        maxsizemask = d->ht[1].sizemask;

    /* Pick a random point inside the larger table. */
    // 随机出一个hash槽 这个桶脚标i区间[0...两个hash表中的最大长度)
    unsigned long i = randomULong() & maxsizemask;
    // 空槽计数
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
    while(stored < count && maxsteps--) {
        // dict在rehash就遍历两张hash表 没在rehash就遍历一张hash表
        for (j = 0; j < tables; j++) {
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            // 跳过那些已经迁移到新表的槽
            // tables==2 -> dict正在rehash
            // j==0 -> 当前在遍历旧表
            // i<=rehashidx -> 桶i已经被迁移了
            if (tables == 2 && j == 0 && i < (unsigned long) d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                // i的区间是[0...max(size)) 怎么会发生i>=size呢
                // 说明发生了缩容场景 新表是要比旧表小的
                // 随机出来的桶脚标已经超出了新表上限
                // 所以桶只能在老表上 但是老表上的这个桶又被迁移了
                // 因此重新赋值为旧表上有效的桶脚标
                if (i >= d->ht[1].size)
                    i = d->rehashidx;
                else
                    // 随机出来的桶脚标i已经被迁移到了新表上 开始考察新表
                    continue;
            }
            // 无效桶脚标
            if (i >= d->ht[j].size) continue; /* Out of range for this table. */
            dictEntry *he = d->ht[j].table[i]; // 桶里面元素

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) { // 空桶
                emptylen++; // 空桶计数
                if (emptylen >= 5 && emptylen > count) {
                    i = randomULong() & maxsizemask;
                    emptylen = 0;
                }
            } else { // 桶里面有单链表
                emptylen = 0;
                while (he) { // 采集链表上的节点
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    *des = he;
                    des++;
                    he = he->next;
                    stored++;
                    if (stored == count) return stored;
                }
            }
        }
        i = (i+1) & maxsizemask; // 下一个hash桶脚标
    }
    return stored;
}

/* This is like dictGetRandomKey() from the POV of the API, but will do more
 * work to ensure a better distribution of the returned element.
 *
 * This function improves the distribution because the dictGetRandomKey()
 * problem is that it selects a random bucket, then it selects a random
 * element from the chain in the bucket. However elements being in different
 * chain lengths will have different probabilities of being reported. With
 * this function instead what we do is to consider a "linear" range of the table
 * that may be constituted of N buckets with chains of different lengths
 * appearing one after the other. Then we report a random element in the range.
 * In this way we smooth away the problem of different chain lengths. */
#define GETFAIR_NUM_ENTRIES 15
dictEntry *dictGetFairRandomKey(dict *d) {
    dictEntry *entries[GETFAIR_NUM_ENTRIES];
    unsigned int count = dictGetSomeKeys(d,entries,GETFAIR_NUM_ENTRIES);
    /* Note that dictGetSomeKeys() may return zero elements in an unlucky
     * run() even if there are actually elements inside the hash table. So
     * when we get zero, we call the true dictGetRandomKey() that will always
     * yield the element if the hash table has at least one. */
    if (count == 0) return dictGetRandomKey(d);
    unsigned int idx = rand() % count;
    return entries[idx];
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
// 二进制形式翻转
static unsigned long rev(unsigned long v) {
    unsigned long s = CHAR_BIT * sizeof(v); // bit size; must be power of 2
    // 64位全是1
    unsigned long mask = ~0UL;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 *
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 *
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 *
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 *
 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 *
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 *
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 *
 * The disadvantages resulting from this design are:
 *
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 */
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       dictScanBucketFunction* bucketfn,
                       void *privdata)
{
    dictht *t0, *t1;
    const dictEntry *de, *next;
    unsigned long m0, m1;

    if (dictSize(d) == 0) return 0;

    /* This is needed in case the scan callback tries to do dictFind or alike. */
    dictPauseRehashing(d);

    if (!dictIsRehashing(d)) {
        t0 = &(d->ht[0]);
        m0 = t0->sizemask;

        /* Emit entries at cursor */
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        v |= ~m0;

        /* Increment the reverse cursor */
        v = rev(v);
        v++;
        v = rev(v);

    } else {
        t0 = &d->ht[0];
        t1 = &d->ht[1];

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (t0->size > t1->size) {
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        m0 = t0->sizemask;
        m1 = t1->sizemask;

        /* Emit entries at cursor */
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            if (bucketfn) bucketfn(privdata, &t1->table[v & m1]);
            de = t1->table[v & m1];
            while (de) {
                next = de->next;
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    dictResumeRehashing(d);

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Because we may need to allocate huge memory chunk at once when dict
 * expands, we will check this allocation is allowed or not if the dict
 * type has expandAllowed member function. */
static int dictTypeExpandAllowed(dict *d) {
    if (d->type->expandAllowed == NULL) return 1;
    return d->type->expandAllowed(
                    _dictNextPower(d->ht[0].used + 1) * sizeof(dictEntry*),
                    (double)d->ht[0].used / d->ht[0].size);
}

/* Expand the hash table if needed */
// 考察是否有扩容的需求
// 需要扩容就进行hash表扩容操作
static int _dictExpandIfNeeded(dict *d)
{
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    // hash表初始化
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio) &&
        dictTypeExpandAllowed(d))
    {
        return dictExpand(d, d->ht[0].used + 1); // 扩容新的数组长度是>节点数的2的幂次方 也就为扩容基准并不是现在数组大小 而是现在节点数量 最坏情况下没有hash冲突保证容纳所有节点
    }
    return DICT_OK;
}

/* Our hash table capability is a power of two */
/**
 *  找到>=size的最小的2的幂次方
 *  将hash表数组长度固化为2的幂次方 方便位元算实现key的脚标计算
 *  最新分支的源码已经更新了计算方式 计算2被就是左移位运算
 *  但凡涉及到运算 就要考虑溢出问题
 *  <ul>尤其是轮询条件的运算
 *    <li>轮询过程中的溢出在函数内部就可以进行检查</li>
 *    <li>结果的溢出检查<ul>
 *      <li>既可以在函数内部return之前检查完再return</li>
 *      <li>也可以在调用方拿到函数结果再检查</li>
 *    </ul></li>
 *  </ul>
 *  正常工程设计上肯定是将结果的检查放在调用方的
 *  <ul>
 *    <li>一方面符合最少知道原则 每个函数只关注在自己的业务领域</li>
 *    <li>其次 一旦将结果的校验放在函数内部 就意味着要继而关注如果发生溢出的处理机制 破坏了设计层次</li>
 *  </ul>
 */
static unsigned long _dictNextPower(unsigned long size)
{
    unsigned long i = DICT_HT_INITIAL_SIZE;
	/**
	 * 为什么加上这么个分支
	 * 我的理解是从数学角度来讲可以去掉
	 * 但是考虑到实际应用场景的话 这个分支是在帮助提升内存使用率 减少不必要的内存空间浪费
	 * 这个if分支的判断准绳是多大 这个肯定是没有标准的 源码采用long的最大值 当前可以换成int的最大值
	 * long型正数表达的大小(2^(64-1)-1 long型64bit 高位0标识正数 低63位均是1)(byte)已经很大了 如果在此之上的空间还继续采取2被扩容方式 可能将会有很大的空间在未来并用不上
	 */
    // TODO: 2023/12/8 如果步进值是1的话怎么保证数组长度是2的幂次方呢 计算key的hash桶脚标
    if (size >= LONG_MAX) return LONG_MAX + 1LU;
	// 在最新的分支源码中乘法已经更新成了位运算
    while(1) {
        if (i >= size)
            return i;
        i *= 2;
    }
}

/* Returns the index of a free slot that can be populated with
 * a hash entry for the given 'key'.
 * If the key already exists, -1 is returned
 * and the optional output parameter may be filled.
 *
 * Note that if we are in the process of rehashing the hash table, the
 * index is always returned in the context of the second (new) hash table. */
// 在字典hash表中key对应的hash表数组脚标 也就是桶位 存在key的时候用existing指针指向节点
// @param d dict实例
// @param key 键
// @param key的hash值
// @param existing 指向hash表键值对节点 key已经存在的时候找到了key就把指针指向键值对节点
// @return key的hash桶脚标 -1标识key已经存在
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing)
{
    unsigned long idx, table;
    dictEntry *he;
    if (existing) *existing = NULL; // 预警式放置指针污染 只有当确定key存在于hash表中时才将该指针指向节点

    /* Expand the hash table if needed */
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;
    // 只有当字典rehash时才会轮询两张hash表 否则只考察老hash表
    for (table = 0; table <= 1; table++) {
        // key所在的数组脚标位置
        idx = hash & d->ht[table].sizemask; // 位运算计算key的hash桶脚标
        /* Search if this slot does not already contain the given key */
        // 遍历单链表找到key
        he = d->ht[table].table[idx];
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                if (existing) *existing = he; // 要找的key已经存在了 返回-1标识key已经存在 并把existing指针指向已经存在的键值对
                return -1;
            }
            he = he->next;
        }
        // 字典没有在rehash 就考察hash表老表就行
        if (!dictIsRehashing(d)) break;
    }
    return idx;
}

void dictEmpty(dict *d, void(callback)(void*)) {
    _dictClear(d,&d->ht[0],callback);
    _dictClear(d,&d->ht[1],callback);
    d->rehashidx = -1;
    d->pauserehash = 0;
}

void dictEnableResize(void) {
    dict_can_resize = 1;
}

void dictDisableResize(void) {
    dict_can_resize = 0;
}

uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* Finds the dictEntry reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dictEntry if found, or NULL if not found. */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash) {
    dictEntry *he, **heref;
    unsigned long idx, table;

    if (dictSize(d) == 0) return NULL; /* dict is empty */
    for (table = 0; table <= 1; table++) {
        idx = hash & d->ht[table].sizemask;
        heref = &d->ht[table].table[idx];
        he = *heref;
        while(he) {
            if (oldptr==he->key)
                return heref;
            heref = &he->next;
            he = *heref;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50
size_t _dictGetStatsHt(char *buf, size_t bufsize, dictht *ht, int tableid) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];
    size_t l = 0;

    if (ht->used == 0) {
        return snprintf(buf,bufsize,
            "No stats available for empty dictionaries\n");
    }

    /* Compute stats. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    for (i = 0; i < ht->size; i++) {
        dictEntry *he;

        if (ht->table[i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = ht->table[i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    l += snprintf(buf+l,bufsize-l,
        "Hash table %d stats (%s):\n"
        " table size: %lu\n"
        " number of elements: %lu\n"
        " different slots: %lu\n"
        " max chain length: %lu\n"
        " avg chain length (counted): %.02f\n"
        " avg chain length (computed): %.02f\n"
        " Chain length distribution:\n",
        tableid, (tableid == 0) ? "main hash table" : "rehashing target",
        ht->size, ht->used, slots, maxchainlen,
        (float)totchainlen/slots, (float)ht->used/slots);

    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) continue;
        if (l >= bufsize) break;
        l += snprintf(buf+l,bufsize-l,
            "   %s%ld: %ld (%.02f%%)\n",
            (i == DICT_STATS_VECTLEN-1)?">= ":"",
            i, clvector[i], ((float)clvector[i]/ht->size)*100);
    }

    /* Unlike snprintf(), return the number of characters actually written. */
    if (bufsize) buf[bufsize-1] = '\0';
    return strlen(buf);
}

void dictGetStats(char *buf, size_t bufsize, dict *d) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf,bufsize,&d->ht[0],0);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0) {
        _dictGetStatsHt(buf,bufsize,&d->ht[1],1);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize-1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef REDIS_TEST

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, strlen((char*)key));
}

int compareCallback(void *privdata, const void *key1, const void *key2) {
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = strlen((char*)key1);
    l2 = strlen((char*)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

    zfree(val);
}

char *stringFromLongLong(long long value) {
    char buf[32];
    int len;
    char *s;

    len = sprintf(buf,"%lld",value);
    s = zmalloc(len+1);
    memcpy(s, buf, len);
    s[len] = '\0';
    return s;
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL,
    NULL
};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg) do { \
    elapsed = timeInMilliseconds()-start; \
    printf(msg ": %ld items in %lld ms\n", count, elapsed); \
} while(0)

/* ./redis-server test dict [<count> | --accurate] */
int dictTest(int argc, char **argv, int accurate) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType,NULL);
    long count = 0;

    if (argc == 4) {
        if (accurate) {
            count = 5000000;
        } else {
            count = strtol(argv[3],NULL,10);
        }
    } else {
        count = 5000;
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        int retval = dictAdd(dict,stringFromLongLong(j),(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMilliseconds(dict,100);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        zfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        dictEntry *de = dictGetRandomKey(dict);
        assert(de != NULL);
    }
    end_benchmark("Accessing random keys");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict,key);
        assert(de == NULL);
        zfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        char *key = stringFromLongLong(j);
        int retval = dictDelete(dict,key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict,key,(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
    dictRelease(dict);
    return 0;
}
#endif
