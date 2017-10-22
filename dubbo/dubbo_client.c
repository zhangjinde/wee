#include <stdbool.h>
#include <sys/socket.h>
#include <errno.h>
#include <assert.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "dubbo_codec.h"
#include "dubbo_client.h"

#include "../ae/ae.h"
#include "../net/socket.h"
#include "../base/buffer.h"
#include "../base/cJSON.h"
#include "../base/mq.h"
#include "../base/dbg.h"

#define CLI_INIT_BUF_SZ 1024

struct dubbo_client
{
    struct aeEventLoop *el;
    struct dubbo_args *args;
    union sockaddr_all addr;

    struct buffer *rcv_buf;
    struct buffer *snd_buf;
    int pipe_n;
    int pipe_left;
    int req_n;
    int req_left;

    int fd;
    bool connected;

    struct timeval start;
    struct timeval end;
};


static void cli_on_connect(struct aeEventLoop *el, int fd, void *ud, int mask);
static void cli_on_read(struct aeEventLoop *el, int fd, void *ud, int mask);
static void cli_on_write(struct aeEventLoop *el, int fd, void *ud, int mask);
static bool cli_decode_resp_frombuf(struct buffer *buf);
static void cli_pipe_send(struct dubbo_client *cli);

static struct buffer *cli_encode_req(struct dubbo_client *cli)
{
    struct dubbo_req *req = dubbo_req_create(cli->args->service, cli->args->method, cli->args->args, cli->args->attach);
    if (req == NULL)
    {
        return false;
    }
    return dubbo_encode(req);
}

#define cli_decode_resp(cli) cli_decode_resp_frombuf(((struct dubbo_client *)(cli))->rcv_buf)

static void cli_reset(struct dubbo_client *cli)
{
    cli->connected = false;
    cli->fd = -1;
    cli->pipe_left = cli->pipe_n;
    buf_retrieveAll(cli->rcv_buf);
    buf_retrieveAll(cli->snd_buf);
}

static struct dubbo_client *cli_create(struct dubbo_args *args, struct dubbo_async_args *async_args)
{
    struct dubbo_client *cli = calloc(1, sizeof(*cli));
    assert(cli);
    cli->el = async_args->el;

    cli->rcv_buf = buf_create(CLI_INIT_BUF_SZ);
    cli->snd_buf = buf_create(CLI_INIT_BUF_SZ);
    cli->pipe_n = async_args->pipe_n;
    cli->pipe_left = async_args->pipe_n;
    cli->req_n = async_args->req_n;
    cli->req_left = async_args->req_n;
    cli->args = args;
    cli_reset(cli);

    if (!sa_resolve(cli->args->host, &cli->addr))
    {
        PANIC("%s DNS解析失败", cli->args->host);
    }
    cli->addr.v4.sin_port = (in_port_t)atoi(cli->args->port);
    return cli;
}

static bool cli_connected(struct dubbo_client *cli)
{
    cli->connected = true;
    if (AE_ERR == aeCreateFileEvent(cli->el, cli->fd, AE_READABLE, cli_on_read, cli))
    {
        return false;
    }
    cli_pipe_send(cli);
    return true;
}

bool cli_connect(struct dubbo_client *cli)
{
    int fd = socket_create();
    if (fd < 0)
    {
        return false;
    }
    cli->fd = fd;

    int status = socket_connect(fd, &cli->addr, sizeof(cli->addr.s));
    if (status == 0)
    {
        if (!cli_connected(cli))
        {
            goto close;
        }
    }
    else
    {
        if (errno == EINPROGRESS)
        {
            // 这里 貌似应该 read + write
            if (AE_ERR == aeCreateFileEvent(cli->el, fd, AE_WRITABLE, cli_on_connect, cli))
            {
                goto close;
            }
        }
        else
        {
            goto close;
        }
    }
    return true;

close:
    close(fd);
    return false;
}

static void cli_close(struct dubbo_client *cli)
{
    aeDeleteFileEvent(cli->el, cli->fd, AE_READABLE | AE_WRITABLE);
    close(cli->fd);
    cli_reset(cli);
}

static void cli_release(struct dubbo_client *cli)
{
    buf_release(cli->rcv_buf);
    buf_release(cli->snd_buf);
    free(cli);
}

