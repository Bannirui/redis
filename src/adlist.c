/* adlist.c - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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


#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* Create a new list. The created list can be freed with
 * listRelease(), but private value of every node need to be freed
 * by the user before to call listRelease(), or by setting a free method using
 * listSetFreeMethod.
 *
 * On error, NULL is returned. Otherwise the pointer to the new list. */
/**
 * 链表实例化
 * @return 双链表实例
 */
list *listCreate(void)
{
    struct list *list;

    // 申请内存空间 申请48 bytes
    if ((list = zmalloc(sizeof(*list))) == NULL)
        return NULL;
    // 初始化操作
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL; // 节点复制函数
    list->free = NULL; // 节点释放函数
    list->match = NULL; // 节点匹配函数
    return list;
}

/* Remove all the elements from the list without destroying the list itself. */
/**
 * 头链表头开始 正向遍历链表 逐个回收
 */
void listEmpty(list *list)
{
    // 用于记录链表的长度 有多少个节点
    unsigned long len;
    listNode *current, *next;

    current = list->head;
    len = list->len;
	// 从链表头开始 轮询整个链表 回收每个节点的内存
    while(len--) {
	    // 缓存后继节点 一会要回收当前节点了 得提前记下来后继节点
        next = current->next;
		// 防止链表中val是指针类型的数据 避免内存泄漏
        if (list->free) list->free(current->value);
		// 回收节点
        zfree(current);
		// 步进到后继节点
        current = next;
    }
	// 成员归零
    list->head = list->tail = NULL;
    list->len = 0;
}

/* Free the whole list.
 *
 * This function can't fail. */
/**
 * 链表回收
 * <ul>
 *   <li>链表中节点的资源回收</li>
 *   <li>链表本身回收</li>
 * </ul>
 */
