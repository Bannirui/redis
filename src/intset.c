/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "intset.h"
#include "zmalloc.h"
#include "endianconv.h"
#include "redisassert.h"

/* Note that these encodings are ordered, so:
 * INTSET_ENC_INT16 < INTSET_ENC_INT32 < INTSET_ENC_INT64. */
#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

/* Return the required encoding for the provided value. */
/**
 * @brief 确认整数的编码方式 共3种大小 16bit\32bit\64bit
 * @param v 整数
 * @return 编码整数的字节大小
 */
static uint8_t _intsetValueEncoding(int64_t v) {
    if (v < INT32_MIN || v > INT32_MAX)
        return INTSET_ENC_INT64;
    else if (v < INT16_MIN || v > INT16_MAX)
        return INTSET_ENC_INT32;
    else
        return INTSET_ENC_INT16;
}

/* Return the value at pos, given an encoding. */
/**
 * @brief 获取intset的pos位置的元素
 * @param is intset
 * @param pos 数组脚标
 * @param enc 编码方式
 * @return 获取到的整数
 */
static int64_t _intsetGetEncoded(intset *is, int pos, uint8_t enc) {
    int64_t v64;
    int32_t v32;
    int16_t v16;

    if (enc == INTSET_ENC_INT64) {
        memcpy(&v64,((int64_t*)is->contents)+pos,sizeof(v64));
        memrev64ifbe(&v64);
        return v64;
    } else if (enc == INTSET_ENC_INT32) {
        memcpy(&v32,((int32_t*)is->contents)+pos,sizeof(v32));
        memrev32ifbe(&v32);
        return v32;
    } else {
        memcpy(&v16,((int16_t*)is->contents)+pos,sizeof(v16));
        memrev16ifbe(&v16);
        return v16;
    }
}

/* Return the value at pos, using the configured encoding. */
/**
 * @brief 获取指定脚标位置的元素
 * @param is intset实例
 * @param pos 数组脚标位置
 * @return 获取到指定脚标位置的整数
 */
static int64_t _intsetGet(intset *is, int pos) {
    return _intsetGetEncoded(is,pos,intrev32ifbe(is->encoding));
}

/* Set the value at pos, using the configured encoding. */
/**
 * @brief 将整数添加到集合指定的脚标位置
 * @param is intset
 * @param pos 数组脚标位置
 * @param value 要添加的整数
 */
static void _intsetSet(intset *is, int pos, int64_t value) {
    // 要添加的整数的编码
    uint32_t encoding = intrev32ifbe(is->encoding);

    if (encoding == INTSET_ENC_INT64) {
        ((int64_t*)is->contents)[pos] = value;
        memrev64ifbe(((int64_t*)is->contents)+pos);
    } else if (encoding == INTSET_ENC_INT32) {
        ((int32_t*)is->contents)[pos] = value;
        memrev32ifbe(((int32_t*)is->contents)+pos);
    } else {
        ((int16_t*)is->contents)[pos] = value;
        memrev16ifbe(((int16_t*)is->contents)+pos);
    }
}

/* Create an empty intset. */
/**
 * @brief 创建intset实例 默认编码是int16
 * @return intset实例
 */
intset *intsetNew(void) {
    // 申请8byte的内存
    intset *is = zmalloc(sizeof(intset));
    // 初始化
    is->encoding = intrev32ifbe(INTSET_ENC_INT16);
    is->length = 0;
    return is;
}

/* Resize the intset */
/**
 * @brief intset扩容 在既有的内存上分配连续内存空间
 *        [start...end]假设为现有的一片内存 在end后面继续申请空闲空间 成为[start...end...newEnd]
 *        也就是说新的数组中完全包含了之前所有的元素内容
 *        新追加的内存空间上全部写0
 * @param is 旧intset
 * @param len 要扩容到多大(能存放多少个元素)
 * @return 新intset
 */
static intset *intsetResize(intset *is, uint32_t len) {
    uint64_t size = (uint64_t)len*intrev32ifbe(is->encoding);
    assert(size <= SIZE_MAX - sizeof(intset));
    is = zrealloc(is,sizeof(intset)+size);
    return is;
}

/* Search for the position of "value". Return 1 when the value was found and
 * sets "pos" to the position of the value within the intset. Return 0 when
 * the value is not present in the intset and sets "pos" to the position
 * where "value" can be inserted. */
/**
 * @brief 二分查找有序数组
 * @param is intset实例
 * @param value 要查找的目标值
 * @param pos value已经存在于intset中就返回value所在的脚标
 *            intset中没有value 就返回<value的整数最大的脚标
 * @return 在intset中是否查找到了value值
 *         1标识intset中存在value 并且脚标在pos位置
 *         0标识intset中不存在value
 */
