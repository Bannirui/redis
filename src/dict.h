/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
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

#ifndef __DICT_H
#define __DICT_H

#include "mt19937-64.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define DICT_OK 0 // hash表操作码-成功
#define DICT_ERR 1 // hash表操作码-失败

/* Unused arguments generate annoying warnings... */
/**
 * 函数原型中设计了n个形式参数 函数体中只用了m(m<n)
 * 老编译器可能存在的warning行为 提示用户存在已经声明的变量未使用
 * 我使用的gcc编译器是13.x版本 不存在这个问题
 * 因此这个宏的目的就是消除编译器的这种warning
 */
#define DICT_NOTUSED(V) ((void) V)

// 哈希表的节点 存储k-v键值对
// hash表的节点会组成一个单链表
// key可以是void*指针类型 相当于Java的Object
typedef struct dictEntry {
    void *key; // 节点的key
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v; // 节点的v 可以是指针\u64整数\int64整数\浮点数
    struct dictEntry *next; // 下一个节点 相当于单链表
} dictEntry;

// 字典类型
// 提供相关函数的指针 用于扩展
// 相当于实现了多态 不同的dictType对应的函数指针不一样
typedef struct dictType {
    uint64_t (*hashFunction)(const void *key); // 键的hash函数
    void *(*keyDup)(void *privdata, const void *key); // 复制键的函数
    void *(*valDup)(void *privdata, const void *obj); // 复制值的函数
    int (*keyCompare)(void *privdata, const void *key1, const void *key2); // 键的比较函数
    void (*keyDestructor)(void *privdata, void *key); // 键的销毁函数
    void (*valDestructor)(void *privdata, void *obj); // 值的销毁函数
    int (*expandAllowed)(size_t moreMem, double usedRatio); // 根据扩容后的数组的内存和负载因子判断是否可以扩容
} dictType;

/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
// 哈希表
typedef struct dictht {
    /**
     * hash表的数组
     * table成员类型是指针变量 这个指针地址存储的数据类型又是dictEntry*
     * dictEntry*又是一个指针变量 这个指针地址存储的数据类型是dictEntry
     * 即table是一个数组 每个数组元素存放的是一个单向链表
     */
    dictEntry **table;
	/**
	 * hash表的大小 即hash表数组长度 可以容纳多少个hash桶
	 */
    unsigned long size;
	/**
	 * sizemask=size-1
	 * 纯粹的冗余成员
	 * 在hash数组长度是2的幂次方前提之下
	 * 计算key所在hash桶的脚标索引idx = hash(key) & (size-1)
	 *                            = hash(key) & sizemask
	 */
    unsigned long sizemask;
	/**
	 * 键值对计数
	 * 统计hash表中存储了多少个键值对的entry节点
	 */
    unsigned long used;
} dictht;

/**
 * 字典
 * 字典是由两个hash表组成的 常用的hash表是ht[0] 当进行rehash时使用到ht[1]进行渐进式rehash
 * type和privdata为了实现多态
 * type保存了特定函数的指针
 * privdata携带了特定函数需要的一些可选参数
 * redis根据字典的用途 在type中设置不同的特定函数
 */
