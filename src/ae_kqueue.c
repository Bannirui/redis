/* Kqueue(2)-based ae.c module
 *
 * Copyright (C) 2009 Harish Mallipeddi - harish.mallipeddi@gmail.com
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


#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

// 可以理解成redis对不同OS平台的多路复用的抽象 借此屏蔽各个OS系统实现上的差异性
typedef struct aeApiState {
    int kqfd;
	/**
     * kevent系统调用之后就绪的fd存放的数组
     * redis为了交互使用方便再将这个数组加工拷贝到fired数组
	 */
    struct kevent *events;

    /* Events mask for merge read and write event.
     * To reduce memory consumption, we use 2 bits to store the mask
     * of an event, so that 1 byte will store the mask of 4 events. */
    /**
     * 复制使用的中转数组 因此要格外关注复制之后的fd事件状态置回
     * char数组 跟上面的events数组对应
     * 存储事件就绪的状态
     * kqueue通过kevent这个系统调用返回的events就绪事件数组中的就绪状态是单一的 也就是说某个事件要么可读 要么可写
     * 对应关系是 一个kevent对应一个char的2个bit 这2个bit用来保存kevent的就绪状态是可读还是可写
     * 也就是说每4个fd占用1个byte来保存可读可写状态
     */
    char *eventsMask; 
} aeApiState;

/**
 * 1个fd占用2个bit 4个fd占用1个byte
 * sz个fd需要占用多大的空间
 * @param sz fd的数量
 */
#define EVENT_MASK_MALLOC_SIZE(sz) (((sz) + 3) / 4)
// 每个fd的事件状态占用2bit 1个byte可以容纳4个 找到要把fd的事件状态放到byte的哪个位置
#define EVENT_MASK_OFFSET(fd) ((fd) % 4 * 2)
// 0x03是二进制11 把mask的低2bit取出来 放到byte正确的位置上
#define EVENT_MASK_ENCODE(fd, mask) (((mask) & 0x3) << EVENT_MASK_OFFSET(fd))

// 从eventsMask中转数组中把fd事件状态读出来
static inline int getEventMask(const char *eventsMask, int fd) {
    return (eventsMask[fd/4] >> EVENT_MASK_OFFSET(fd)) & 0x3;
}

/**
 * 将fd的可读还是可写事件状态编排到eventMask数组中
 * @param eventsMask 就绪事件状态数组 每个fd占用2个bit
 * @param fd 事件体
 * @param mask 事件状态枚举值
 *             <ul>
 *               <li>1 标识事件可读</li>
 *               <li>2 标识事件可写</li>
 *             </ul>
 */
static inline void addEventMask(char *eventsMask, int fd, int mask) {
    /**
     * 每个fd的事件状态本来是可以从系统的多路复用器分会结果看出来 但是这样的话就得先从events数组根据脚标定位到event事件然后再根据event的filter确认是可读事件还是可写事件
     * 这个事件复杂度并不友好
     * 因此建立map映射关系优化到O(1)事件复杂度
     * 因为可读事件状态枚举值是0x01 可写事件状态枚举值是0x02 最多占用2bit
     * 将map的空间复杂度压下来
     * 每个fd的状态标识用2bit表达 内存最小单位是byte 数组item是byte 将其编排容纳4个fd事件状态
     */
    eventsMask[fd/4] |= EVENT_MASK_ENCODE(fd, mask);
}

/**
 * 将fd的就绪事件状态映射在byte数组上 这个eventsMask数组仅仅是拷贝的中转数组
 * 复制使用完之后要把之前设置上去的事件状态移除
 * 所谓的移除方式就是与上一个按位反的值
 */
static inline void resetEventMask(char *eventsMask, int fd) {
    eventsMask[fd/4] &= ~EVENT_MASK_ENCODE(fd, 0x3);
}

/**
 * 创建多路复用器
 * 将事件循环器与系统的多路复用器关联上
 * <ul>
 *   <li>linux的epoll</li>
 *   <li>mac的kqueue</li>
 * </ul>
 */
static int aeApiCreate(aeEventLoop *eventLoop) {
    // 内存开辟 多路复用器实例
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
	// 数组创建
    state->events = zmalloc(sizeof(struct kevent)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    // kqueue实例 mac系统多路复用实现
    state->kqfd = kqueue();
    if (state->kqfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
	// 设置kqueue的fd标志位
    anetCloexec(state->kqfd);
	// eventsMask中转数组的实例化
    state->eventsMask = zmalloc(EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));
	// eventsMask中转数组置0初始值
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));
    eventLoop->apidata = state;
    return 0;
}

/**
 * 重置多路复用器的就绪事件数组大小 使用场景是扩大容量
 * @param setsize 期待盛放就绪事件的数组容量
 * @return 状态码 <ul>
 *                 <li>0 标识成功</li>
 *                 <li>非0 标识失败</li>
 *               </ul>
 */
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

	// 系统多路复用器返回原声就绪事件存放的数组
    state->events = zrealloc(state->events, sizeof(struct kevent)*setsize);
	// redis封装的临时复制中转就绪事件状态的数组
    state->eventsMask = zrealloc(state->eventsMask, EVENT_MASK_MALLOC_SIZE(setsize));
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(setsize));
    return 0;
}