void listRelease(list *list)
{
    // 回收链表上所有节点的内存
    listEmpty(list);
    // 回收链表内存
    zfree(list);
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
/**
 * 新增元素作链表的头节点
 * <ul>
 *   <li>空链表的时候直接初始化为头</li>
 *   <li>头插法</li>
 * </ul>
 */
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;
    // 列表节点内存申请 大小为24字节
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
	// 节点的值 value字段
    node->value = value;
    if (list->len == 0) {
	    /**
	     * 空链表
	     * 加进来的这个几点就是当头节点的
	     * 初始化两个哨兵指针
	     */
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
	    /**
	     * 新节点头插到现成的链表上当头
	     */
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
	// 链表节点计数
    list->len++;
    return list;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
/**
 * 新增元素作链表的尾节点
 * <ul>
 *   <li>空链表的时候初始化为节点</li>
 *   <li>尾插法</li>
 * </ul>
 */
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;

    // 申请内存24字节
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    // 节点的value字段
    node->value = value;
    if (list->len == 0) {
	    // 空链表 初始化尾节点
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
	    // 尾插法
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    // 链表节点计数
    list->len++;
    return list;
}

/**
 * 给定参考节点 新增元素到参考节点的前驱或者后继位置
 * @param old_node 参考基准位置
 * @param after    位置标识
 *                 <ul>
 *                   <li>0 标识新增元素挂到参考基准位置的前驱</li>
 *                   <li>非0 标识新增元素挂到参考基准位置的后继</li>
 *                 </ul>
 */
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    // 实例化链表节点
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    node->value = value;
    if (after) {
	    // 新增节点挂到参考位置的后继
        node->prev = old_node;
        node->next = old_node->next;
		// 考察哨兵要更新
        if (list->tail == old_node) {
            list->tail = node;
        }
    } else {
	    // 新增节点挂到参考位置的前驱
        node->next = old_node;
        node->prev = old_node->prev;
		// 考察哨兵要更新
        if (list->head == old_node) {
            list->head = node;
        }
    }
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
	// 新增了元素 更新节点计数
    list->len++;
    return list;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 *
 * This function can't fail. */
/**
 * 删除链表上给定的节点
 * <ul>
 *   <li>首先从链表上移除节点</li>
 *   <li>其次将移除的节点内存进行回收</li>
 * </ul>
 * @param node 要删除的节点
 */
void listDelNode(list *list, listNode *node)
{
    /**
     * 从双链表上移除节点 要考察两个边界
     * <ul>
     *   <li>要移除的是头节点</li>
     *   <li>要移除的尾节点</li>
     * </ul>
     */
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;
	/**
	 * 存储在链表节点上的值是个指针类型的数据
	 * 为了防止内存泄漏 用户指定了内存回收函数
	 */
    if (list->free) list->free(node->value);
	// 回收节点内存
    zfree(node);
    // 链表节点计数
    list->len--;
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */
/**
 * 创建列表的迭代器
 * @param direction 标识迭代器的迭代方向
 *                  <ul>
 *                    <li>0 头->尾</li>
 *                    <li>1 尾->头</li>
 *                  </ul>
 * @return 返回创建出来的迭代器实例
 */
listIter *listGetIterator(list *list, int direction)
{
    // 实例化迭代器
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL) return NULL;
    if (direction == AL_START_HEAD)
        iter->next = list->head; // 迭代器方向 head->tail
    else
        iter->next = list->tail; // 迭代器方向 tail->head
    iter->direction = direction;
    return iter;
}

/* Release the iterator memory */
// 回收迭代器
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* Create an iterator in the list private iterator structure */
/**
 * 重置迭代器
 * 从头到尾的方向
 */
void listRewind(list *list, listIter *li) {
    li->next = list->head;
	// 迭代方向 头->尾
    li->direction = AL_START_HEAD;
}

/**
 * 重置迭代器
 * 从尾到头的方向
 */
void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
	// 迭代方向 尾->头
    li->direction = AL_START_TAIL;
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage
 * pattern is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *
 * */
/**
 * 使用迭代器进行遍历
 */
listNode *listNext(listIter *iter)
{
    // 遍历出来的链表节点
    listNode *current = iter->next;

    if (current != NULL) {
	    /**
	     * 根据迭代器的迭代方向找链表节点的前驱和后继节点
	     * <ul>
	     *   <li>迭代器方向是0 标识从头到尾 就找后继节点</li>
	     *   <li>迭代器方向是1 标识从尾到头 就找前驱节点</li>
	     * </ul>
	     * 更新好next指针指向的链表节点 为下一次遍历做准备
	     */
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
    return current;
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */
/**
 * 复制链表
 * @param orig 要复制的链表
 * @return 复制出来的链表
 */
list *listDup(list *orig)
{
    // 复制出来的链表
    list *copy;
	// 实例化一个链表迭代器
    listIter iter;
    listNode *node;

    if ((copy = listCreate()) == NULL) // 创建链表
        return NULL;
	// 指针函数的成员赋值
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
	/**
	 * 因为要复制链表上的每个节点 借助迭代器遍历目标链表上的元素
	 * 初始化目标链表的迭代器
	 */
    listRewind(orig, &iter);
	/**
	 * 通过迭代器遍历出来每个链表的节点
	 * 再把节点复制到新的链表上
	 * 老链表的遍历方向是从头到尾 那么新链表的挂载方式就是尾插
	 */
    while((node = listNext(&iter)) != NULL) {
	    // 存储链表节点上存储的元素
        void *value;

        if (copy->dup) {
            value = copy->dup(node->value);
            if (value == NULL) {
			    /**
			     * 整个链表但凡有一个节点复制失败 就算链表复制失败
			     * 回收新链表
			     */
                listRelease(copy);
                return NULL;
            }
        } else
            value = node->value;
		// 元素尾插到新的链表上
        if (listAddNodeTail(copy, value) == NULL) {
            listRelease(copy);
            return NULL;
        }
    }
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned. */
/**
 * 在链表中检索元素
 * @param key 要找的元素
 */
listNode *listSearchKey(list *list, void *key)
{
    // 实例化一个迭代器
    listIter iter;
	// 轮询链表过程中的节点
    listNode *node;
	/**
	 * 但凡涉及到链表的轮询遍历 都用迭代器
	 * 没有特定要求
	 * 就用从头到尾的迭代器即可
	 */
    listRewind(list, &iter);
    while((node = listNext(&iter)) != NULL) {
	    // 考察遍历到的链表节点是不是要找的目标节点
        if (list->match) {
            if (list->match(node->value, key)) {
                return node;
            }
        } else {
            if (key == node->value) {
                return node;
            }
        }
    }
	// 整条链表遍历完都没有找到
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned. */
/**
 * 根据脚标找链表节点
 * 正负标识方向
 * @param index 脚标索引
 *              <ul>假使链表有N个节点
 *                <li>p 非负数标识的是从0开始 [  0      1      2  ... N-1] 即这种情况是0-based从前往后遍历</li>
 *                <li>n 负数标识的从尾开始    [ -N  -(N-1) -(N-2) ... -1] 即这种情况是1-based从后往前遍历</li>
 *              </ul>
 */
listNode *listIndex(list *list, long index) {
    // 链表遍历过程中的节点
    listNode *n;
    // 索引脚标的有效性检验
    if (index < 0) {
	    // 负数转换成1-based的从后往前遍历
        index = (-index)-1;
        n = list->tail;
        while(index-- && n) n = n->prev;
    } else {
	    // 非负数转换成0-based的从前往后遍历
        n = list->head;
        while(index-- && n) n = n->next;
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
/**
 * 尾节点晋升为头节点
 * 其他节点的顺序不变
 */
void listRotateTailToHead(list *list) {
    if (listLength(list) <= 1) return;

    /* Detach current tail */
    listNode *tail = list->tail;
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* Move it as head */
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}

/* Rotate the list removing the head node and inserting it to the tail. */
/**
 * 头节点降级为尾节点
 * 其他节点的顺序不变
 */
void listRotateHeadToTail(list *list) {
    if (listLength(list) <= 1) return;

    listNode *head = list->head;
    /* Detach current head */
    list->head = head->next;
    list->head->prev = NULL;
    /* Move it as tail */
    list->tail->next = head;
    head->next = NULL;
    head->prev = list->tail;
    list->tail = head;
}

/* Add all the elements of the list 'o' at the end of the
 * list 'l'. The list 'other' remains empty but otherwise valid. */
/**
 * 链表o尾插到链表l上
 */
void listJoin(list *l, list *o) {
    if (o->len == 0) return;

    o->head->prev = l->tail;

    if (l->tail)
        l->tail->next = o->head;
    else
        l->head = o->head;

    l->tail = o->tail;
	// 两条链表的节点计数
    l->len += o->len;

    /* Setup other as an empty list. */
    o->head = o->tail = NULL;
    o->len = 0;
}