typedef struct dict {
    dictType *type; // 字典的类型指针
	/**
	 * 这个成员应该仅仅算是预留的一个空能扩展点
	 * 目前而言还没看到实际的用处
	 * <ul>
	 *   <li>在keyCompare函数接口中声明了 看是看了整个源码中xxxKeyCompare的实现这个成员都使用了 DICT_NOTUSED这个宏定义 也就意味着在privdata在keyCompare函数中是没有使用的</li>
	 * </ul>
	 */
    void *privdata;
    // 2个hash表 用于渐进式rehash
    dictht ht[2];

	/**
	 * 标识dict的rehash状态
	 * <ul>
	 *   <li>-1 标识dict没有处于rehash</li>
	 *   <li>另一种情况只能是unsigned 标识的是dict在rehash 并且要将[0]号表的对应脚标的hash桶迁移到[1]号表上去</li>
	 * </ul>
	 */
    long rehashidx; /* rehashing not in progress if rehashidx == -1 */

	 /**
	  * 用于状态标识 rehash暂停状态
	  * 为什么需要暂停rehash呢 迭代器模式提供了一种轮询数据的方式 但是dict的设计出于对扩容之后数据离散重hash的性能设计 用了2张hash表
	  * 以如下场景为例
	  *   dict处于rehash中 外部通过接口查看数据 在整个迭代过程中rehash都没有结束掉
	  *   轮询到了[1]号表x脚标 即意味着[0]号表和[1]号表的[0...x)都已经轮询过了
	  *   但是此时rehash的脚标为[0]号表的m 恰好这个m被rehash到了[1]号表的y(y>x)
	  *   那么就会导致在轮询过程中[0]号表hash桶的数据被访问了2次
	  * 因此为了设计出安全的迭代模式 源码给出了两种模式
	  * <ul>
	  *   <li>普通的迭代器</li>
	  *   <li>安全的迭代器</li>
	  * </ul>
	  * 为了上述两种模式的区别 设计了pauserehash这个成员用于标识和控制
	  * <ul>
	  *   <li>0 正常状态 这个正常状态又可以根据是否在rehash细分出2种状态 因为有2中状态 所以要配合rehashidx配合使用<ul>
	  *     <li>dict没有在rehash</li>
	  *     <li>dict在rehash</li>
	  *   </ul></li>
	  *   <li>正数 标识rehash是暂停的状态</li>
	  *   <li>负数 标识rehash异常</li>
	  * </ul>
	  */
    int16_t pauserehash; /* If >0 rehashing is paused (<0 indicates coding error) */
} dict;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
// 迭代器
// 迭代器分为安全迭代和非安全迭代
// 安全迭代中会将rehash暂停
// fingerprint根据字典内存地址生成的64位hash值 代表着字典当前状态 非安全迭代中 如果字典内存发生了新的变化 则fingerprint的值也会发生变化 用于非安全迭代的快速失败
typedef struct dictIterator {
    dict *d; // 指向字典指针
    long index; // 标识着哪些槽已经遍历过
    // table 当前正在迭代的hash表 [0...1]
    // safe 标识是否安全
    int table, safe;
    // entry 标识当前已经返回的节点
    // nextEntry 标识下一个节点
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    // 字典dict当前状态签名 64位hash值
    long long fingerprint;
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
typedef void (dictScanBucketFunction)(void *privdata, dictEntry **bucketref);

/* This is the initial size of every hash table */
// 初始化hash表的容量
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        (entry)->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        (entry)->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do { (entry)->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { (entry)->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \
    do { (entry)->v.d = _val_; } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        (entry)->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        (entry)->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key)
#define dictGetKey(he) ((he)->key)
#define dictGetVal(he) ((he)->v.val)
#define dictGetSignedIntegerVal(he) ((he)->v.s64)
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)
#define dictGetDoubleVal(he) ((he)->v.d)
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
#define dictIsRehashing(d) ((d)->rehashidx != -1)
#define dictPauseRehashing(d) (d)->pauserehash++
#define dictResumeRehashing(d) (d)->pauserehash--

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff
#define randomULong() ((unsigned long) genrand64_int64())
#else
#define randomULong() random()
#endif

/* API */
// dict实例化
dict *dictCreate(dictType *type, void *privDataPtr);
// hash表扩容
int dictExpand(dict *d, unsigned long size);
int dictTryExpand(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);
dictEntry *dictAddOrFind(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
int dictDelete(dict *d, const void *key);
dictEntry *dictUnlink(dict *ht, const void *key);
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
dictEntry *dictGetFairRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictGetStats(char *buf, size_t bufsize, dict *d);
uint64_t dictGenHashFunction(const void *key, int len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void*));
void dictEnableResize(void);
void dictDisableResize(void);
int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(uint8_t *seed);
uint8_t *dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);
uint64_t dictGetHash(dict *d, const void *key);
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#ifdef REDIS_TEST
int dictTest(int argc, char *argv[], int accurate);
#endif

#endif /* __DICT_H */
