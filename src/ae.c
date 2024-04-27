/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
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

#include "ae.h"
#include "anet.h"

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "zmalloc.h"
#include "config.h"

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
/**
 * 不同的系统平台有不同的多路复用器的实现
 * <ul>
 *   <li>linux的epoll</li>
 *   <li>macos的kqueue</li>
 *   <li>windows的select</li>
 * </ul>
 */
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
    #ifdef HAVE_EPOLL
    #include "ae_epoll.c"
    #else
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.c"
        #else
        #include "ae_select.c"
        #endif
    #endif
#endif


/**
 * 创建事件循环管理器
 * @param setsize 指定事件循环管理器的容量
 * @return 事件循环管理器
 */
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    monotonicInit();    /* just in case the calling app didn't initialize */

	// 内存开辟
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
	// 数组内存开辟 用来存放需要注册到多路复用器的事件
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);
	// 数组内存开辟 用来存放从多路复用器返回的就绪事件集合
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
	// 事件循环管理器容量
    eventLoop->setsize = setsize;
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
    eventLoop->aftersleep = NULL;
    eventLoop->flags = 0;
	/**
	 * 创建系统的多路复用器实例
	 * 系统平台有差异 每个系统的多路复用器实现有差异 这样就可以屏蔽对用户的实现差异
	 */
    if (aeApiCreate(eventLoop) == -1) goto err; // 我是很喜欢goto关键字的 新语言go中也使用了这个关键字 很难理解Java中竟然没有这个机制
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
	/**
	 * malloc分配的内存未初始化
	 * 防止以后引起状态误判 这个地方进行事件状态初始化
	 */
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* Return the current set size. */
/**
 * getter方法 事件循环器的容量
 */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/* Tells the next iteration/s of the event processing to set timeout of 0. */
/**
 * flags位状态标识不同的能力项
 */
void aeSetDontWait(aeEventLoop *eventLoop, int noWait) {
    if (noWait)
        eventLoop->flags |= AE_DONT_WAIT;
    else
        eventLoop->flags &= ~AE_DONT_WAIT;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */
/**
 * setter方法 设置事件循环器的容量 使用场景是扩大容量
 * @param setsize 期待将事件循环器设置到多大的容量
 * @return 状态码
 *         <ul>
 *           <li>0 标识成功</li>
 *           <li>-1 标识失败</li>
 *         </ul>
 */
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    int i;

    if (setsize == eventLoop->setsize) return AE_OK;
    if (eventLoop->maxfd >= setsize) return AE_ERR;
	// 扩大盛放多路复用器就绪事件的数组大小
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;

	// 扩大两个数组 events数组和fired数组
    eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
    eventLoop->setsize = setsize;

    /* Make sure that if we created new slots, they are initialized with
     * an AE_NONE mask. */
	/**
	 * 扩容的events数组中内存需要初始化
	 */
    for (i = eventLoop->maxfd+1; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    return AE_OK;
}

/**
 * 内存回收
 */
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);

    /* Free the time events list. */
	// 链表内存的回收 轮询链表节点逐个释放
    aeTimeEvent *next_te, *te = eventLoop->timeEventHead;
    while (te) {
        next_te = te->next;
        zfree(te);
        te = next_te;
    }
    zfree(eventLoop);
}

/**
 * setter方法 设置stop标识符
 */
void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

/**
 * 将fd以及对fd关注的IO事件类型封装IO任务 添加到事件管理器中
 * @param mask 要监听fd的什么类型IO事件 可读还是可写
 *             <ul>
 *               <li>1 监听可读事件</li>
 *               <li>2 监听可写事件</li>
 *             </ul>
 * @param proc 处理器 将来借助多路复用器发现fd事件状态就绪时可以找个合适的时机进行回调
 * @param clientData 回调的时候可能需要处理一些数据 将数据维护在eventLoop中供将来使用
 * @return 状态码
 *         <ul>
 *           <li>-1 标识失败</li>
 *           <li>0 标识成功</li>
 *         </ul>
 */
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    // 边界校验 fd要当作events数组的脚标使用 不能越界
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
    // fd就是脚标索引 在未就绪数组中找到对应位置 完成初始化
    aeFileEvent *fe = &eventLoop->events[fd];

	/**
     * 将fd注册到多路复用器上 指定监听fd的事件类型
     * <ul>
     *   <li>1 监听可读事件</li>
     *   <li>2 监听可写事件</li>
     * </ul>
	 */
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
    // 记录fd关注的事件类型
    fe->mask |= mask;
    // fd可读可写时指定回调的处理器
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    /**
     * 给回调函数使用
     */
    fe->clientData = clientData;
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

