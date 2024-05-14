/* anet.c -- Basic TCP socket stuff made a bit less boring
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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "anet.h"

static void anetSetError(char *err, const char *fmt, ...)
{
    va_list ap;

    if (!err) return;
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

/**
 * 设置socket的阻塞模式
 * 设置成阻塞或者非阻塞的
 * @param fd socket的fd
 * @param non_block 想要把fd设置成什么阻塞模式
 *                  <ul>
 *                    <li>非0 想要socket是非阻塞的</li>
 *                    <li>0 想要socket是阻塞的</li>
 *                  </ul>
 */
int anetSetBlock(char *err, int fd, int non_block) {
    int flags;

    /* Set the socket blocking (if non_block is zero) or non-blocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
	/**
	 * 获取socket的fd状态标志
	 */
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        anetSetError(err, "fcntl(F_GETFL): %s", strerror(errno));
        return ANET_ERR;
    }

    /* Check if this flag has been set or unset, if so, 
     * then there is no need to call fcntl to set/unset it again. */
	/**
	 * 判定fd的阻塞状态 已经是想要的效果了就ASAP地退出
	 */
    if (!!(flags & O_NONBLOCK) == !!non_block)
        return ANET_OK;

	/**
	 * <ul>
	 *   <li>想要socket是非阻塞的 就把描述符状态标志低位第2位设置成1</li>
	 *   <li>想要socket是阻塞的 就把描述符状态标志低位第2位设置成0</li>
	 * </ul>
	 * 然后再把新的描述符状态标志设置给socket
	 */
    if (non_block)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;

	// 设置新的描述符状态标志给socket
    if (fcntl(fd, F_SETFL, flags) == -1) {
        anetSetError(err, "fcntl(F_SETFL,O_NONBLOCK): %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/**
 * 将socket设置为非阻塞模式
 */
int anetNonBlock(char *err, int fd) {
    /**
     * 设置socket的阻塞模式
     * <ul>
     *   <li>0 设置成非阻塞模式</li>
     *   <li>1 设置成非阻塞模式</li>
     * </ul>
     */
    return anetSetBlock(err,fd,1);
}

/**
 * 将socket设置为阻塞模式
 */
int anetBlock(char *err, int fd) {
    return anetSetBlock(err,fd,0);
}

/* Enable the FD_CLOEXEC on the given fd to avoid fd leaks. 
 * This function should be invoked for fd's on specific places 
 * where fork + execve system calls are called. */
/**
 * 在fd上设置close-on-exec标志
 * 作用是一个文件描述符fd被标记为FD_CLOEXEC时 当进程通过exec系列函数(比如execve()和execvp())执行新程序时 该fd会被自动关闭
 * @return fcntl系统调用的返回值
 */
int anetCloexec(int fd) {
    int r;
    int flags;

    do {
	    // 读取socket的fd标志
        r = fcntl(fd, F_GETFD);
    } while (r == -1 && errno == EINTR);

	// 看看标志位上是不是已经有了FD_CLOEXEC标志
    if (r == -1 || (r & FD_CLOEXEC))
        return r;

	// 在标志上打上FD_CLOEXEC
    flags = r | FD_CLOEXEC;

    do {
	    // 将新的socket描述符标志设置给socket
        r = fcntl(fd, F_SETFD, flags);
    } while (r == -1 && errno == EINTR);

    return r;
}

/* Set TCP keep alive option to detect dead peers. The interval option
 * is only used for Linux as we are using Linux-specific APIs to set
 * the probe send time, interval, and count. */
/**
 * 设置socket的属性
 * @param interval 开启keepalive后 设置探测连接是否存活的报文时间间隔 多少秒
 * @return
 */
int anetKeepAlive(char *err, int fd, int interval)
{
    int val = 1;

	// 开启socket的keepalive功能
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1)
    {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
        return ANET_ERR;
    }

#ifdef __linux__
    /* Default settings are more or less garbage, with the keepalive time
     * set to 7200 by default on Linux. Modify settings to make the feature
     * actually useful. */

    /* Send first probe after interval. */
    val = interval;
	/**
	 * TCP_KEEPIDLE是用于设置TCP的keepalive开始发送探测报文之前的空闲时间的选项
	 * 一旦TCP的keepalive功能被启用 系统将会在连接空闲一段时间后开始发送探测报文以检测连接的存活性
	 * TCP_KEEPIDLE选项允许指定在开始发送探测报文之前允许的最大空闲时间
	 */
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPIDLE: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Send next probes after the specified interval. Note that we set the
     * delay as interval / 3, as we send three probes before detecting
     * an error (see the next setsockopt call). */
    val = interval/3;
    if (val == 0) val = 1;
	/**
	 * TCP_KEEPINTVL是用于设置TCP的keepalive探测间隔的选项
	 * 一旦TCP的keepalive功能被启用 系统将会定期发送探测报文以检测连接的存活性
	 * TCP_KEEPINTVL选项允许指定探测报文之间的时间间隔
	 */
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPINTVL: %s\n", strerror(errno));
        return ANET_ERR;
    }

    /* Consider the socket in error state after three we send three ACK
     * probes without getting a reply. */
    val = 3;
	/**
	 * 用于设置TCP的keepalive探测尝试次数的选项
	 * 当TCP的keepalive功能被启用时 系统将会定期发送探测报文以检测连接的存活性
	 * TCP_KEEPCNT选项允许指定在关闭连接之前允许的最大探测失败次数
	 */
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
        anetSetError(err, "setsockopt TCP_KEEPCNT: %s\n", strerror(errno));
        return ANET_ERR;
    }
