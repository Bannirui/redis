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
	/**
	 * 这个键值对的值设计成union很巧妙
	 * <ul>
	 *   <li>从数据类型的角度来讲这样的设计丰富了redis的dict支持的值的类型
	 *     <ul>
	 *       <li>既支持指针类型的数据</li>
	 *       <li>又支持非指针类型的数据
	 *         <ul>
	 *           <li>unsigned 64位</li>
	 *           <li>signed 64位</li>
	 *           <li>double</li>
	 *         </ul>
	 *       </li>
	 *     </ul>
	 *   </li>
	 *   <li>从内存占用角度来说 普遍64位系统 指针类型占8byte 上述非指针类型都是64位也是8byte 空间占用是一样的</li>
	 * </ul>
	 * 那为什么要支持值是非指针类型的呢
	 * 我的猜想是为了减少以后使用过程中的解引用操作(尤其是频繁地get数字加减乘除之后再set回去)
	 * 但是因为v的类型是union的 就注定代码复杂度立马倍数级上来 需要对u64 s64和double的3种类型单独定义get和set实现
	 * <ul>源码通过dictSetVal宏定义解决value类型赋值指针类型的数据
	 *   <li>涉及深拷贝时候 客户端定义valDup的实现 value拷贝的时候回调</li>
	 *   <li>浅拷贝的时候直接赋值即可</li>
	 * </ul>
	 * 至于数字类型的成员u64 s64或者d就需要客户端根据需要分别调用
	 * <ul>
	 *   <li>dictSetSignedIntegerVal</li>
	 *   <li>dictSetUnsignedIntegerVal</li>
	 *   <li>dictSetDoubleVal</li>
	 * </ul>
	 */
	// TODO: 2023/12/13 为什么不直接设计成void* 而要考虑数字
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next; // 下一个节点 相当于单链表
} dictEntry;

// 字典类型
// 提供相关函数的指针 用于扩展
// 相当于实现了多态 不同的dictType对应的函数指针不一样
typedef struct dictType {
    uint64_t (*hashFunction)(const void *key); // 键的hash函数
    void *(*keyDup)(void *privdata, const void *key); // 复制键的函数
    /**
     * <ul>
     *   <li>添加键值对的时候用到 value是指针类型的时候用自定义的valDup函数赋值</li>
     * </ul>
     */
	void *(*valDup)(void *privdata, const void *obj);
    int (*keyCompare)(void *privdata, const void *key1, const void *key2); // 键的比较函数
    void (*keyDestructor)(void *privdata, void *key); // 键的销毁函数
    void (*valDestructor)(void *privdata, void *obj); // 值的销毁函数

	/**
	 * 为什么需要这个函数
	 * 在考察是否需要扩容的时候 可能需要扩容很大的一个空间 那么将来在内存分配的时候可能会因为OOM而失败
	 * 这个函数可以不定义的 也就是将内存大小的合理性延迟到内存分配的时候裁决
	 * 当然内存分配函数也就是malloc系列调用涉及内核态切换 涉及性能开销
	 * 因此可以将内存申请的合理性前置化 在用户层完成
	 * @param moreMem   要申请的内存大小
	 * @param usedRatio 扩容前的内存使用比率=键值对数量/数组长度
	 */
	 // TODO: 2023/12/13 重点关注这个函数的实现 用什么样的方式判决要申请的内存是否合理
    int (*expandAllowed)(size_t moreMem, double usedRatio);
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
	 *   <li>在valDup函数接口中声明了 在实现中也是`((void) x);`这样的语句 定义中没有使用privdata</li>
	 * </ul>
	 */
	 // TODO: 2023/12/13 成员的作用
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

	/**
	 * table 当前正在迭代的hash表 可能是[0]号表也可能是[1]号表
	 * safe 标识是否安全
	 */
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

/**
 * dict中的键值对数据类型支持的很丰富 value支持
 * <ul>
 *   <li>指针类型</li>
 *   <li>无符号整数</li>
 *   <li>有符号整数</li>
 *   <li>浮点数</li>
 * </ul>
 * 这个宏就是来赋值用的val的 也就是说只用来处理指针类型的数据
 * 指针类型数据涉及深拷贝/浅拷贝 因此怎么复制交给调用方决定
 * <ul>
 *   <li>调用方指定了valDup函数 就用指定的函数进行拷贝</li>
 *   <li>调用放没有指定 因为是指针类型 就直接赋值 比如客户端断定这个数据前拷贝足矣</li>
 * </ul>
 */
#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        (entry)->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        (entry)->v.val = (_val_); \
} while(0)

// 支持键值对中的有符号整数
#define dictSetSignedIntegerVal(entry, _val_) \
    do { (entry)->v.s64 = _val_; } while(0)

// 支持键值对中的无符号整数
#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { (entry)->v.u64 = _val_; } while(0)

// 支持键值对中的浮点数
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
// dict中存储了多少个键值对
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
// 成员rehashidx是-1标识没有在rehash
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
// hash表扩容
int dictTryExpand(dict *d, unsigned long size);
// 写入键值对
int dictAdd(dict *d, void *key, void *val);
// 开放了非指针类型的value设置
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);
dictEntry *dictAddOrFind(dict *d, void *key);
// 新增/更新
int dictReplace(dict *d, void *key, void *val);
// 删除key
int dictDelete(dict *d, const void *key);
// 删除key 不释放内存
dictEntry *dictUnlink(dict *ht, const void *key);
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);
// 释放dict
void dictRelease(dict *d);
// 查找key
dictEntry * dictFind(dict *d, const void *key);
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