/**
 * 告知事件管理器 让其移除对fd关注的IO事件类型 关注的事件移除光了就将文件事件从事件管理器中逻辑删除
 * @param eventLoop 事件循环管理器
 * @param mask 移除什么类型的事件监听
 *             <ul>
 *               <li>1 可读事件</li>
 *               <li>2 可写事件</li>
 *             </ul>
 */
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    // 边界校验
    if (fd >= eventLoop->setsize) return;
    // 以fd为数组脚标从events数组中索引到事件体
    aeFileEvent *fe = &eventLoop->events[fd];
	// 事件状态还是初始化的状态
    if (fe->mask == AE_NONE) return;

    /* We want to always remove AE_BARRIER if set when AE_WRITABLE
     * is removed. */
    /**
     * 要移除fd上对可写IO事件的关注
     * 这行代码想了好久没有想明白 现在的猜想是
     * AE_BARRIER的标识是用来将来收到IO事件就绪通知时 干预正常的先读后写顺序为先写后读
     * 现在既然都要移除对可写事件的关注了 也就是将来并不想继续对fd进行写操作了 也就自然不必要干预读写顺序了
     */
    if (mask & AE_WRITABLE) mask |= AE_BARRIER;

	/**
	 * 让多路复用器移除监听事件状态
	 * <ul>
	 *   <li>1 移除对可读事件的监听</li>
	 *   <li>2 移除对可写事件的监听</li>
	 * </ul>
	 */
    aeApiDelEvent(eventLoop, fd, mask);
	/**
	 * 更新对fd关注的事件类型
	 * <ul>
	 *   <li>关注IO的可读</li>
	 *   <li>关注IO的可写</li>
	 *   <li>关注IO的读写顺序 强制先写后读的顺序</li>
	 * <ul>
	 */
    fe->mask = fe->mask & (~mask);
	// 更新最大的fd 一旦fd上监听着的事件都被移除光了 那么fd在逻辑上也就被删除了
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* Update the max fd */
        int j;
        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}

/**
 * 注册在多路复用器上监听着的事件类型是什么
 * @param eventLoop 事件管理器
 * @return 监听着的事件状态 要监听fd的什么事件类型
 *         <ul>
 *           <li>1 可读事件</li>
 *           <li>2 可写事件</li>
 *         </ul>
 */
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}

/**
 * 注册一个定时任务到eventLoop中
 * @param milliseconds 期望定时任务在多久之后被调度执行 毫秒
 * @param proc 事件处理器 定义了定时任务被调度起来之后如何执行 回调函数
 * @param clientData 回调函数执行的时候可能需要一些数据 可以通过这样的方式传递
 * @param finalizerProc 定时任务的的析构处理器 用于回收资源
 * @return 事件的id -1标识失败
 */
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
    // 给定时任务分配id
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    // 定时任务的id
    te->id = id;
    // 记录定时任务理论应该被调度执行的时间
    te->when = getMonotonicUs() + milliseconds * 1000;
    // 定时任务处理器
    te->timeProc = proc;
    // 定时任务的析构处理器
    te->finalizerProc = finalizerProc;
    // 回到函数执行的时候可能需要一些数据
    te->clientData = clientData;
    te->prev = NULL;
    /**
     * 定时任务头插到timeEventHead双链表上
     * 由此可见redis对于定时任务的管理很简单
     * 将来采用的查找策略也只能是轮询
     * 变向说明redis中管理的定时任务基数很小
     */
    te->next = eventLoop->timeEventHead;
    te->refcount = 0;
    if (te->next)
        te->next->prev = te;
    eventLoop->timeEventHead = te;
    return id;
}