static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos) {
    // intset的脚标区间[min...max]
    int min = 0, max = intrev32ifbe(is->length)-1, mid = -1;
    int64_t cur = -1;

    /* The value can never be found when the set is empty */
    if (intrev32ifbe(is->length) == 0) { // intset为空 不存在元素
        if (pos) *pos = 0;
        return 0;
    } else {
        /* Check for the case where we know we cannot find the value,
         * but do know the insert position. */
        // intset中有元素 有序集合 升序 校验边界脚标
        if (value > _intsetGet(is,max)) { // 查找的值比intset最大值还大
            if (pos) *pos = intrev32ifbe(is->length);
            return 0;
        } else if (value < _intsetGet(is,0)) { // 查找的值比intset最小值还小
            if (pos) *pos = 0;
            return 0;
        }
    }

    // 有序数组二分查找
    while(max >= min) {
        mid = ((unsigned int)min + (unsigned int)max) >> 1;
        cur = _intsetGet(is,mid);
        if (value > cur) {
            min = mid+1;
        } else if (value < cur) {
            max = mid-1;
        } else {
            break;
        }
    }

    if (value == cur) {
        if (pos) *pos = mid;
        return 1;
    } else {
        if (pos) *pos = min;
        return 0;
    }
}

/* Upgrades the intset to a larger encoding and inserts the given integer. */
/**
 * @brief intset因为整数编码而扩容 并将新整数添加到集合中 保证有序性
 * @param is inset集合
 * @param value 添加的整数
 * @return 升级编码之后的新集合
 */
static intset *intsetUpgradeAndAdd(intset *is, int64_t value) {
    // intset旧的编码方式
    uint8_t curenc = intrev32ifbe(is->encoding);
    // intset新的编码方式
    uint8_t newenc = _intsetValueEncoding(value);
    // 旧inset中有多少个整数
    int length = intrev32ifbe(is->length);
    // value<0就往新intset头上添加 否则往新intset尾上添加
    int prepend = value < 0 ? 1 : 0;

    /* First set new encoding and resize */
    // 新的编码 要根据编码方式额外申请内存
    is->encoding = intrev32ifbe(newenc);
    // inset额外追加1个脚标的内存空间(具体多少字节要根据整数编码而定)
    is = intsetResize(is,intrev32ifbe(is->length)+1);

    /* Upgrade back-to-front so we don't overwrite values.
     * Note that the "prepend" variable is used to make sure we have an empty
     * space at either the beginning or the end of the intset. */
    /**
     * 从最后一个脚标往前
     * 取的时候按照老编码方式取
     * 放的时候按照新编码方式放
     * 效果就是将intset结合中元素从最后一个元素开始往前依次将放到新的位置
     * 如果待添加的元素>=0 就将最后一个脚标空出来
     * 如果待添加的元素<0 就将第一个脚标空出来
     *
     * 为什么这样设计 很巧妙地保证了intset集合的有序性
     *   - 首先 在编码升级之前 该intset集合是严格有序的
     *   - 该函数的入口是因为编码升级触发的 意味着新添加的数据字节变大了 那么
     *     - data>max(intset)
     *     - data<min(intset)
     *   - 所以 对intset柔性数组扩容之后 之前的整数顺序不变
     *     - 新加进来的数<0 一定就是新集合中最小的数 往最前面放
     *     - 新加进来的数>0 一定就是新集合中最大的数 往最后面放
     */
    while(length--) // 循环体中length [长度-1...0]
        _intsetSet(is,length+prepend,_intsetGetEncoded(is,length,curenc));

    /* Set the value at the beginning or the end. */
    if (prepend) // 新增的数字<0 添加到新数组最前面
        _intsetSet(is,0,value);
    else // 新增的数>=0 添加到新数组最后面
        _intsetSet(is,intrev32ifbe(is->length),value);
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);
    return is;
}

/**
 * @brief 元素保持相对位置移到intset集合最后 目的是为了空出[from...to)位置用来写新元素
 * @param is intset
 * @param from, to 有序集合intset中[from...]区间保持相对位置挪到intset的[to...]区间上
 */
static void intsetMoveTail(intset *is, uint32_t from, uint32_t to) {
    void *src, *dst;
    uint32_t bytes = intrev32ifbe(is->length)-from;
    uint32_t encoding = intrev32ifbe(is->encoding);

    if (encoding == INTSET_ENC_INT64) {
        src = (int64_t*)is->contents+from;
        dst = (int64_t*)is->contents+to;
        bytes *= sizeof(int64_t);
    } else if (encoding == INTSET_ENC_INT32) {
        src = (int32_t*)is->contents+from;
        dst = (int32_t*)is->contents+to;
        bytes *= sizeof(int32_t);
    } else {
        src = (int16_t*)is->contents+from;
        dst = (int16_t*)is->contents+to;
        bytes *= sizeof(int16_t);
    }
    memmove(dst,src,bytes);
}