/**
 * 释放多路复用器
 */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->kqfd);
    zfree(state->events);
    zfree(state->eventsMask);
    zfree(state);
}

/**
 * 向ae中kq添加对事件的监听
 * @param eventLoop 事件循环器
 * @param fd 要监听谁的状态
 * @param mask 要监听fd的什么状态 也就是要监听的事件类型
 * @return 状态标识 <ul>
 *                   <li>0 标识成功</li>
 *                   <li>-1 标识失败</li>
 *                 </ul>
 */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    // 多路复用器的封装
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

	/**
	 * 下面是2个if分支
	 * 说明支持的特性是对于同一个fd 既可以让多路复用器关注它的可读状态 又可以关注它的可写状态
	 * 但是当通过kevent系统调用获取已经就绪事件集合时 返回出来的就绪事件要么是读事件 要么是写事件
	 * 也就是说监听事件的过滤器可以放多个 既可以监听fd的读事件 也可以监听fd的写事件
	 * 但是监听的结果只有一个 要么是读事件 要么是写事件 不可能既是可读又是可写
	 */
	// 为fd添加可读过滤器 把fd注册到kq里面 让kq去监听fd的可读状态 读事件
    if (mask & AE_READABLE) {
	    // 这个宏的作用就相当于setter函数
        EV_SET(&ke, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
		// kevent函数向kq注册时间ke
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    // 为fd添加可写过滤器 把fd注册到kq里面 让kq去监听fd的可写状态 写事件
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    return 0;
}

/**
 * 从ae中kq移除对事件的监听
 * @param eventLoop 事件循环器
 * @param fd 谁的状态
 * @param mask 要移除对fd什么状态的监听
 * @return 状态标识 <ul>
 *                   <li>0 标识成功</li>
 *                   <li>-1 标识失败</li>
 *                 </ul>
 */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    // ae中的多路复用器
    aeApiState *state = eventLoop->apidata;
    struct kevent ke;

	// 要移除fd的可读状态监听 不监听读事件
    if (mask & AE_READABLE) {
	    // kevent的setter方法
        EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		// 向kqueue中注册事件
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
	// 要移除fd的可写状态监听 不监听写事件
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
}

/**
 * IO复用器发生一次阻塞式系统调用 看看哪些fd就绪了 就绪的fd以fd为脚标放在了fired就绪数组中了
 * @param eventLoop 事件管理器
 * @param tvp 阻塞时长
 *            <ul>
 *              <li>tvp是NULL 说明不带timeout 就一直阻塞等待系统多路复用器的响应</li>
 *              <li>tvp不是NULL 说明带timeout地发起系统调用</li>
 *            </ul>
 * @return 就绪的fd数量
 */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    // 多路复用器kqueue
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    if (tvp != NULL) {
	    // 带超时系统调用
        struct timespec timeout;
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
		// kevent调用 让kqueue把注册在其中的事件就绪的fd集合放到events数组中 retval用于标识多少个fd状态就绪
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,
                        &timeout);
    } else {
	    // 阻塞式调用 直到结果有结果返回
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,
                        NULL);
    }

    if (retval > 0) {
	    // kqueue已经告知了状态就绪的fd
        int j;

        /* Normally we execute the read event first and then the write event.
         * When the barrier is set, we will do it reverse.
         * 
         * However, under kqueue, read and write events would be separate
         * events, which would make it impossible to control the order of
         * reads and writes. So we store the event's mask we've got and merge
         * the same fd events later. */
        // 遍历所有就绪的fd 把fd就绪状态事件类型整合到2个bit上 保存在eventsMask上
        for (j = 0; j < retval; j++) {
		    // 就绪的fd放在了events数组中 轮询出来
            struct kevent *e = state->events+j;
            int fd = e->ident;
            int mask = 0; 

            if (e->filter == EVFILT_READ) mask = AE_READABLE;
            else if (e->filter == EVFILT_WRITE) mask = AE_WRITABLE;
            // 将fd的可读还是可写状态用2bit标识 编排到eventMask数组中
            addEventMask(state->eventsMask, fd, mask);
        }

        /* Re-traversal to merge read and write events, and set the fd's mask to
         * 0 so that events are not added again when the fd is encountered again. */
        numevents = 0;
        for (j = 0; j < retval; j++) {
            struct kevent *e = state->events+j;
            int fd = e->ident;
			// fd的事件状态
            int mask = getEventMask(state->eventsMask, fd);
			// 有就绪状态的事件都放到就绪事件数组fired中
            if (mask) {
                eventLoop->fired[numevents].fd = fd;
                eventLoop->fired[numevents].mask = mask;
				/**
				 * 数据的复制链路是OS多路复用器结果->eventsMask数组->fired数组
				 * 相当于eventMask数组仅仅是中转 拷贝使用完之后将事件状态重置回去 不要污染下一次系统调用kevent时机的拷贝
				 */
                resetEventMask(state->eventsMask, fd);
                numevents++;
            }
        }
    }
    return numevents;
}

static char *aeApiName(void) {
    return "kqueue";
}