static bool cli_start(struct dubbo_client *cli)
{
    gettimeofday(&cli->start, NULL);
    return cli_connect(cli);
}

// fixme at exit
static void cli_end(struct dubbo_client *cli)
{
    cli_close(cli);
    gettimeofday(&cli->end, NULL);
    // fixme qps
    aeStop(cli->el);
    cli_release(cli);
}

static void cli_reconnect(struct dubbo_client *cli)
{
    cli_close(cli);
    if (!cli_connect(cli))
    {
        LOG_ERROR("重连失败");
    }
}

static bool cli_write(struct dubbo_client *cli)
{
    struct buffer *buf = cli->snd_buf;
    if (!buf_readable(buf))
    {
        aeDeleteFileEvent(cli->el, cli->fd, AE_WRITABLE);
        return true;
    }

    int nwritten = 0;
    while (buf_readable(buf))
    {
        nwritten = write(cli->fd, buf_peek(buf), buf_readable(buf));
        if (nwritten <= 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        buf_retrieve(buf, nwritten);
    }

    if (nwritten <= 0)
    {
        if (errno == EAGAIN)
        {
            if (AE_ERR == aeCreateFileEvent(cli->el, cli->fd, AE_WRITABLE, cli_on_write, cli))
            {
                LOG_ERROR("Dubbo 请求失败: 创建可写事件失败");
                return false;
            }
            return true;
        }
        else
        {
            LOG_ERROR("Dubbo 发送数据失败: %s", strerror(errno));
            return false;
        }
    }

    if (!buf_readable(buf))
    {
        aeDeleteFileEvent(cli->el, cli->fd, AE_WRITABLE);
    }
    return true;
}

static void cli_send_req(struct dubbo_client *cli)
{
    struct buffer *buf = cli_encode_req(cli);
    if (buf == NULL)
    {
        PANIC("Dubbo 请求失败: 编码失败");
        return;
    }

    buf_append(cli->snd_buf, buf_peek(buf), buf_readable(buf));
    buf_release(buf);
    if (!cli_write(cli))
    {
        cli_reconnect(cli);
    }
}

static void cli_pipe_send(struct dubbo_client *cli)
{
    while (cli->pipe_left--)
    {
        cli_send_req(cli);
    }
}

static void cli_on_connect(struct aeEventLoop *el, int fd, void *ud, int mask)
{
    struct dubbo_client *cli = (struct dubbo_client *)ud;
    // 注意: ae 将 err 与 hup 转换成 write 事件
    if ((mask & AE_WRITABLE)/* && !(mask & AE_READABLE)*/)
    {
        aeDeleteFileEvent(el, fd, AE_WRITABLE/* | AE_READABLE*/ );
        // 所以, 可能出错 !!!
        cli_connected(cli);
    }
    else
    {
        LOG_ERROR("连接失败: %s", strerror(errno));
        cli_reconnect(cli);
    }
}

static void cli_on_write(struct aeEventLoop *el, int fd, void *ud, int mask)
{
    UNUSED(el);
    UNUSED(fd);
    UNUSED(mask);

    struct dubbo_client *cli = (struct dubbo_client *)ud;
    if (!cli_write(cli))
    {
        cli_reconnect(cli);
    }
}

static void cli_on_read(struct aeEventLoop *el, int fd, void *ud, int mask)
{
    UNUSED(el);
    UNUSED(mask);

    struct dubbo_client *cli = (struct dubbo_client *)ud;
    assert(cli->connected);

    for (;;)
    {
        int errno_ = 0;
        ssize_t recv_n = buf_readFd(cli->rcv_buf, fd, &errno_);
        if (recv_n < 0)
        {
            if (errno_ == EINTR)
            {
                continue;
            }
            else if (errno_ == EAGAIN)
            {
            }
            else
            {
                LOG_ERROR("从 Dubbo 服务端读取数据: %s", strerror(errno));
                cli_reconnect(cli);
                return;
            }
        }
        else if (recv_n == 0)
        {
            LOG_ERROR("Dubbo 服务端断开连接");
            cli_reconnect(cli);
            return;
        }
        break;
    }

    if (!is_dubbo_pkt(cli->rcv_buf))
    {
        LOG_ERROR("接收到非 dubbo 数据包");
        cli_reconnect(cli);
    }

    if (!is_completed_dubbo_pkt(cli->rcv_buf, NULL))
    {
        return;
    }

    cli->pipe_left++;
    cli->req_left--;
    bool ok = cli_decode_resp(cli);
    if (cli->req_left <= 0)
    {
        cli_end(cli);
    }
    else
    {
        if (ok)
        {
            cli_pipe_send(cli);
        }
        else
        {
            cli_reconnect(cli);
        }
    }
}

static bool cli_decode_resp_frombuf(struct buffer *buf)
{
    struct dubbo_res *res = dubbo_decode(buf);
    if (res == NULL)
    {
        return false;
    }

    if (res->is_evt)
    {
        LOG_INFO("接收到 dubbo 事件包");
    }
    else if (res->data_sz)
    {
        if (res->data[0] == '[' || res->data[0] == '{')
        {
            // 返回 json, 不应该有 NULL 存在, 且非 NULL 结尾
            char *json = malloc(res->data_sz + 1);
            assert(json);
            memcpy(json, res->data, res->data_sz);
            json[res->data_sz] = '\0';

            cJSON *resp = cJSON_Parse(json);
            if (resp)
            {
                if (res->ok)
                {
                    printf("\x1B[1;32m%s\x1B[0m\n", cJSON_Print(resp));
                }
                else
                {
                    printf("\x1B[1;31m%s\x1B[0m\n", res->desc);
                    printf("\x1B[1;31m%s\x1B[0m\n", cJSON_Print(resp));
                }
                cJSON_Delete(resp);
            }
            else
            {
                printf("\x1B[1;32m%s\x1B[0m\n", res->data);
            }
            free(json);
        }
        else
        {
            printf("\x1B[1;32m%s\x1B[0m\n", res->data);
        }
    }

    dubbo_res_release(res);
    return true;
}

bool dubbo_bench_async(struct dubbo_args *args, struct dubbo_async_args *async_args)
{
    struct dubbo_client *cli = cli_create(args, async_args);
    if (cli == NULL)
    {
        return false;
    }
    return cli_start(cli);
}

bool dubbo_invoke_sync(struct dubbo_args *args)
{
    bool ret = false;

    struct dubbo_req *req = dubbo_req_create(args->service, args->method, args->args, args->attach);
    if (req == NULL)
    {
        return false;
    }

    struct buffer *buf = dubbo_encode(req);
    if (!buf)
    {
        dubbo_req_release(req);
        return false;
    }

    int sockfd = socket_clientSync(args->host, args->port);
    if (sockfd == -1)
    {
        goto release;
    }

    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &args->timeout, sizeof(struct timeval));
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &args->timeout, sizeof(struct timeval));

    size_t send_n = socket_sendAllSync(sockfd, buf_peek(buf), buf_readable(buf));
    if (send_n != buf_readable(buf))
    {
        goto release;
    }

    // fucking swoole, reactor 对EPOLLRDHUP事件直接当error处理，关闭, 不能正确处理版关闭事件
    // socket_shutdownWrite(sockfd);

    // reset buffer
    buf_retrieveAll(buf);

    int errno_ = 0;
    for (;;)
    {
        ssize_t recv_n = buf_readFd(buf, sockfd, &errno_);
        if (recv_n < 0)
        {
            if (errno_ == EINTR)
            {
                continue;
            }
            else
            {
                perror("ERROR readv");
                goto release;
            }
        }
        else if (!is_dubbo_pkt(buf))
        {
            goto release;
        }
        break;
    }

    int remaining = 0;
    if (!is_completed_dubbo_pkt(buf, &remaining))
    {
        goto release;
    }

    while (remaining < 0)
    {
        ssize_t recv_n = buf_readFd(buf, sockfd, &errno_);
        if (recv_n < 0)
        {
            if (errno_ == EINTR)
            {
                continue;
            }
            else
            {
                perror("ERROR receiving");
                goto release;
            }
        }
        remaining += recv_n;
    }

    cli_decode_resp_frombuf(buf);

// fimxe print attach

release:
    dubbo_req_release(req);
    buf_release(buf);
    if (sockfd != -1)
    {
        close(sockfd);
    }
    return ret;
}