/* Insert an integer in the intset */
/**
 * @brief 将value整数添加到intset中 保证intset集合中整数的有序性和唯一性
 * @param is intset集合实例
 * @param value 要添加到集合中的整数
 * @param success 标识添加元素成功与否
 *                1标识添加成功
 *                0标识添加失败 比如往intset中添加重复元素
 * @return intset集合
 */
intset *intsetAdd(intset *is, int64_t value, uint8_t *success) {
    uint8_t valenc = _intsetValueEncoding(value); // 整数的编码方式
    /**
     * pos指向的是intset有序集合中<=value的整数的最大脚标位置
     * 即
     * intset中包含value 指向value所在脚标
     * intset中不包含value 指向离value最近的脚标
     */
    uint32_t pos;
    if (success) *success = 1;

    /* Upgrade encoding if necessary. If we need to upgrade, we know that
     * this value should be either appended (if > 0) or prepended (if < 0),
     * because it lies outside the range of existing values. */
    if (valenc > intrev32ifbe(is->encoding)) { // 整数编码要升级
        /* This always succeeds, so we don't need to curry *success. */
        return intsetUpgradeAndAdd(is,value);
    } else { // 整数编码不需要升级
        /* Abort if the value is already present in the set.
         * This call will populate "pos" with the right position to insert
         * the value when it cannot be found. */
        if (intsetSearch(is,value,&pos)) { // 要添加的整数已经存在了
            if (success) *success = 0;
            return is;
        }

        /**
         * 给集合intset追加申请1个脚标的空间 假设长度为len 之前所有元素还分布在前len-1个脚标上
         * 现在的布局为[0...pos pos+1...n-2 n-1]
         * [0...n-2]为之前的所有整数
         * 把[pos+1...n-2]整数全部后移1个脚标也就是分布在[pos+2...n-1]上了
         * 然后把新元素添加到pos上
         */
        is = intsetResize(is,intrev32ifbe(is->length)+1);
        if (pos < intrev32ifbe(is->length)) intsetMoveTail(is,pos,pos+1); // 从pos位置开始元素后移1个脚标 空出pos位置
    }
    // 新添加的整数应该插入的有序集合位置
    _intsetSet(is,pos,value);
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1); // intset中新增了一个元素 维护length字段
    return is;
}

/* Delete integer from intset */
/**
 * @brief 如果整数value在inset集合中 就删除value
 * @param is intset集合
 * @param value 要删除的整数
 * @param success 标识intset中包含value 可以进行删除
 * @return 删除元素之后的intset集合
 */
intset *intsetRemove(intset *is, int64_t value, int *success) {
    // 整数编码
    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;
    if (success) *success = 0;

    if (valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,&pos)) { // 二分查找value
        uint32_t len = intrev32ifbe(is->length);

        /* We know we can delete */
        if (success) *success = 1;

        /* Overwrite value with tail and update length */
        /**
         * 将pos脚标位置删除
         * 采用原地算法实现
         * 将[pos+1...n-1]的所有元素前移1个脚标[pos...n-1]
         * 然后将数组缩容1个脚标 就实现了删除效果
         */
        if (pos < (len-1)) intsetMoveTail(is,pos+1,pos);
        is = intsetResize(is,len-1);
        is->length = intrev32ifbe(len-1);
    }
    return is;
}

/* Determine whether a value belongs to this set */
/**
 * @brief intset中是否存在元素value
 * @param is intset实例
 * @param value 要查找的值
 * @return 1标识intset中存在value
 *         0标识intset中不存在value
 */
uint8_t intsetFind(intset *is, int64_t value) {
    uint8_t valenc = _intsetValueEncoding(value);
    return valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,NULL);
}

/* Return random member */
int64_t intsetRandom(intset *is) {
    uint32_t len = intrev32ifbe(is->length);
    assert(len); /* avoid division by zero on corrupt intset payload. */
    return _intsetGet(is,rand()%len);
}

/* Get the value at the given position. When this position is
 * out of range the function returns 0, when in range it returns 1. */
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value) {
    if (pos < intrev32ifbe(is->length)) {
        *value = _intsetGet(is,pos);
        return 1;
    }
    return 0;
}

/* Return intset length */
uint32_t intsetLen(const intset *is) {
    return intrev32ifbe(is->length);
}

/* Return intset blob size in bytes. */
size_t intsetBlobLen(intset *is) {
    return sizeof(intset)+(size_t)intrev32ifbe(is->length)*intrev32ifbe(is->encoding);
}

/* Validate the integrity of the data structure.
 * when `deep` is 0, only the integrity of the header is validated.
 * when `deep` is 1, we make sure there are no duplicate or out of order records. */