#else
	// 防止编译器warning
    ((void) interval); /* Avoid unused var warning for non Linux systems. */
#endif

    return ANET_OK;
}

/**
 * 设置是否开启Nagle算法
 * @param val 开启还是关闭延迟算法 1表示关闭延迟算法 0表示不关闭延迟算法
 */
static int anetSetTcpNoDelay(char *err, int fd, int val)
{
    /**
     * TCP_NODELAY是用于设置TCP的Nagle算法的选项
     * Nagle算法是一种优化TCP传输的算法 它通过在发送数据时进行缓冲 将多个小数据包合并成一个大数据包 从而减少网络上的数据包数量 提高传输效率
     * 在某些情况下 例如实时通信或者需要快速响应的应用中 这种缓冲可能会引入延迟，因此需要禁用Nagle算法
     */
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == -1)
    {
        anetSetError(err, "setsockopt TCP_NODELAY: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

// 关闭TCP的Nagle延迟算法
int anetEnableTcpNoDelay(char *err, int fd)
{
    // 关闭Nagle延迟算法
    return anetSetTcpNoDelay(err, fd, 1);
}

// 开启TCP的Nagle延迟算法
int anetDisableTcpNoDelay(char *err, int fd)
{
    // 开启Nagle延迟算法
    return anetSetTcpNoDelay(err, fd, 0);
}

/* Set the socket send timeout (SO_SNDTIMEO socket option) to the specified
 * number of milliseconds, or disable it if the 'ms' argument is zero. */
/**
 * 设置socket的发送超时时间
 * @param ms 超时时间设置多少毫秒
 */
int anetSendTimeout(char *err, int fd, long long ms) {
    struct timeval tv;

    tv.tv_sec = ms/1000;
    tv.tv_usec = (ms%1000)*1000;
	/**
	 * SO_SNDTIMEO是用于设置发送操作超时时间的选项
	 * 它允许设置在发送数据时等待的最大时间 如果在此时间内无法完成发送操作 则发送操作将被中断并返回错误
	 */
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1) {
        anetSetError(err, "setsockopt SO_SNDTIMEO: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/* Set the socket receive timeout (SO_RCVTIMEO socket option) to the specified
 * number of milliseconds, or disable it if the 'ms' argument is zero. */
/**
 * socket在接收数据时最大的等待时长
 * @param ms 等待时长 毫秒
 */
int anetRecvTimeout(char *err, int fd, long long ms) {
    struct timeval tv;

    tv.tv_sec = ms/1000;
    tv.tv_usec = (ms%1000)*1000;
	/**
	 * SO_RCVTIMEO选项用于设置接收操作的超时时间
	 * 它允许指定在接收数据时等待的最大时间 如果在此时间内未接收到数据 则接收操作将被中断并返回错误
	 */
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
        anetSetError(err, "setsockopt SO_RCVTIMEO: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/* Resolve the hostname "host" and set the string representation of the
 * IP address into the buffer pointed by "ipbuf".
 *
 * If flags is set to ANET_IP_ONLY the function only resolves hostnames
 * that are actually already IPv4 or IPv6 addresses. This turns the function
 * into a validating / normalizing function. */
/**
 * 主机名解析成ip地址
 * 将二进制格式转换为字符串格式
 * @param err 上抛异常信息
 * @param host 要解析的主机名
 * @param ipbuf 解析出来的ip地址结果是字符串格式 放到这个char数组里面
 * @param ipbuf_len char数组的长度
 * @param flags
 * @return <ul>状态码
 *           <li>-1 失败</li>
 *           <li>0 成功</li>
 *         </ul>
 */
int anetResolve(char *err, char *host, char *ipbuf, size_t ipbuf_len,
                       int flags)
{
    struct addrinfo hints, *info;
    int rv;

	// 提供解析提示
    memset(&hints,0,sizeof(hints));
    if (flags & ANET_IP_ONLY) hints.ai_flags = AI_NUMERICHOST;
	// 协议族 适用于IPv4或者IPv6
    hints.ai_family = AF_UNSPEC;
	// 套接字类型是TCP套接字
    hints.ai_socktype = SOCK_STREAM;  /* specify socktype to avoid dups */

    if ((rv = getaddrinfo(host, NULL, &hints, &info)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    if (info->ai_family == AF_INET) {
	    // 解析结果的协议族是IPv4
        struct sockaddr_in *sa = (struct sockaddr_in *)info->ai_addr;
		// IPv4类型的ip地址放到ipbuf上
        inet_ntop(AF_INET, &(sa->sin_addr), ipbuf, ipbuf_len);
    } else {
	    // 解析结果的协议族是IPv6
        struct sockaddr_in6 *sa = (struct sockaddr_in6 *)info->ai_addr;
		// IPv6类型的ip地址放到ipbuf上
        inet_ntop(AF_INET6, &(sa->sin6_addr), ipbuf, ipbuf_len);
    }

    freeaddrinfo(info);
    return ANET_OK;
}

/**
 * 设置套接字重用
 */
static int anetSetReuseAddr(char *err, int fd) {
    int yes = 1;
    /* Make sure connection-intensive things like the redis benchmark
     * will be able to close/open sockets a zillion of times */
	/**
	 * SO_REUSEADDR是一个套接字选项 用于设置套接字的重用地址属性 它允许在关闭套接字之后立即重新绑定到相同的端口 即使先前的套接字连接依然存在
	 * 这个选项通常用于服务器程序 可以在服务器重启后快速重新启动 无需等待一段时间以使操作系统释放先前绑定的端口
	 *
	 * 需要5个参数
	 * <ul>
	 *   <li>sockfd 套接字文件描述符</li>
	 *   <li>level 通常为SOL_SOCKET 表示设置的套接字级别的选项</li>
	 *   <li>optname 要设置的选项名称</li>
	 *   <li>optval 指向选项值的指针</li>
	 *   <li>optlen optval指向的值的大小</li>
	 * </ul>
	 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt SO_REUSEADDR: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/**
 * 创建TCP套接字
 * 并且将其设置socket端口重用
 * @param domain 指定socket的协议族 <ul>
 *                                  <li>AF_INET 表示IPv4地址族</li>
 *                                  <li>AF_INET6 表示IPv6地址族</li>
 *                                  <li>AF_LOCAL 表示本地(Unix域)套接字 用于在同一台计算机上不同进程间进行本地通信 而不通过网络 因为不需要经过网络协议栈 因此开销小 速度快</li>
 *                                </ul>
 * @return <ul>
 *           <li>-1 标识错误码</li>
 *           <li>非-1 表示socket的fd</li>
 *         </ul>
 */
static int anetCreateSocket(char *err, int domain) {
    int s;
	/**
	 * 系统调用创建socket实例
	 * <ul>
	 *   <li>domain 指定通信的地址族<ul>
	 *     <li>AF_INET 表示IPv4地址族</li>
	 *     <li>AF_INET6 表示IPv6地址族</li>
	 *     <li>AF_LOCAL 表示本地(Unix域)套接字 用于在同一台计算机上不同进程间进行本地通信 而不通过网络 因为不需要经过网络协议栈 因此开销小 速度快</li>
	 *   </ul></li>
	 *   <li>type 指定套接字类型<ul>
	 *     <li>SOCK_STREAM表示面向连接的流套接字</li>
	 *     <li>SOCK_DGRAM表示无连接的数据报套接字</li>
	 *   </ul></li>
	 *   <li>protocol 使用的协议 通常使用默认的协议 即0</li>
	 * </ul>
	 */
    if ((s = socket(domain, SOCK_STREAM, 0)) == -1) {
        anetSetError(err, "creating socket: %s", strerror(errno));
        return ANET_ERR;
    }

    /* Make sure connection-intensive things like the redis benchmark
     * will be able to close/open sockets a zillion of times */
	// 这是reids的服务端 将socket设置为端口重用
    if (anetSetReuseAddr(err,s) == ANET_ERR) {
        close(s);
        return ANET_ERR;
    }
    return s;
}

#define ANET_CONNECT_NONE 0
#define ANET_CONNECT_NONBLOCK 1
#define ANET_CONNECT_BE_BINDING 2 /* Best effort binding. */
/**
 * 创建服务端tcp
 * 这个方法妙就妙在source_addr和flag这两个参数
 * <ul>
 *   <li>source_add
 *     <ul>
 *       <li>没传source_add 那么套接字就压根不会去bind 那么就是一个标准的主动socket 也就是用在客户端上</li>
 *       <li>传了source_add 就会将socket尽力地进行bind操作 可以用在服务端上</li>
 *     </ul>
 *   </li>
 *   <li>flag 套接字的各种能力属性全靠这个标识来指定</li>
 * </ul>
 * 能力项全靠flags标识来指定
 * @param err 上抛的异常信息
 * @param addr 主机名(比如www.baidu.com或者localhost)
 * @param port 服务名(比如http或者80)
 * @param source_addr 如果指定了地址 就让socket去bind到这个地址上
 *                    这个方法
 * @param flags socket的能力标识 比如指定socket是非阻塞模式
 * @return <ul>状态标识
 *           <li>-1 失败</li>
 *           <li>0 成功</li>
 *         </ul>
 */
static int anetTcpGenericConnect(char *err, const char *addr, int port,
                                 const char *source_addr, int flags)
{
    int s = ANET_ERR, rv;
    char portstr[6];  /* strlen("65535") + 1; */
    struct addrinfo hints, *servinfo, *bservinfo, *p, *b;

    snprintf(portstr,sizeof(portstr),"%d",port);
	// 提供解析提示
    memset(&hints,0,sizeof(hints));
	// 协议族 适用于IPv4和IPv6
    hints.ai_family = AF_UNSPEC;
	// 套接字类型 TCP套接字
    hints.ai_socktype = SOCK_STREAM;

	/**
	 * 解析主机名和服务名 解析成一个或者多个的套接字地址结构
	 * 主机名=计算机名+域名
	 * getaddrinfo形参4个
	 * <ul>
	 *   <li>hostname 主机名或者ip地址</li>
	 *   <li>servername 服务名或者端口号</li>
	 *   <li>hints 解析提示</li>
	 *   <li>serverinfo 该库函数解析出来的结果 数组结构</li>
	 * </ul>
	 *
	 * 举2个参数例子
	 * <ul>
	 *   <li>hostname是www.baidu.com servername是http</li>
	 *   <li>hostname是localhost     servername是8080</li>
	 * </ul>
	 *
	 * 解析模板指定了解析出来的套接字地址结构
	 * <ul>
	 *   <li>地址族 网络通信地址族 </li>
	 *   <li>套接字类型 面向连接的面向流的套接字</li>
	 * </ul>
	 */
    if ((rv = getaddrinfo(addr,portstr,&hints,&servinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
	/**
	 * 遍历解析结果 拿着解析出来的套接字地址结构
	 * <ul>
	 *   <li>对于客户端而言 创建socket 连接解析出来的地址</li>
	 *   <li>对于服务端而言 创建socket 绑定到解析出来的地址上</li>
	 * </ul>
	 */
    for (p = servinfo; p != NULL; p = p->ai_next) {
        /* Try to create the socket and to connect it.
         * If we fail in the socket() call, or on connect(), we retry with
         * the next entry in servinfo. */
		/**
		 * 创建TCP的socket套接字实例
		 * <ul>
	     *   <li>domain 指定通信的地址族<ul>
	     *     <li>AF_INET 表示IPv4地址族</li>
	     *     <li>AF_INET6 表示IPv6地址族</li>
	     *     <li>AF_LOCAL 表示本地(Unix域)套接字 用于在同一台计算机上不同进程间进行本地通信 而不通过网络 因为不需要经过网络协议栈 因此开销小 速度快</li>
	     *   </ul></li>
	     *   <li>type 指定套接字类型<ul>
	     *     <li>SOCK_STREAM表示面向连接的流套接字</li>
	     *     <li>SOCK_DGRAM表示无连接的数据报套接字</li>
	     *   </ul></li>
	     *   <li>protocol 使用的协议 通常使用默认的协议 即0</li>
		 * </ul>
		 * 因为getaddrinfo()解析方法指定了解析提示 通信地址族是网络通信 套接字类型是面向连接的流套接字
		 * 也就是TCP套接字 这里创建的要么是IPv4的要么是IPv6的
		 */
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;
		// 设置TCP套接字重用端口
        if (anetSetReuseAddr(err,s) == ANET_ERR) goto error;
		// 设置TCP套接字非阻塞模式
        if (flags & ANET_CONNECT_NONBLOCK && anetNonBlock(err,s) != ANET_OK)
            goto error;
		// 指定了bind地址
        if (source_addr) {
            int bound = 0;
            /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
            if ((rv = getaddrinfo(source_addr, NULL, &hints, &bservinfo)) != 0)
            {
                anetSetError(err, "%s", gai_strerror(rv));
                goto error;
            }
            for (b = bservinfo; b != NULL; b = b->ai_next) {
                if (bind(s,b->ai_addr,b->ai_addrlen) != -1) {
                    bound = 1;
                    break;
                }
            }
            freeaddrinfo(bservinfo);
            if (!bound) {
                anetSetError(err, "bind: %s", strerror(errno));
                goto error;
            }
        }
		/**
		 * 这个地方很巧妙 一举多得
		 * <ul>
		 *   <li>首先如果是指定了bind地址的 此刻socket已经bind到了指定地址 那么通过自己连自己的方式可以有效测试服务端socket的状况</li>
		 *   <li>其次 如果没有显式指定bind的地址 那么此时仅仅是创建了一个socket实例 通过connect自己连自己这样的方式 因为此时socket还没bind 所以系统会随机分配一个没有使用的端口让socket绑定上</li>
		 * </ul>
		 */
        if (connect(s,p->ai_addr,p->ai_addrlen) == -1) {
            /* If the socket is non-blocking, it is ok for connect() to
             * return an EINPROGRESS error here. */
            if (errno == EINPROGRESS && flags & ANET_CONNECT_NONBLOCK)
                goto end;
            close(s);
            s = ANET_ERR;
            continue;
        }

        /* If we ended an iteration of the for loop without errors, we
         * have a connected socket. Let's return to the caller. */
        goto end;
    }
    if (p == NULL)
        anetSetError(err, "creating socket: %s", strerror(errno));

error:
    if (s != ANET_ERR) {
        close(s);
        s = ANET_ERR;
    }

end:
    // 释放解析结果
    freeaddrinfo(servinfo);

    /* Handle best effort binding: if a binding address was used, but it is
     * not possible to create a socket, try again without a binding address. */
	/**
	 * 这个地方设计也是比较巧妙的
	 * socket没有创建成功的原因
	 * <ul>
	 *   <li>socket系统调用失败</li>
	 *   <li>bind系统调用失败</li>
	 *   <li>connect系统调用失败 因为没有bind成功导致的connect失败</li>
	 * </ul>
	 * 无论哪种失败 都没有达到创建服务端socket的预期
	 * 如果入参指定了bind的地址 那么再给一次机会 用随机的bind地址
	 * 整体上增加创建服务端socket的成功率
	 */
    if (s == ANET_ERR && source_addr && (flags & ANET_CONNECT_BE_BINDING)) {
        return anetTcpGenericConnect(err,addr,port,NULL,flags);
    } else {
        return s;
    }
}

/**
 * 非阻塞TCP套接字 服务端socket
 * bing在了系统随机分配的端口上
 */
int anetTcpNonBlockConnect(char *err, const char *addr, int port)
{
    /**
     * 新建的套接字bind到随机分配的端口
     * 指定套接字非阻塞
     */
    return anetTcpGenericConnect(err,addr,port,NULL,ANET_CONNECT_NONBLOCK);
}

/**
 * 非阻塞TCP套接字 服务端socket
 * bind在了指定的端口上
 */
int anetTcpNonBlockBestEffortBindConnect(char *err, const char *addr, int port,
                                         const char *source_addr)
{
    /**
     * 新建的套接字要bind到source_addr上
     * 指定套接字非阻塞
     */
    return anetTcpGenericConnect(err,addr,port,source_addr,
            ANET_CONNECT_NONBLOCK|ANET_CONNECT_BE_BINDING);
}

int anetUnixGenericConnect(char *err, const char *path, int flags)
{
    int s;
    struct sockaddr_un sa;

    /**
     * 创建Unix域的socket用于本地通信
     */
    if ((s = anetCreateSocket(err,AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

	// unix域的本地连接
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (flags & ANET_CONNECT_NONBLOCK) {
	    // 设置socket非阻塞
        if (anetNonBlock(err,s) != ANET_OK) {
            close(s);
            return ANET_ERR;
        }
    }
	/**
	 * 让s这个套接字连接到服务器的套接字
	 * <ul>
	 *   <li>s 客户端的套接字</li>
	 *   <li>sa sockaddr结构体的指针 包含着服务端地址和端口信息</li>
	 *   <li>sa大小</li>
	 * </ul>
	 */
    if (connect(s,(struct sockaddr*)&sa,sizeof(sa)) == -1) {
        if (errno == EINPROGRESS &&
            flags & ANET_CONNECT_NONBLOCK)
            return s;

        anetSetError(err, "connect: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return s;
}

/**
 * 包含2个步骤
 * <ul>
 *   <li>bind将服务端套接字绑定到固定的地址和端口上 方便服务端程序稳定的监听来自客户端的请求连接</li>
 *   <li>调用listen将套接字从主动套接字转换为被动套接字 并设置缓冲队列大小 方便接收来自客户端的请求</li>
 * </ul>
 * @param err 负责将系统调用的错误码传递出去
 * @param s 服务端套接字
 * @param sa 服务端套接字要绑定的地址端口信息
 * @param len sa的sockaddr结构体大小
 * @param backlog listen等待队列大小
 * @return 响应状态码 -1标识失败 0标识成功
 */
static int anetListen(char *err, int s, struct sockaddr *sa, socklen_t len, int backlog) {
	/**
     * socket系列系统调用 绑定到特定的地址和端口
     * bind系统调用对服务端程序尤为重要
     * 服务器需要在一个固定的地址和端口上监听客户端的请求连接
	 */
    if (bind(s,sa,len) == -1) {
        anetSetError(err, "bind: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }

	/**
     * listen系统调用 将主动socket转换为被动socket
     * 以便接收来自客户端的请求
     * backlog指定挂起连接队列的最大长度 未处理的连接请求将保存在这个队列中 直到用accept系统调用进行处理
	 */
    if (listen(s, backlog) == -1) {
        anetSetError(err, "listen: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return ANET_OK;
}

static int anetV6Only(char *err, int s) {
    int yes = 1;
    if (setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,&yes,sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt: %s", strerror(errno));
        return ANET_ERR;
    }
    return ANET_OK;
}

/**
 * 新建服务端TCP套接字
 * 套接字调用过了bind和listen系统调用
 * @param port 套接字bind的端口
 * @param bindaddr 计算机名或服务 解析出来给套接字使用 套接字bind的地址信息
 * @param af 协议族 网络连接 由调用方指定是IPv4或者IPv6
 * @param backlog listen的缓冲队列大小
 * @return 套接字的fd -1标识失败
 */
static int _anetTcpServer(char *err, int port, char *bindaddr, int af, int backlog)
{
    int s = -1, rv;
    char _port[6];  /* strlen("65535") */
    struct addrinfo hints, *servinfo, *p;

    snprintf(_port,6,"%d",port);
	/**
	 * 解析提示
	 * <ul>
	 *   <li>套接字协议族是网络 调用方指定是IPv4还是IPv6</li>
	 *   <li>套接字类型是TCP连接</li>
	 *   <li></li>
	 *   <li></li>
	 * </ul>
	 */
    memset(&hints,0,sizeof(hints));
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    /* No effect if bindaddr != NULL */
    if (bindaddr && !strcmp("*", bindaddr))
        bindaddr = NULL;
    if (af == AF_INET6 && bindaddr && !strcmp("::*", bindaddr))
        bindaddr = NULL;

	// 解析计算机名或服务到serverinfo 用于给套接字分配地址信息
    if ((rv = getaddrinfo(bindaddr,_port,&hints,&servinfo)) != 0) {
        anetSetError(err, "%s", gai_strerror(rv));
        return ANET_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        // socket系统调用创建TCP套接字 由入参指定是IPv4还是IPv6
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;

        if (af == AF_INET6 && anetV6Only(err,s) == ANET_ERR) goto error;
		// 设置套接字重用功能
        if (anetSetReuseAddr(err,s) == ANET_ERR) goto error;
        // bind到指定上 并调用listen转换为被动socket 以便接收来自客户端的连接请求
        if (anetListen(err,s,p->ai_addr,p->ai_addrlen,backlog) == ANET_ERR) s = ANET_ERR;
        goto end;
    }
    if (p == NULL) {
        anetSetError(err, "unable to bind socket, errno: %d", errno);
        goto error;
    }

error:
    if (s != -1) close(s);
    s = ANET_ERR;
end:
    freeaddrinfo(servinfo);
    return s;
}

/**
 * 新建服务端IPv4 TCP套接字
 * @param port 监听的端口
 * @param bindaddr 监听的地址
 * @param backlog 监听的缓冲队列
 * @return 套接字fd -1标识失败
 */
int anetTcpServer(char *err, int port, char *bindaddr, int backlog)
{
    // IPv4的服务端TCP套接字
    return _anetTcpServer(err, port, bindaddr, AF_INET, backlog);
}

/**
 * 新建服务端IPv6 TCP套接字
 * @param port 监听的端口
 * @param bindaddr 监听的地址
 * @param backlog 监听的缓冲队列
 * @return 套接字fd -1标识失败
 */
int anetTcp6Server(char *err, int port, char *bindaddr, int backlog)
{
    // IPv6的TCP连接
    return _anetTcpServer(err, port, bindaddr, AF_INET6, backlog);
}

/**
 * 新建服务端Unix域本地套接字
 * @param path 监听的地址端口信息
 * @param perm 指定的文件权限 在类unix系统中万物皆文件 就可以设置文件权限
 * @param backlog 服务端套接字的缓冲队列
 * @return 套接字fd -1标识失败
 */
int anetUnixServer(char *err, char *path, mode_t perm, int backlog)
{
    int s;
    struct sockaddr_un sa;

	/**
	 * 创建Unix域的socket用于本地通信
	 */
    if ((s = anetCreateSocket(err,AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

    memset(&sa,0,sizeof(sa));
	// 套接字协议族是Unix本地协议
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
	// 监听在path指定的地址端口上
    if (anetListen(err,s,(struct sockaddr*)&sa,sizeof(sa),backlog) == ANET_ERR)
        return ANET_ERR;
	// 设置文件权限
    if (perm)
        chmod(sa.sun_path, perm);
    return s;
}

/**
 * accept系统调用接受请求 返回与客户端进行通信的套接字fd
 * @param err
 * @param s 服务端套接字
 * @param sa 用于存储客户端socket的地址信息
 *           NULL时表示不需要存储客户端的地址信息
 * @param len sa的大小
 * @return 与客户端套接字进行通信的新的套接字
 */
static int anetGenericAccept(char *err, int s, struct sockaddr *sa, socklen_t *len) {
    int fd;
    while(1) {
	    /**
	     * 接受连接请求，并在网络服务器编程中扮演关键角色
         * 它从监听套接字队列中获取一个待处理的连接，并返回一个新的套接字文件描述符，用于与客户端进行通信
         * <ul>
         *   <li>成功时，返回新的套接字文件描述符，用于与客户端进行通信</li>
         *   <li>失败时，返回-1，并设置errno以指示错误</li>
         * </ul>
	     */
        fd = accept(s,sa,len);
        if (fd == -1) {
            if (errno == EINTR)
                continue;
            else {
                anetSetError(err, "accept: %s", strerror(errno));
                return ANET_ERR;
            }
        }
        break;
    }
    return fd;
}

/**
 * 接受TCP连接的套接字
 * @param s 服务端套接字
 * @param ip 存储客户端套接字地址信息
 * @param ip_len 数据结构大小
 * @param port 存储客户端套接字端口
 * @return 新建出来跟客户端通信的套接字
 */
int anetTcpAccept(char *err, int s, char *ip, size_t ip_len, int *port) {
    int fd;
	// 用来存储接受到的客户端请求的套接字地址端口信息
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err,s,(struct sockaddr*)&sa,&salen)) == -1)
        return ANET_ERR;

	// TCP网络通信 接受到的客户端套接字请求要么是IPv4的要么是IPv6的
    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) inet_ntop(AF_INET,(void*)&(s->sin_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) inet_ntop(AF_INET6,(void*)&(s->sin6_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin6_port);
    }
	// 跟客户端套接字通信的新的套接字
    return fd;
}

/**
 * 接受Unix域本地连接的请求
 * @param s 服务端套接字
 * @return 跟客户端套接字通信的新的套接字
 */
int anetUnixAccept(char *err, int s) {
    int fd;
	// 接受到的连接过来的客户端套接字地址端口信息存储在这
    struct sockaddr_un sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err,s,(struct sockaddr*)&sa,&salen)) == -1)
        return ANET_ERR;

    return fd;
}

int anetFdToString(int fd, char *ip, size_t ip_len, int *port, int fd_to_str_type) {
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);

    if (fd_to_str_type == FD_TO_PEER_NAME) {
        if (getpeername(fd, (struct sockaddr *)&sa, &salen) == -1) goto error;
    } else {
        if (getsockname(fd, (struct sockaddr *)&sa, &salen) == -1) goto error;
    }
    if (ip_len == 0) goto error;

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) inet_ntop(AF_INET,(void*)&(s->sin_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else if (sa.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) inet_ntop(AF_INET6,(void*)&(s->sin6_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin6_port);
    } else if (sa.ss_family == AF_UNIX) {
        if (ip) snprintf(ip, ip_len, "/unixsocket");
        if (port) *port = 0;
    } else {
        goto error;
    }
    return 0;

error:
    if (ip) {
        if (ip_len >= 2) {
            ip[0] = '?';
            ip[1] = '\0';
        } else if (ip_len == 1) {
            ip[0] = '\0';
        }
    }
    if (port) *port = 0;
    return -1;
}

/* Format an IP,port pair into something easy to parse. If IP is IPv6
 * (matches for ":"), the ip is surrounded by []. IP and port are just
 * separated by colons. This the standard to display addresses within Redis. */
int anetFormatAddr(char *buf, size_t buf_len, char *ip, int port) {
    return snprintf(buf,buf_len, strchr(ip,':') ?
           "[%s]:%d" : "%s:%d", ip, port);
}

/* Like anetFormatAddr() but extract ip and port from the socket's peer/sockname. */
int anetFormatFdAddr(int fd, char *buf, size_t buf_len, int fd_to_str_type) {
    char ip[INET6_ADDRSTRLEN];
    int port;

    anetFdToString(fd,ip,sizeof(ip),&port,fd_to_str_type);
    return anetFormatAddr(buf, buf_len, ip, port);
}
