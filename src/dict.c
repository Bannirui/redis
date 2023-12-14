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
/**
 * 扩容阈值
 * 键值对数量/数组长度>5
 * 从平均角度来讲 也就是说平均每个链表长度为5 超过了5就认为过长 就触发扩容
 * 只要hash算法设计的足够好 就能够尽量保证每个链表都相对较短
 */
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
/**
 * 手动触发hash表扩容
 * hash表每次扩容扩到多大: 数组长度扩到当前键值对数量 并保证长度为2的幂次方
 * 当前键值对数量的界定=min{4, 键值对实际数量}
 */
int dictResize(dict *d)
{
    unsigned long minimal;

    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;
    minimal = d->ht[0].used;
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
    return dictExpand(d, minimal);
}

/* Expand or create the hash table,
 * when malloc_failed is non-NULL, it'll avoid panic if malloc fails (in which case it'll be set to 1).
 * Returns DICT_OK if expand was performed, and DICT_ERR if skipped. */
/**
 * @param size          hash表数组期待扩容到多长
 * @param malloc_failed zmalloc的OOM异常标识
 *                      <ul>
 *                        <li>没有传入指针变量 说明调用方肯定不关注OOM的异常</li>
 *                        <li>传入了指针变量 说明调用方可能关注OOM异常 调用方可能想自行处理OOM</li>
 *                      </ul>
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
	 *   <li>首先dict要是已经在rehash中了 上一次的hash桶还没迁移完 就不能再触发扩容</li>
	 *   <li>其次 要检验期待扩容的大小是否合理 扩容是因为当前数组的容量使用达到了某一阈值 也就是扩容时机问题 dict中hash表的数据结构是数组+链表 极致情况下 如果用空间换时间 没有出现hash冲突的时候 hash表退化成数组 那么数组的最小长度就是键值对数量
	 *       那么一次扩容被触发 最优的扩容后长度=min(比size的最小的2的幂次方, >=键值对数量)
	 *   </li>
	 * </ul>
	 * 扩容时机是发生在dictAdd中考察键值对数量是否过多 键值对数量/数组长度>5
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
	 *   <li>dict实例化之后还没put数据 就手动主动进行expand</li>
	 *   <li>dict在put过程中触发了数组长度使用阈值 被动触发扩容</li>
	 * </ul>
	 * dict刚实例化完 直接把新表给[0]号就行
	 */
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return DICT_OK;
    }

    /* Prepare a second hash table for incremental rehashing */
	/**
	 * 因为hash表数组长度使用触发了扩容阈值
	 * 新表给到[1]号表
	 * 标记rehash标识
	 */
    d->ht[1] = n;
    d->rehashidx = 0;
    return DICT_OK;
}

/* return DICT_ERR if expand was not performed */
/**
 * hash表扩容 本质就是数组长度的增加
 * zmalloc涉及OOM异常交由zmalloc机制处理 当前函数不单独处理OOM
 * @param size 期待将数组扩容到多长
 *             期待是一回事 实际扩容结果是另一回事 源码为了将来使用位元算计算key落在的数组的hash桶脚标 需要确保数组长度是2的幂次方
 *             因此希望数组长度是x 实际会计算得出y(y是2的幂次方 y>=2的最小值)
 * @return <ul>请求操作状态
 *           <li>0 标识扩容成功</li>
 *           <li>非0 标识扩容失败</li>
 *         </ul>
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
/**
 * 渐进式rehash实现
 * @param d
 * @param n 目标帮助迁移n个hash桶
 *          所谓目标是迁移n个hash桶 实际上前的数量<=n的 因为中间涉及到性能保护的设计可以让线程提前退出rehash流程
 * @return  返回值为0或者1
 *          <ul>
 *            <li>0 标识dict的rehash已经完成 现在有且只有1个hash表 不再需要线程进来帮助迁移hash桶了</li>
 *            <li>1 标识dict的hash表还有hash桶要迁移</li>
 *          </ul>
 */