int intsetValidateIntegrity(const unsigned char *p, size_t size, int deep) {
    intset *is = (intset *)p;
    /* check that we can actually read the header. */
    if (size < sizeof(*is))
        return 0;

    uint32_t encoding = intrev32ifbe(is->encoding);

    size_t record_size;
    if (encoding == INTSET_ENC_INT64) {
        record_size = INTSET_ENC_INT64;
    } else if (encoding == INTSET_ENC_INT32) {
        record_size = INTSET_ENC_INT32;
    } else if (encoding == INTSET_ENC_INT16){
        record_size = INTSET_ENC_INT16;
    } else {
        return 0;
    }

    /* check that the size matchies (all records are inside the buffer). */
    uint32_t count = intrev32ifbe(is->length);
    if (sizeof(*is) + count*record_size != size)
        return 0;

    /* check that the set is not empty. */
    if (count==0)
        return 0;

    if (!deep)
        return 1;

    /* check that there are no dup or out of order records. */
    int64_t prev = _intsetGet(is,0);
    for (uint32_t i=1; i<count; i++) {
        int64_t cur = _intsetGet(is,i);
        if (cur <= prev)
            return 0;
        prev = cur;
    }

    return 1;
}

#ifdef REDIS_TEST
#include <sys/time.h>
#include <time.h>

#if 0
static void intsetRepr(intset *is) {
    for (uint32_t i = 0; i < intrev32ifbe(is->length); i++) {
        printf("%lld\n", (uint64_t)_intsetGet(is,i));
    }
    printf("\n");
}

static void error(char *err) {
    printf("%s\n", err);
    exit(1);
}
#endif

static void ok(void) {
    printf("OK\n");
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

static intset *createSet(int bits, int size) {
    uint64_t mask = (1<<bits)-1;
    uint64_t value;
    intset *is = intsetNew();

    for (int i = 0; i < size; i++) {
        if (bits > 32) {
            value = (rand()*rand()) & mask;
        } else {
            value = rand() & mask;
        }
        is = intsetAdd(is,value,NULL);
    }
    return is;
}

static void checkConsistency(intset *is) {
    for (uint32_t i = 0; i < (intrev32ifbe(is->length)-1); i++) {
        uint32_t encoding = intrev32ifbe(is->encoding);

        if (encoding == INTSET_ENC_INT16) {
            int16_t *i16 = (int16_t*)is->contents;
            assert(i16[i] < i16[i+1]);
        } else if (encoding == INTSET_ENC_INT32) {
            int32_t *i32 = (int32_t*)is->contents;
            assert(i32[i] < i32[i+1]);
        } else {
            int64_t *i64 = (int64_t*)is->contents;
            assert(i64[i] < i64[i+1]);
        }
    }
}

#define UNUSED(x) (void)(x)
int intsetTest(int argc, char **argv, int accurate) {
    uint8_t success;
    int i;
    intset *is;
    srand(time(NULL));

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    printf("Value encodings: "); {
        assert(_intsetValueEncoding(-32768) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(+32767) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(-32769) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+32768) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483648) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+2147483647) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483649) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+2147483648) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(-9223372036854775808ull) ==
                    INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+9223372036854775807ull) ==
                    INTSET_ENC_INT64);
        ok();
    }

    printf("Basic adding: "); {
        is = intsetNew();
        is = intsetAdd(is,5,&success); assert(success);
        is = intsetAdd(is,6,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(!success);
        ok();
        zfree(is);
    }

    printf("Large number of random adds: "); {
        uint32_t inserts = 0;
        is = intsetNew();
        for (i = 0; i < 1024; i++) {
            is = intsetAdd(is,rand()%0x800,&success);
            if (success) inserts++;
        }
        assert(intrev32ifbe(is->length) == inserts);
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Upgrade from int16 to int32: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,65535));
        checkConsistency(is);
        zfree(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-65535));
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Upgrade from int16 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);
        zfree(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Upgrade from int32 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);
        zfree(is);

        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
        zfree(is);
    }

    printf("Stress lookups: "); {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits,size);
        checkConsistency(is);

        start = usec();
        for (i = 0; i < num; i++) intsetSearch(is,rand() % ((1<<bits)-1),NULL);
        printf("%ld lookups, %ld element set, %lldusec\n",
               num,size,usec()-start);
        zfree(is);
    }

    printf("Stress add+delete: "); {
        int i, v1, v2;
        is = intsetNew();
        for (i = 0; i < 0xffff; i++) {
            v1 = rand() % 0xfff;
            is = intsetAdd(is,v1,NULL);
            assert(intsetFind(is,v1));

            v2 = rand() % 0xfff;
            is = intsetRemove(is,v2,NULL);
            assert(!intsetFind(is,v2));
        }
        checkConsistency(is);
        ok();
        zfree(is);
    }

    return 0;
}
#endif