/**
 * 根据定时任务的唯一id 将该定时任务从事件管理器中移除
 * 删除是逻辑删除 将待删除的事件id置为特定标记 id标识为-1
 * 等待以后轮询使用定时任务的时候扫描到id是-1的再进行删除操作
 * @param id 定时任务id 定时任务的唯一标识
 * @return 操作状态码
 *         <ul>
 *           <li>0 标识成功</li>
 *           <li>-1 标识失败</li>
 *         </ul>
 */
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
    // 链表
    aeTimeEvent *te = eventLoop->timeEventHead;
    // 轮询链表 找到要删除的目标节点
    while(te) {
        if (te->id == id) {
		    // 打上删除标记即可 真正的移除操作延迟到使用的时候
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* How many microseconds until the first timer should fire.
 * If there are no timers, -1 is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
/**
 * 考察定时任务中最早可以被调度的某个定时任务 还要多久
 * 这种一般都是在某个阻塞点之前
 * 先计算出最早可以调度的定时任务 比如需要等待n时间
 * 比如阻塞点是多路复用器的poll系统调用 让阻塞点以timeout的方式执行
 * 这样即使poll的唤醒最坏情况是阻塞n 唤醒之后调度起来待执行的定时任务
 * 最大化地利用了cpu资源
 * @return 还有多久(单位微秒)待执行定时任务
 *         <ul>
 *           <li>-1 标识没有定时任务待执行</li>
 *           <li>x 标识某个定时任务等待x微秒待调度</li>
 *         </ul>
 */
static int64_t usUntilEarliestTimer(aeEventLoop *eventLoop) {
    // 定时任务的链表
    aeTimeEvent *te = eventLoop->timeEventHead;
    // 链表为空的情况
    if (te == NULL) return -1;

    // 轮询链表 找到链表节点中最早要被调度执行的定时任务
    aeTimeEvent *earliest = NULL;
    while (te) {
        if (!earliest || te->when < earliest->when)
            earliest = te;
        te = te->next;
    }

    monotime now = getMonotonicUs();
    return (now >= earliest->when) ? 0 : earliest->when - now;
}

/* Process time events */
/**
 * 调度定时任务的执行
 * <ul>
 *   <li>执行物理删除定时任务的时机</li>
 *   <li>调度可以执行的定时任务<ul>
 *     <li>一次性定时任务执行完打上逻辑删除标识</li>
 *     <li>周期性定时任务执行完更新下一次调度时机</li>
 *   </ul>
 *   </li>
 * </ul>
 * @return 在这一轮处理中调度起来的定时任务数量
 */
static int processTimeEvents(aeEventLoop *eventLoop) {
    // 统计调度了多少个定时任务执行
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;

    // 链表
    te = eventLoop->timeEventHead;
	/**
	 * 当前定时任务的最大id
	 * 在遍历链表的时候发现链表节点上挂着的定时任务id大于了maxId 就说明这个节点是在轮询动作之后新增的
	 * maxId相当于在链表轮询之前给链表数据状况打了个快照
	 * 这种场景是不可能发生的 因为链表的挂载方式是头插法 即使有新增的链表节点加进来 也是加在了头节点之前 不可能遇到
	 */
    maxId = eventLoop->timeEventNextId-1;
    monotime now = getMonotonicUs();
    while(te) {
        long long id;

        /* Remove events scheduled for deletion. */
		/**
		 * 惰性删除的体现
		 * 当初提供删除定时任务的api并没有执行物理删除 仅仅是把id打成了-1标识 逻辑删除
		 * 现在这个场景遍历使用定时任务 发现id为-1的判定为删除状态的节点 进行真正的删除判定
		 */
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            /* If a reference exists for this timer event,
             * don't free it. This is currently incremented
             * for recursive timerProc calls */
			/**
			 * 这个引用计数是该定时任务正在被调度执行的次数 也就是定时任务还在运行中 不能删除
			 * 继续把删除动作延迟 放到以后的某个时机再去删除
			 */
            if (te->refcount) {
                te = next;
                continue;
            }
            // 经典的从双链表上删除某个节点 删除te节点
            if (te->prev)
                te->prev->next = te->next;
            else
                eventLoop->timeEventHead = te->next;
            if (te->next)
                te->next->prev = te->prev;
			// 准备回收te 执行回调 定制化处理析构逻辑
            if (te->finalizerProc) {
                te->finalizerProc(eventLoop, te->clientData);
                now = getMonotonicUs();
            }
            zfree(te);
            te = next;
            continue;
        }

        /* Make sure we don't process time events created by time events in
         * this iteration. Note that this check is currently useless: we always
         * add new timers on the head, however if we change the implementation
         * detail, this check may be useful again: we keep it here for future
         * defense. */
		/**
		 * 对于正常的定时任务 就是那些id!=-1的
		 * 在遍历这个链表之前已经给id的上限打了个快照maxId 也就是说在遍历过程中不可能遇到某个节点的id是>maxId的
		 * 这个地方单纯低防御性编程
		 */
        if (te->id > maxId) {
            te = te->next;
            continue;
        }

		/**
		 * 找到可以被调度的定时任务
		 */
        if (te->when <= now) {
            int retval;

            id = te->id;
			// 标识定时任务正在执行中 事件被调度执行的次数
            te->refcount++;
			/**
			 * 回调函数 调度执行定时任务
			 * 执行完毕之后根据回调函数的返回值界定该定时任务是不是周期性定时任务
			 * <ul>
			 *   <li>-1 标识一次性事件</li>
			 *   <li></li>
			 * </ul>
			 */
            retval = te->timeProc(eventLoop, id, te->clientData);
			// 释放计数 让将来的物理删除事件得以正常进行下去
            te->refcount--;
			// 更新调度计数
            processed++;
            now = getMonotonicUs();
			/**
			 * 周期性定时任务要更新下一次调度时机
			 * 回调函数的返回值语义是retval秒后
			 */
            if (retval != AE_NOMORE) {
                te->when = now + retval * 1000;
            } else {
			    // 一次性定时任务执行完毕后打上逻辑删除标识 等着下一次执行物理删除的时机
                te->id = AE_DELETED_EVENT_ID;
            }
        }
        te = te->next;
    }
    return processed;
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * the events that's possible to process without to wait are processed.
 * if flags has AE_CALL_AFTER_SLEEP set, the aftersleep callback is called.
 * if flags has AE_CALL_BEFORE_SLEEP set, the beforesleep callback is called.
 *
 * The function returns the number of events processed. */
 /**
  * 调度任务
  * <ul>既包含(网络)IO任务也包含定时任务
  *   <li>IO事件托管给系统多路复用器</li>
  *   <li>定时任务自己维护调度策略</li>
  * </ul>
  * @param flags 调度策略 看位信息
  *              <ul>
  *                <li>1 调度IO任务</li>
  *                <li>2 调度定时任务</li>
  *                <li>3 调度IO任务和定时任务</li>
  *                <li>4 非阻塞式调用多路复用器</li>
  *                <li>8 在多路复用器阻塞前执行回调</li>
  *                <li>16 在多路复用器阻塞唤醒后执行回调</li>
  *              </ul>
  * @return 总共调度了多少个任务
  *         <ul>
  *           <li>IO任务</li>
  *           <li>定时任务</li>
  *         </ul>
  */
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    /**
     * 事件管理器eventLoop中只管理有2种类型的任务
     * <ul>
     *   <li>IO任务</li>
     *   <li>定时任务 又分为一次性定时任务和周期性定时任务</li>
     * </ul>
     * 参数校验需要调度IO任务还是定时任务
     */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want to call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
	/**
	 * 这个条件判断是什么意思
	 * <ul>
	 *   <li>IO任务的maxfd!=-1意味着eventLoop管理者IO任务</li>
	 *   <li>调度策略指定<ul>
	 *     <li>需要调度定时任务</li>
	 *     <li>需要利用多路复用器的超时阻塞机制</li></ul>
	 *   </li>
	 * </ul>
	 * 也就是说多路复用器的机制和能力被用于
	 * <ul>
	 *   <li>注册IO事件到系统多路复用器上 多路复用器管理和告知用户就绪事件</li>
	 *   <li>利用多路复用器的超时阻塞机制实现精准定时功能</li>
	 * </ul>
	 * 这个if判断就是看看是不是需要使用多路复用器
	 */
    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        struct timeval tv, *tvp;
        int64_t usUntilTimer = -1;

		/**
		 * 调度策略
		 * <ul>
		 *   <li>需要调度IO任务</li>
		 *   <li>需要多路复用器的超时阻塞功能</li>
		 * </ul>
		 * 计算出最近一次的定时任务需要被调度的时机 最大化执行效率
		 * 最坏的情况就是 多路复用器没有就绪的事件 一直到超时时间才从阻塞中唤醒 然后这个时机无缝衔接开始调度普通任务
		 */
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            usUntilTimer = usUntilEarliestTimer(eventLoop);

        if (usUntilTimer >= 0) {
            tv.tv_sec = usUntilTimer / 1000000;
            tv.tv_usec = usUntilTimer % 1000000;
            tvp = &tv;
        } else {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }
		// 多路复用器poll的timeout是0
        if (eventLoop->flags & AE_DONT_WAIT) {
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        }

		// 多路复用器poll调用之前 执行回调的时机
        if (eventLoop->beforesleep != NULL && flags & AE_CALL_BEFORE_SLEEP)
            eventLoop->beforesleep(eventLoop);

        /* Call the multiplexing API, will return only on timeout or when
         * some event fires. */
        /**
         * 发起多路复用器的poll调用
         * 根据tvp超时标识实现阻塞与否以及控制超时时间是多久
         * <ul>
         *   <li>tvp是null 标识阻塞式调用 直到有就绪事件</li>
         *   <li>tvp是0 相当于立马返回 非阻塞式调用</li>
         *   <li>tvp非0 阻塞相应时间</li>
         * </ul>
         * 这样设计的根因在于兼顾定时任务的处理 提高整个系统的吞吐
         */
        numevents = aeApiPoll(eventLoop, tvp);

        /* After sleep callback. */
		// 多路复用器poll调用之后 执行回调的时机
        if (eventLoop->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP)
            eventLoop->aftersleep(eventLoop);

        for (j = 0; j < numevents; j++) {
		    // 就绪的事件 当初注册在eventLoop时的IO任务
		    int fd = eventLoop->fired[j].fd;
            aeFileEvent *fe = &eventLoop->events[fd];
			/**
			 * IO任务的就绪状态
			 * <ul>
			 *   <li>1 可读</li>
			 *   <li>2 可写</li>
			 * </ul>
			 */
            int mask = eventLoop->fired[j].mask;
            // 计数被调度执行IO任务
            int fired = 0; /* Number of events fired for current fd. */

            /* Normally we execute the readable event first, and the writable
             * event later. This is useful as sometimes we may be able
             * to serve the reply of a query immediately after processing the
             * query.
             *
             * However if AE_BARRIER is set in the mask, our application is
             * asking us to do the reverse: never fire the writable event
             * after the readable. In such a case, we invert the calls.
             * This is useful when, for instance, we want to do things
             * in the beforeSleep() hook, like fsyncing a file to disk,
             * before replying to a client. */
            /**
             * 当初注册任务事件的时候指定了监听的事件类型
             * 对于系统的多路复用器而言 只有可读可写
             * <ul>
             *   <li>关注可读事件</li>
             *   <li>关注可写事件</li>
             * </ul>
             * 但是redis在实现的是为了某些场景的高性能 对客户端暴露了指定先写后读的顺序
             * 正常的读写顺序是先读后写
             * 客户端可以通过AE_BARRIER标识指定先写后读
             * 下面即将对就绪的fd进行读写操作 因此要先判断好读写顺序
             *
             * 比较巧妙的设计
             * 对于读写顺序而言 要么是先写后读 要么是先读后写
             * 以下代码编排的就很优雅
             * 不是通过
             * if(先读后写)
             * {
             *     read();
             *     write();
             * }
             * else
             * {
             *     write();
             *     read();
             * }
             * 而是直接将写fd的逻辑固定在中间 再将读fd的逻辑固定在前后 然后通过if条件是走前面的读逻辑还是后面的读逻辑
             */
            int invert = fe->mask & AE_BARRIER;

            /* Note the "fe->mask & mask & ..." code: maybe an already
             * processed event removed an element that fired and we still
             * didn't processed, so we check if the event is still valid.
             *
             * Fire the readable event if the call sequence is not
             * inverted. */
			// 先读后写的顺序
            if (!invert && fe->mask & mask & AE_READABLE) {
			    // 回调读fd的函数
                fe->rfileProc(eventLoop,fd,fe->clientData,mask); // 回调执行 可读
                fired++;
                fe = &eventLoop->events[fd]; /* Refresh in case of resize. */
            }

            /* Fire the writable event. */
            if (fe->mask & mask & AE_WRITABLE) {
                if (!fired || fe->wfileProc != fe->rfileProc) {
				    // 回调写fd的函数
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            /* If we have to invert the call, fire the readable event now
             * after the writable one. */
			// 先写后读的顺序
            if (invert) {
                fe = &eventLoop->events[fd]; /* Refresh in case of resize. */
                if ((fe->mask & mask & AE_READABLE) &&
                    (!fired || fe->wfileProc != fe->rfileProc))
                {
				    // 回调读fd的函数
                    fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            processed++;
        }
    }
    /* Check time events */
	/**
	 * 调度策略指定了需要调度定时任务
	 */
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

/**
 * eventLoop启动
 */
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
		/**
		 * 调度策略
		 * <ul>
		 *   <li>需要调度执行的任务类型 IO任务和普通任务</li>
		 *   <li>在多路复用器阻塞前执行回调函数</li>
		 *   <li>在多路复用器从阻塞中唤醒后执行回调函数</li>
		 * </ul>
		 */
        aeProcessEvents(eventLoop, AE_ALL_EVENTS|
                                   AE_CALL_BEFORE_SLEEP|
                                   AE_CALL_AFTER_SLEEP);
    }
}

/**
 * 对多路复用器进行了跨平台的封装
 * <ul>
 *   <li>mac的kqueue</li>
 *   <li>linux的epoll</li>
 * </ul>
 */
char *aeGetApiName(void) {
    return aeApiName();
}

/**
 * setter函数 指定回调函数
 */
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

/**
 * setter函数 指定回调函数
 */
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}