int dictRehash(dict *d, int n) {
	/**
	 * 数据质量的防御
	 * <ul>
	 *   <li>迁移1个桶的时候不存在这个担忧</li>
	 *   <li>迁移n(n>1)个桶的时候 假使n个桶两两之间分布在hash表数组很离散的位置 即数组元素之间脚标间隔很远 就意味着当前线程遍历数组脚标可能数量很大 会耗时 对于当前线程而言 协助迁移hash桶只是分外之事 不是主线任务 所以要平衡迁移hash桶的时间</li>
	 * </ul>
	 * 鉴于这样的考量 就要给hash表数组的轮询设定阈值
	 */
    int empty_visits = n*10; /* Max number of empty buckets to visit. */
	// rehash状态校验
    if (!dictIsRehashing(d)) return 0;

	/**
	 * 当前线程rehash任务的停止边界
	 * <ul>
	 *   <li>迁移目标n个桶</li>
	 *   <li>轮询的hash表脚标数量统计上限</li>
	 *   <li>dict的[0]号表迁移结束</li>
	 * </ul>
	 */
    while(n-- && d->ht[0].used != 0) {
	    /**
	     * <ul>
	     *   <li>de记录hash表数组中存放的单链表</li>
	     *   <li>nextde记录单链表头</li>
	     * </ul>
	     */
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
		/**
		 * 防止数组脚标溢出
		 * rehashidx是标识的要迁移的hash桶 也就是数组脚标指向的数组元素 将来要通过arr[rehashidx]方式来获取数组元素的 防止溢出
		 */
        assert(d->ht[0].size > (unsigned long)d->rehashidx);
		/**
		 * <ul>
		 *   <li>一方面 跳过hash表数组中的空桶</li>
		 *   <li>另一方面 统计总共遍历了多少个桶 考察是不是达到了遍历上限</li>
		 * </ul>
		 */
        while(d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        /**
         * 不是空桶
         * 准备把整条单链表迁移走 轮询单链表所有节点 依次迁移走
         */
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
        d->rehashidx++; // 一个桶迁移结束后 下一次迁移的脚标位置
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
/**
 *
 * @param d
 */
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
/**
 *
 * @param d
 * @param key
 * @param val
 * @return
 */
int dictAdd(dict *d, void *key, void *val)
{
	/**
	 * 添加键值对节点
	 * 可能会失败
	 * <ul>
	 *   <li>dict不允许重复key</li>
	 *   <li>触发扩容 扩容失败</li>
	 * </ul>
	 */
    dictEntry *entry = dictAddRaw(d,key,NULL);

    if (!entry) return DICT_ERR;
	/**
	 * 这个地方就开始体现怎么应对多类型的value赋值问题了
	 * <ul>键值对的value可以是
	 *   <li>指针类型</li>
	 *   <li>无符号整数</li>
	 *   <li>有符号整数</li>
	 *   <li>浮点数</li>
	 * </ul>
	 * 调用这个宏来应付不同的选择
	 * 根据用户的valDup函数来处理value的赋值
	 * <ul>
	 *   <li>给定了valDup 就用这个函数处理指针类型的数据</li>
	 *   <li>其他的数据类型是值类型的</li>
	 * </ul>
	 */
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
/**
 * 这个函数存在的必要性是什么 也就是说为什么要同时提供dictAdd和dictAddRaw这两个API
 * 我的理解还是因为键值对v类型设计union衍生出来的特性支持
 * 因为想要键值对存储的类型丰富->将v的类型设计为union->因为v是union的意味着redis源码无法明确知晓用户到底想要把value设置成什么类型->因此在dictAdd基础指向额外提供这个方法给用户提供自定义设置value类型的入口
 * 调用方在此基础上自己关注value怎么设置
 * @param existing hash表不存在重复key key已经存在了就无法新增 用于存放重复的key
 *                 至于为什么设计成dictEntry**类型的指针变量 因为键值对存储在hash表中的形式是数组+链表 每个链表节点的类型都是dictEntry*
 *                 那么用户交互键值对节点最直接的类型就是dictEntry*
 *                 所以这个地方existing类型相当于对dictEntry*的一个变量再取址一次 就是dictEntry**
 * @return         写入失败就返回NULL
 *                 写入成功就返回新增的键值对节点
 */
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    // key在hash表的脚标位置
    long index;
    dictEntry *entry;
	// dict可能有1个表 可能有2个表 表示数据往哪个表上写
    dictht *ht;
    // 字典当前正在rehash 当前线程协助迁移一个桶
    if (dictIsRehashing(d)) _dictRehashStep(d);

    /* Get the index of the new element, or -1 if
     * the element already exists. */
	/**
	 * index -1表示key在dict中hash表数组脚标计算不出来
	 * <ul>
	 *   <li>key已经存在了 把存在的key返回出去 dict不允许重复key 调用方就去更新value值就完成了add新键值对的语义</li>
	 *   <li>诸如扩容失败导致计算不出来key落在数组的位置</li>
	 * </ul>
	 */
    if ((index = _dictKeyIndex(d, key, dictHashKey(d,key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
	/**
	 * 数据往哪儿写
	 * <ul>
	 *   <li>dict可能只有1个hash表</li>
	 *   <li>可能有2个hash表</li>
	 * </ul>
	 * 取决于当前dict的状态 在rehash中就有2个表
	 * 既然在rehash 就是处在把[0]号表元素往[1]号表迁移的过程中
	 * 所以新增的数据就不要往[0]号表写了 然后再迁移
	 * 直接一步到位 写到[1]号表上去
	 */
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
	// 键值对 实例化 初始化
    entry = zmalloc(sizeof(*entry));
	// 经典的单链表头插法
    entry->next = ht->table[index];
    ht->table[index] = entry;
    // 键值对计数
    ht->used++;

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
/**
 * 新增/更新
 */
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
	/**
	 * dict不重复key
	 * 能新增就新增 有重复了就找到存在的key
	 */
    entry = dictAddRaw(d,key,&existing);
	// 新增key的情况
    if (entry) {
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
	// 重复key的情况 更新value
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
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
	/**
	 * dict不重复key
	 * 能新增就新增 不能新增就找到已经存在的key
	 */
    entry = dictAddRaw(d,key,&existing);
    return entry ? entry : existing;
}

/* Search and remove an element. This is an helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions. */
/**
 * 从dict中移除key
 * @param nofree 标识移除的键值对内存怎么处理
 *               <ul>
 *                 <li>0 释放键值对内存</li>
 *                 <li>1 不需要释放键值对内存</li>
 *               </ul>
 * @return 被移除的键值对
 */
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    /**
     * h key的hash值
     * idx key落在hash表数组的脚标位置
     */
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;

    // 空表的情况
    if (d->ht[0].used == 0 && d->ht[1].used == 0) return NULL;

    // 渐进式rehash的体现 hash表正在迁移 该线程就协助进行节点迁移
    if (dictIsRehashing(d)) _dictRehashStep(d);
    h = dictHashKey(d, key);

    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        prevHe = NULL;
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                /* Unlink the element from the list */
                if (prevHe)
                    prevHe->next = he->next; // 当前要删除的链表节点不是链表头
                else
                    d->ht[table].table[idx] = he->next; // 当前要删除的链表节点是链表头
			    // 根据入参决定内存回收策略
                if (!nofree) {
                    dictFreeKey(d, he);
                    dictFreeVal(d, he);
                    zfree(he);
                }
			    // 更新dict中的键值对计数 键值对少了一个
                d->ht[table].used--;
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
/**
 * 删除key 释放被删除的键值对的内存
 */
int dictDelete(dict *ht, const void *key) {
    // 删除key 释放被删除的键值对的内存
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
/**
 * 删除key 不释放被删除的键值对的内存
 */
dictEntry *dictUnlink(dict *ht, const void *key) {
    // 从dict中移除key 不释放被移除的键值对的内存
    return dictGenericDelete(ht,key,1);
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
/**
 * 跟dictUnlink配套使用的 将dict中移除的key进行内存释放回收
 * 为啥要提供这样的API 直接提供一个dictDelete就行了
 */
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    dictFreeKey(d, he);
    dictFreeVal(d, he);
    zfree(he);
}

/* Destroy an entire dictionary */
/**
 * 释放dict
 * 轮询dict中所有键值对节点逐个释放内存
 * 继而释放dict的hash表引用
 * 最后释放dict内存
 * @param callback
 */
int _dictClear(dict *d, dictht *ht, void(callback)(void *)) {
    unsigned long i;

    /* Free all the elements */
	// 轮询hash表中所有hash桶中的单链表 逐个释放内存
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        dictEntry *he, *nextHe;

		/**
		 * 0xFFFF
		 * 什么意思 为什么要回调
		 * <ul>
		 *   <li>0号脚标触发回调</li>
		 *   <li>大于等于65536号脚标触发回调</li>
		 * </ul>
		 * 什么意思0号触发可以理解成销毁节点的开始
		 * 如果数组长度<65536 只会被触发一次
		 * 如果数组长度>=65536 那么除了[1...65535]脚标 后面将会一直触发回调
		 */
		 // TODO: 2023/12/13 回调的意义是什么
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

/**
 * 查找key
 */
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    uint64_t h, idx, table;

    // 字典为空 没有键值对
    if (dictSize(d) == 0) return NULL; /* dict is empty */
    // 字典正在rehash状态 协助迁移1个hash桶
    if (dictIsRehashing(d)) _dictRehashStep(d);
    // key的hash值
    h = dictHashKey(d, key);
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        while(he) {
		    // 找到了key
            if (key==he->key || dictCompareKeys(d, key, he->key))
                return he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

/**
 * 键值对的value
 * 这个函数仅仅针对的值类型是指针类型的
 * 值类型很丰富 要去获取的话也要读取union中不同的成员
 * <ul>
 *   <li>void*指针类型</li>
 *   <li>u64整数</li>
 *   <li>s64整数</li>
 *   <li>double浮点数</li>
 * </ul>
 */
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

/**
 * 实例化不安全模式的迭代器
 */
dictIterator *dictGetIterator(dict *d)
{
    // 分配迭代器内存
    dictIterator *iter = zmalloc(sizeof(*iter));
    // 迭代器初始化
    iter->d = d;
    // 不管是否在rehash中 从旧表开始准没错的
    iter->table = 0;

	/**
	 * 刚初始化出来的迭代器 -1标识还没有遍历过hash桶
	 * 也就是说下一次迭代从0号桶开始
	 */
    iter->index = -1;

	/**
	 * 不要求安全迭代模式
	 * 也即不强制要求dict暂停rehash
	 */
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

/**
 * 实例化安全模式的迭代器
 */
dictIterator *dictGetSafeIterator(dict *d) {
    // 不安全模式的迭代器
    dictIterator *i = dictGetIterator(d);
	/**
	 * 用这个成员来标识安全模式
	 * 怎么使用的呢 将来在next的过程中考察safe这个成员->控制pauserehash自增
	 * 那么当前前台请求进来过后发现dict需要rehash 但是发现pauserehash要暂停 就停止了协助迁移hash桶
	 */
    i->safe = 1;
    return i;
}

/**
 * 使用迭代器访问数据
 * 至于是否是安全模式就看迭代器的safe成员了 由它来决定
 */
dictEntry *dictNext(dictIterator *iter)
{
    while (1) {
		/**
		 * 什么时候会entry位null
		 * <ul>
		 *   <li>初始化完迭代器后首次调用这个方法的时候</li>
		 *   <li>某个hash桶是空的</li>
		 *   <li>某个hash桶的链表遍历完之后</li>
		 * </ul>
		 * 因为键值对节点仅仅的存在形式是单链表节点
		 * 因此总言之可以这么理解这个边界条件: hash表上的某个hash桶的单链表访问完了
		 * 要驱动着后移数组脚标了 既然数组脚标要动 就要考虑
		 * <ul>
		 *   <li>数组脚标溢出</li>
		 *   <li>数组访问结束了 是不是还有另一个数组要继续访问</li>
		 * </ul>
		 * 那么也就是单链表访问结束->倒过来去考察数据脚标->继续考察整个数组->考察dict有没有其他hash表了
		 */
        if (iter->entry == NULL) {
		    // hash桶访问结束了 准备访问数组的下一个hash桶 得先定位到hash表的数组
            dictht *ht = &iter->d->ht[iter->table];
			/**
			 * 每个人的编码不尽相同
			 * 为什么在这加个前置条件 不加行不行
			 * <ul>
			 *   <li>首先不加这个if肯定是可以的 直接把数组脚标后移继续迭代 但是这样做的话就要把某些初始化工作前置到初始化时候了</li>
			 *   <li>这样做起始本质就是懒加载 在实例化/初始化的时候只关注资源以及必要的数据状态 假使迭代器是要实例化出来一个安全模式的 那标记完必要的状态成员后 暂停rehash这个完全没有必要做 因为你不知道初始化出来的这个玩意将来实际被不被真正使用 如果并不被使用 那么这次浪费的资源就是白白浪费</li>
			 * </ul>
			 * 我自己也是推荐懒加载的方式进行编码 有些资源后置延迟到使用的时机处理
			 */
            if (iter->index == -1 && iter->table == 0) {
				/**
				 * 延迟处理访问模式
				 * <ul>
				 *   <li>安全迭代模式 就打上暂停rehash的标志 给将来想要参与rehash的线程看 它看到了自然就放弃rehash工作了</li>
				 *   <li>不是安全模式 给dict打个签名 比如一个访问周期结束了 将来比较一下此时的和彼时的两个值 签名不一样肯定在访问周期内动过了数据</li>
				 * </ul>
				 */
                if (iter->safe)
                    dictPauseRehashing(iter->d);
                else
                    iter->fingerprint = dictFingerprint(iter->d);
            }
			// hash桶里面链表访问结束了 就后移数组指针找下一个hash桶 移动了数组脚标 就要溢出检查
            iter->index++;
            if (iter->index >= (long) ht->size) {
			    // 数组访问溢出了 说明hash表读完了 就要考察dict是不是还有hash表了
                if (dictIsRehashing(iter->d) && iter->table == 0) {
				    // dict还有hash表可读
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];
                } else {
				    // 整个dict都读完了 直接跳出了最外层的while死循环
                    break;
                }
            }
			// 找到了hash表数组上的新的hash桶
            iter->entry = ht->table[iter->index];
        } else {
		    // hash桶里面的链表还没到底 那就顺着链表找后继节点
            iter->entry = iter->nextEntry;
        }
		// 当前访问的键值对entry 保存现场 记录好后继节点 才能在外层轮询下去 因为实际使用肯定是while((x=dictNext(d))!=NULL){}
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    return NULL;
}

void dictReleaseIterator(dictIterator *iter)
{
    /**
     * 不太理解的地方在于 为什么要费劲恢复一些状态
     * 既然我的目的是释放内存 回收资源 为什么还要做这些不想干的事情 反正我都是要回收资源的
     * 内存一旦释放了 别人也不能再拿着这个迭代器实例继续操作的 也就不用担心因为数据污染了
     * <ul>
     *   <li>没在使用的迭代器 简单地释放内存资源即可</li>
     *   <li>在使用中的迭代器 就要先进行一些状态复位工作 再释放回收内存
     *     <ul>
     *       <li>安全模式下 safe这个成员是1 给它自减到0 标识迭代器处于正常状态</li>
     *       <li>非安全模式下 比较一下签名 看看在迭代过程期间 用户没有没有操作过hash表</li>
     *     </ul>
     *   </li>
     * </ul>
     */
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe)
            dictResumeRehashing(iter->d);
        else
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
/**
 * 随机获取一个key的键值对
 */
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
	// 用户层判定是否可以扩容
    return d->type->expandAllowed(
                    _dictNextPower(d->ht[0].used + 1) * sizeof(dictEntry*),
                    (double)d->ht[0].used / d->ht[0].size);
}

/* Expand the hash table if needed */
/**
 * 考察dict是否需要扩容 如果有扩容需求就触发扩容
 * @return <ul>
 *           <li>0标识需要扩容并且扩容成功</li>
 *           <li>1标识不需要扩容或者扩容失败
 *             <ul>
 *               <li>可能因为不需要扩容</li>
 *               <li>可能是需要扩容但是扩容失败了(申请新内存OOM)</li>
 *             </ul>
 *           </li>
 *         </ul>
 */
static int _dictExpandIfNeeded(dict *d)
{
    /* Incremental rehashing already in progress. Return. */
	// 上一轮扩容还没结束(hash桶还没迁移完)
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    // hash表刚初始化出来 把hash表数组初始化到4个长度
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
	/**
	 * 扩容阈值
	 * 键值对数量/数组长度>5
	 */
	// TODO: 2023/12/13 这个地方有2点没明白 在dictTypeExpandAllowed和当前这个函数中 为什么用used+1而不是直接用used 从上面的判断分支走下来 used为0的情况早已经排除了
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio) &&
        dictTypeExpandAllowed(d))
    {
	    // 扩容新的数组长度是>节点数的2的幂次方 也就为扩容基准并不是现在数组大小 而是现在节点数量 最坏情况下没有hash冲突保证容纳所有节点
        return dictExpand(d, d->ht[0].used + 1);
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
/**
 * 已知key(key和key的hash值)计算出它在hash表数组中的脚标
 * @param existing
 * @return         返回值就2种情况 要么-1 要么非-1
 *                 <ul>
 *                   <li>非-1 就是非负数 表示的key所有在的数组脚标(hash桶位置) 就是根据key的hash值计算出来应该放到hash表数组的什么位置
 *                     <ul>
 *                       <li>dict没有在rehash说明只有1张hash表 那脚标就是[0]号表的脚标</li>
 *                       <li>dict在rehash 说明有2张hash表 那脚标就有可能是[0]号表的也有可能是[1]号表的</li>
 *                     </ul>
 *                   </li>
 *                   <li>-1 计算key在hash表中脚标失败 脚标只依赖于key的hash值和hash表数组的长度
 *                     <ul>
 *                       <li>可能是因为key已经存在了 不需要计算出来key在数组中的脚标</li>
 *                       <li>真的计算不出来 可能在查找过程中发现dict需要扩容 扩容操作失败了</li>
 *                     </ul>
 *                   </li>
 *                 </ul>
 */
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing)
{
    /**
     * idx key落在数组的脚标位置 可能是[0]号表的数组位置也可能是[1]号表的数组位置
     * table 遍历hash表 标识[0]号表[1]号表
     */
    unsigned long idx, table;
	// 存放在hash表数组hash桶的链表头
    dictEntry *he;
    /**
     * 预警式放置指针污染 只有当确定key存在于hash表中时才将该指针指向节点
     * 这个操作前置的原因是 下面一个步骤就要考察dict是否需要扩容 如果发现了有扩容需求就会触发扩容
     * 而扩容可能会失败 如果扩容失败了就会中断当前查找流程
     * 所以如果这个地方不初始化内存 到时候调用方拿到的查找结果-1是扩容失败导致的 会引起调用方误解(以为key已经存在了) 继而得到了脏数据
     */
    if (existing) *existing = NULL;

    /* Expand the hash table if needed */
	/**
	 * 考察dict是否需要扩容
	 * 返回-1标识因为扩容失败 没法计算出来key所在的脚标
	 */
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;
	/**
	 * 轮询hash表找key
	 * <ul>
	 *   <li>扩容导致的rehash还没结束就轮询2张表</li>
	 *   <li>没在rehash就只找[0]号表</li>
	 * </ul>
	 * 判断分支放在[0]号表遍历完 极大简化代码
	 */
    for (table = 0; table <= 1; table++) {
        // key所在的数组脚标 位运算计算key的hash桶脚标
        idx = hash & d->ht[table].sizemask;
        /* Search if this slot does not already contain the given key */
        // 遍历单链表找到key
        he = d->ht[table].table[idx];
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
			    /**
			     * 找到了key
			     * 根据调用方需求 调用方需要返回已经存在的key 就把找到的键值对返回出去
			     */
                if (existing) *existing = he;
				// 返回-1标识key已经存在
                return -1;
            }
            he = he->next;
        }
        // 字典没有在rehash 就考察[0]号表就行
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
