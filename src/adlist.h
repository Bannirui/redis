/* adlist.h - A generic doubly linked list implementation
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

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */

// 链表节点 双链表
typedef struct listNode {
    // 前驱节点
    struct listNode *prev;
    // 后继节点
    struct listNode *next;
    // 节点存储的值
    void *value;
} listNode;

/**
 * 链表的迭代器
 * 单向
 * <ul>
 *   <li>要么是头->尾方向</li>
 *   <li>要么是尾->头方向</li>
 * </ul>
 */
typedef struct listIter {
    // 当前迭代位置的后驱节点
    listNode *next;
    /**
     * 标识迭代方向
     * <ul>
     *   <li>0 头->尾</li>
     *   <li>1 尾->头</li>
     * </ul>
     */
    int direction;
} listIter;

// 链表 双链表
typedef struct list {
    // 哨兵指针
    listNode *head;
    // 哨兵指针
    listNode *tail;
    // 复制函数指针 负责实现链表节点的复制 没有指定就浅拷贝
    void *(*dup)(void *ptr);
    // 释放函数指针 负责实现链表节点的值的释放
    void (*free)(void *ptr);
    // 匹配函数指针 负责搜索链表时匹配链表节点值
    int (*match)(void *ptr, void *key);
    // 链表长度 链表中挂了多少个节点
    unsigned long len;
} list;

/* Functions implemented as macros */
#define listLength(l) ((l)->len) // 链表长度
#define listFirst(l) ((l)->head) // 链表头节点 实节点
#define listLast(l) ((l)->tail) // 链表尾节点 实节点
#define listPrevNode(n) ((n)->prev) // 节点的前驱节点
#define listNextNode(n) ((n)->next) // 节点的后继节点
#define listNodeValue(n) ((n)->value) // 节点的值 函数指针

#define listSetDupMethod(l,m) ((l)->dup = (m)) // 链表节点值的复制方法 设置
#define listSetFreeMethod(l,m) ((l)->free = (m)) // 链表节点值的释放方法 设置
#define listSetMatchMethod(l,m) ((l)->match = (m)) // 链表节点值的匹配方法 设置

#define listGetDupMethod(l) ((l)->dup)
#define listGetFreeMethod(l) ((l)->free)
#define listGetMatchMethod(l) ((l)->match)

/* Prototypes */
// 创建链表
list *listCreate(void);
// 链表的释放
void listRelease(list *list);
// 头链表头开始 正向遍历链表 逐个回收
void listEmpty(list *list);
// 新增元素作链表的头节点
list *listAddNodeHead(list *list, void *value);
// 新增元素作链表的尾节点
list *listAddNodeTail(list *list, void *value);
// 给定参考节点 新增元素到参考节点的前驱或者后继位置
list *listInsertNode(list *list, listNode *old_node, void *value, int after);
// 删除链表上指定节点
void listDelNode(list *list, listNode *node);
// 创建列表的迭代器
listIter *listGetIterator(list *list, int direction);
// 使用迭代器进行遍历
listNode *listNext(listIter *iter);
// 回收迭代器
void listReleaseIterator(listIter *iter);
// 复制链表
list *listDup(list *orig);
// 在链表中检索元素
listNode *listSearchKey(list *list, void *key);
// 根据脚标索引链表节点
listNode *listIndex(list *list, long index);
// 重置迭代器 从头到尾的方向
void listRewind(list *list, listIter *li);
// 重置迭代器 从尾到头的方向
void listRewindTail(list *list, listIter *li);
// 尾节点晋升为头节点
void listRotateTailToHead(list *list);
// 头节点降级为尾节点
void listRotateHeadToTail(list *list);
// 链表o挂到链表l上
void listJoin(list *l, list *o);

/* Directions for iterators */
#define AL_START_HEAD 0 // 迭代方向 头->尾
#define AL_START_TAIL 1 // 迭代方向 尾->头

#endif /* __ADLIST_H__ */
