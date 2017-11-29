#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <string.h>
#include <assert.h>
#include "mysql_sniff.h"
#include "../net/sniff.h"
#include "../base/buffer.h"
#include "../base/endian.h"
#include "../base/queue.h"

#if !defined(UNUSED)
#define UNUSED(x) ((void)(x))
#endif

// 每个端口挂一个 struct conn(单向连接) 链表
#define PORT_QUEUE_SIZE 65535
static QUEUE PORT_QUEUE[PORT_QUEUE_SIZE];

static int16_t mysql_server_port;
static struct mysql_conn_data g_conn_data;

struct conn
{
    uint32_t ip;   // 网络字节序
    uint16_t port; // 本机字节序
    struct buffer *buf;
    QUEUE node;
};

static struct conn *conn_create(uint32_t ip, uint16_t port)
{
    struct conn *c = malloc(sizeof(*c));
    if (c == NULL)
        return NULL;
    memset(c, 0, sizeof(*c));
    c->ip = ip;
    c->port = port;
    c->buf = buf_create(8192);
    return c;
}

static void conn_release(struct conn *c)
{
    assert(c);
    buf_release(c->buf);
    free(c);
}

static void pq_init()
{
    int i = 0;
    for (; i < PORT_QUEUE_SIZE; i++)
    {
        QUEUE_INIT(&PORT_QUEUE[i]);
    }
}

static void pq_release()
{
    QUEUE *q;
    QUEUE *el;
    struct conn *c;
    int i = 0;

    for (; i < PORT_QUEUE_SIZE; i++)
    {
        q = &PORT_QUEUE[i];
        if (QUEUE_EMPTY(q))
        {
            continue;
        }

        QUEUE_FOREACH(el, q)
        {
            c = QUEUE_DATA(el, struct conn, node);
            conn_release(c);
        }
    }
}

static void pq_dump()
{
    QUEUE *q;
    QUEUE *el;
    struct conn *c;
    int i = 0;
    char ip_buf[INET_ADDRSTRLEN];

    int bytes = 0;
    for (; i < PORT_QUEUE_SIZE; i++)
    {
        q = &PORT_QUEUE[i];
        if (QUEUE_EMPTY(q))
        {
            continue;
        }

        QUEUE_FOREACH(el, q)
        {
            c = QUEUE_DATA(el, struct conn, node);
            inet_ntop(AF_INET, &c->ip, ip_buf, INET_ADDRSTRLEN);
            printf("%s:%d %zu\n", ip_buf, c->port, buf_readable(c->buf));
            bytes += buf_internalCapacity(c->buf);
        }
        printf("bytes: %d\n", bytes);
    }
}

static struct conn *pq_get_internal(uint32_t ip, uint16_t port, int is_remove)
{
    QUEUE *q = &PORT_QUEUE[port];
    if (QUEUE_EMPTY(q))
    {
        return NULL;
    }

    QUEUE *el;
    struct conn *c;
    QUEUE_FOREACH(el, q)
    {
        c = QUEUE_DATA(el, struct conn, node);
        if (c->ip == ip)
        {
            if (is_remove)
            {
                QUEUE_REMOVE(el);
            }
            return c;
        }
    }
    return NULL;
}

static struct conn *pq_get(uint32_t ip, uint16_t port)
{
    return pq_get_internal(ip, port, 0);
}

struct conn *pq_del(uint32_t ip, uint16_t port)
{
    return pq_get_internal(ip, port, 1);
}

void pq_add(struct conn *c)
{
    QUEUE *q = &PORT_QUEUE[c->port];
    if (pq_get(c->ip, c->port) == NULL)
    {
        QUEUE_INSERT_TAIL(q, &c->node);
    }
    else
    {
        assert(false);
    }
}


// malloc free 全部修改为 栈内存.... 反正不需要保存, 直接打印到控制台好了
static int buf_strsize(struct buffer *buf);
static uint64_t buf_readFLE(struct buffer* buf, uint64_t *len, uint8_t *is_null);
static int buf_peekFLELen(struct buffer* buf);
static int buf_readFLEStr(struct buffer* buf, char** str);

static uint32_t get_mysql_pdu_len(struct buffer *buf);
static void mysql_dissect_compressed_header(struct buffer *buf);
bool is_completed_mysql_pdu(struct buffer *buf);

static void mysql_dissect_auth_switch_response(struct buffer *buf, mysql_conn_data_t *conn_data);
static void mysql_dissect_error_packet(struct buffer *buf);
static void mysql_set_conn_state(mysql_conn_data_t *conn_data, mysql_state_t state);
static void mysql_dissect_greeting(struct buffer *buf, mysql_conn_data_t *conn_data);
static void mysql_dissect_login(struct buffer *buf, mysql_conn_data_t *conn_data);
static int add_connattrs_entry_to_tree(struct buffer *buf);
static void mysql_dissect_request(struct buffer *buf, mysql_conn_data_t *conn_data);
static void mysql_dissect_response(struct buffer *buf, mysql_conn_data_t *conn_data);
static void mysql_dissect_result_header(struct buffer *buf, mysql_conn_data_t *conn_data);
static void mysql_dissect_ok_packet(struct buffer *buf, mysql_conn_data_t *conn_data);
static void mysql_dissect_field_packet(struct buffer* buf, mysql_conn_data_t *conn_data);

void pkt_handle(void *ud,
                const struct pcap_pkthdr *pkt_hdr,
                const struct ip *ip_hdr,
                const struct tcphdr *tcp_hdr,
                const struct tcpopt *tcp_opt,
                const u_char *payload,
                size_t payload_size)
{
    // if (payload_size > 0)
    // {
    //     // print_bytes((char *)payload, payload_size);
    // }

    // char ip_buf[INET_ADDRSTRLEN];

    // printf("+-------------------------+\n");
    // printf("   ACK: %u\n", ntohl(tcp_hdr->th_ack));
    // printf("   SEQ: %u\n", ntohl(tcp_hdr->th_seq));

    // inet_ntop(AF_INET, &(ip_hdr->ip_dst.s_addr), ip_buf, INET_ADDRSTRLEN);
    // printf("   DST IP: %s\n", ip_buf);
    // inet_ntop(AF_INET, &(ip_hdr->ip_src.s_addr), ip_buf, INET_ADDRSTRLEN);
    // printf("   SRC IP: %s\n", ip_buf);

    // printf("   SRC PORT: %d\n", ntohs(tcp_hdr->th_sport));
    // printf("   DST PORT: %d\n", ntohs(tcp_hdr->th_dport));

    struct conn *c;
    uint32_t s_ip = ip_hdr->ip_src.s_addr;
    uint16_t s_port = ntohs(tcp_hdr->th_sport);

    static char s_ip_buf[INET_ADDRSTRLEN];
    static char d_ip_buf[INET_ADDRSTRLEN];

    // uint32_t d_ip = ip_hdr->ip_dst.s_addr;
    uint16_t d_port = ntohs(tcp_hdr->th_dport);

    inet_ntop(AF_INET, &(ip_hdr->ip_src.s_addr), s_ip_buf, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &(ip_hdr->ip_dst.s_addr), d_ip_buf, INET_ADDRSTRLEN);

    // 连接关闭, 清理数据
    if (tcp_hdr->th_flags & TH_FIN || tcp_hdr->th_flags & TH_RST)
    {
        LOG_INFO("%s:%d > %s:%d Close Connection", s_ip_buf, s_port, d_ip_buf, d_port);
        c = pq_del(s_ip, s_port);
        if (c)
        {
            conn_release(c);
        }
        c = pq_del(ip_hdr->ip_dst.s_addr, ntohs(tcp_hdr->th_dport));
        if (c)
        {
            conn_release(c);
        }
        return;
    }

    if (payload_size <= 0)
    {
        return;
    }

    // 这里假定一定是 mysql 数据 !!!
    // 获取或初始化连接对象
    c = pq_get(s_ip, s_port);
    if (c == NULL)
    {
        c = conn_create(s_ip, s_port);
        pq_add(c);
    }

    struct buffer *buf = c->buf;
    buf_append(buf, (const char *)payload, payload_size);

    if (!is_completed_mysql_pdu(buf))
    {
        return;
    }

    bool is_response = false;
    if (s_port == mysql_server_port)
    {
        is_response = true;
    }

    int32_t pkt_sz = buf_readInt32LE24(buf);
    uint8_t pkt_num = buf_readInt8(buf);
    LOG_INFO("packet size %d packet num %d", pkt_sz, pkt_num);


    // TODO 照顾逻辑 !!!!!copy 一份数据包, 存入一个新的buffer !!!!!
    // 优化, buffer_view slice 一个 只读buffer
    struct buffer * once_buf = buf_create(pkt_sz);
    buf_append(once_buf, buf_peek(buf), pkt_sz);
    buf_retrieve(buf, pkt_sz);

    // TODO 检测是否是 SSL !!!
    bool is_ssl = false;

    if (is_response)
    {
        if (pkt_num == 0 && g_conn_data.state == UNDEFINED)
        {
            LOG_INFO("%s:%d > %s:%d Server Greeting", s_ip_buf, s_port, d_ip_buf, s_port);
            mysql_dissect_greeting(once_buf, &g_conn_data);
        }
        else
        {
            LOG_INFO("%s:%d > %s:%d Response", s_ip_buf, s_port, d_ip_buf, s_port);
            mysql_dissect_response(once_buf, &g_conn_data);
        }
    }
    else
    {
        // TODO 这里 有问题, 暂时没进入该分支 !!!!, 抓取不到 login
        if (g_conn_data.state == LOGIN && (pkt_num == 1 || (pkt_num == 2 && is_ssl)))
        {
            LOG_INFO("%s:%d > %s:%d Login Request", s_ip_buf, s_port, d_ip_buf, s_port);
            mysql_dissect_login(once_buf, &g_conn_data);
            // TODO 处理 SSL
            /*
            if ((g_conn_data.srv_caps & MYSQL_CAPS_CP) && (g_conn_data.clnt_caps & MYSQL_CAPS_CP))
            {
                g_conn_data.frame_start_compressed = pinfo->num;
                g_conn_data.compressed_state = MYSQL_COMPRESS_INIT;
            }
            */
        }
        else
        {
            LOG_INFO("%s:%d > %s:%d Request", s_ip_buf, s_port, d_ip_buf, s_port);
            mysql_dissect_request(once_buf, &g_conn_data);
        }
    }

    if (buf_internalCapacity(buf) > 1024 * 1024)
    {
        buf_shrink(c->buf, 0);
    }
    buf_release(once_buf);
}

void init()
{
    pq_init();
    g_conn_data.srv_caps = 0;
    g_conn_data.clnt_caps = 0;
    g_conn_data.clnt_caps_ext = 0;
    g_conn_data.state = UNDEFINED;
    // g_conn_data.stmts =
    g_conn_data.major_version = 0;
    g_conn_data.frame_start_ssl = 0;
    g_conn_data.frame_start_compressed = 0;
    g_conn_data.compressed_state = MYSQL_COMPRESS_NONE;
}

int main(int argc, char **argv)
{
    char *device;
    if (argc < 2)
    {
        device = "lo0";
    }
    else
    {
        device = argv[1];
    }

    mysql_server_port = 3306;

    struct tcpsniff_opt opt = {
        .snaplen = 65535,
        .pkt_cnt_limit = 0,
        .timeout_limit = 10,
        // .device = device,
        .device = "lo0",
        .filter_exp = "tcp and port 3306",
        .ud = NULL};

    init();

    if (!tcpsniff(&opt, pkt_handle))
    {
        fprintf(stderr, "fail to sniff\n");
    }
    return 0;
}


static 
int buf_strsize(struct buffer *buf) {
    const char *eos = buf_findChar(buf, '\0');
    if (eos == NULL)
    {
        return buf_readable(buf);
    } else {
        return eos - buf_peek(buf);
    }
}

static uint32_t
get_mysql_pdu_len(struct buffer *buf)
{
    int total_sz = buf_readable(buf);
    uint32_t pkt_sz = buf_readInt32LE24(buf);

    if ((total_sz - pkt_sz) == 7)
    {
        return pkt_sz + 7; /* compressed header 3+1+3 (len+id+cmp_len) */
    }
    else
    {
        return pkt_sz + 4; /* regular header 3+1 (len+id) */
    }
}

/*
 * Decode the header of a compressed packet
 * https://dev.mysql.com/doc/internals/en/compressed-packet-header.html
 */
static void
mysql_dissect_compressed_header(struct buffer *buf)
{
    int32_t cmp_pkt_sz = buf_readInt32LE24(buf);
    uint8_t cmp_pkt_num = buf_readInt8(buf);
    int32_t cmp_pkt_uncmp_sz = buf_readInt32LE24(buf);

    UNUSED(cmp_pkt_sz);
    UNUSED(cmp_pkt_num);
    UNUSED(cmp_pkt_uncmp_sz);
}

// TOOD 未处理 compressed header
bool is_completed_mysql_pdu(struct buffer *buf)
{
    if (buf_readable(buf) < 4) /* regular header 3+1 (len+id) */
    {
        return false;
    }
    int32_t sz = buf_peekInt32LE24(buf);
    if (sz <= 0 || sz >= MYSQL_MAX_PACKET_LEN)
    {
        PANIC("malformed mysql packet(size=%d)\n", sz);
        return false;
    }
    return buf_readable(buf) >= sz + 4;
}

/**
Value Of     # Of Bytes  Description
First Byte   Following
----------   ----------- -----------
0-250        0           = value of first byte
251          0           column value = NULL
only appropriate in a Row Data Packet
252          2           = value of following 16-bit word
253          3           = value of following 24-bit word
254          8           = value of following 64-bit word
*/
// One may ask why the 1 byte length is limited to 251, when the first reserved value in the
// net_store_length( ) is 252. The code 251 has a special meaning. It indicates that there is
// no length value or data following the code, and the value of the field is the SQL

// field length encoded
// len     out   
// is_null out   where to store ISNULL flag, may be NULL
// return where to store FLE value, may be NULL
static uint64_t
buf_readFLE(struct buffer* buf, uint64_t *len, uint8_t *is_null)
{
	uint8_t prefix = buf_readInt8(buf);
	
	if (is_null) {
		*is_null = 0;
	}

	switch (prefix) {
	case 251:
		if (len) {
			*len = 1;
		}
		if (is_null) {
			*is_null = 1;
		}
		return 0;
	case 252:
		if (len) {
			*len = 1 + 2;
		}
		return buf_readInt16LE(buf);
	case 253:
		if (len) {
			*len = 1 + 4;
		}
		return buf_readInt32LE(buf);

		// TODO 好像有种情况是这样 !!!
		/*
		if (len) {
			*len = 1 + 3;
		}
		return buf_readInt32LE24(buf);
		*/
	case 254:
		if (len) {
			*len = 1 + 8;
		}
		return buf_readInt64LE(buf);
	default: /* < 251 */
		if (len) {
			*len = 1;
		}
		return prefix;
	}
}

static int
buf_peekFLELen(struct buffer* buf)
{
	uint8_t prefix = buf_readInt8(buf);
	
	switch (prefix) {
	case 251:
		return 1;
	case 252:
		return 1 + 2;
	case 253:
		return 1 + 4;
		// TODO
		return 1 + 3;
	case 254:
		return 1 + 8;
	default: /* < 251 */
		return 1;
	}
}


// TODO free
static int
buf_readFLEStr(struct buffer* buf, char** str) {
	uint64_t len;
	uint64_t sz = buf_readFLE(buf, &len, NULL);
	*str = buf_readStr(buf, sz);
	return len + sz;
}



/*
参考文档
https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html
https://dev.mysql.com/doc/internals/en/client-server-protocol.html
http://hutaow.com/blog/2013/11/06/mysql-protocol-analysis/
wireshare 源码

Server Status: 0x0002
.... .... .... ...0 = In transaction: Not set
.... .... .... ..1. = AUTO_COMMIT: Set
.... .... .... .0.. = More results: Not set
.... .... .... 0... = Multi query - more resultsets: Not set
.... .... ...0 .... = Bad index used: Not set
.... .... ..0. .... = No index used: Not set
.... .... .0.. .... = Cursor exists: Not set
.... .... 0... .... = Last row sent: Not set
.... ...0 .... .... = database dropped: Not set
.... ..0. .... .... = No backslash escapes: Not set
.... .0.. .... .... = Session state changed: Not set
.... 0... .... .... = Query was slow: Not set
...0 .... .... .... = PS Out Params: Not set


Server Capabilities: 0xffff
.... .... .... ...1 = Long Password: Set
.... .... .... ..1. = Found Rows: Set
.... .... .... .1.. = Long Column Flags: Set
.... .... .... 1... = Connect With Database: Set
.... .... ...1 .... = Don't Allow database.table.column: Set
.... .... ..1. .... = Can use compression protocol: Set
.... .... .1.. .... = ODBC Client: Set
.... .... 1... .... = Can Use LOAD DATA LOCAL: Set
.... ...1 .... .... = Ignore Spaces before '(': Set
.... ..1. .... .... = Speaks 4.1 protocol (new flag): Set
.... .1.. .... .... = Interactive Client: Set
.... 1... .... .... = Switch to SSL after handshake: Set
...1 .... .... .... = Ignore sigpipes: Set
..1. .... .... .... = Knows about transactions: Set
.1.. .... .... .... = Speaks 4.1 protocol (old flag): Set
1... .... .... .... = Can do 4.1 authentication: Set

Extended Server Capabilities: 0xc1ff
.... .... .... ...1 = Multiple statements: Set
.... .... .... ..1. = Multiple results: Set
.... .... .... .1.. = PS Multiple results: Set
.... .... .... 1... = Plugin Auth: Set
.... .... ...1 .... = Connect attrs: Set
.... .... ..1. .... = Plugin Auth LENENC Client Data: Set
.... .... .1.. .... = Client can handle expired passwords: Set
.... .... 1... .... = Session variable tracking: Set
.... ...1 .... .... = Deprecate EOF: Set
1100 000. .... .... = Unused: 0x60

Client Capabilities: 0x8208
.... .... .... ...0 = Long Password: Not set
.... .... .... ..0. = Found Rows: Not set
.... .... .... .0.. = Long Column Flags: Not set
.... .... .... 1... = Connect With Database: Set
.... .... ...0 .... = Don't Allow database.table.column: Not set
.... .... ..0. .... = Can use compression protocol: Not set
.... .... .0.. .... = ODBC Client: Not set
.... .... 0... .... = Can Use LOAD DATA LOCAL: Not set
.... ...0 .... .... = Ignore Spaces before '(': Not set
.... ..1. .... .... = Speaks 4.1 protocol (new flag): Set
.... .0.. .... .... = Interactive Client: Not set
.... 0... .... .... = Switch to SSL after handshake: Not set
...0 .... .... .... = Ignore sigpipes: Not set
..0. .... .... .... = Knows about transactions: Not set
.0.. .... .... .... = Speaks 4.1 protocol (old flag): Not set
1... .... .... .... = Can do 4.1 authentication: Set

Extended Client Capabilities: 0x0008
.... .... .... ...0 = Multiple statements: Not set
.... .... .... ..0. = Multiple results: Not set
.... .... .... .0.. = PS Multiple results: Not set
.... .... .... 1... = Plugin Auth: Set
.... .... ...0 .... = Connect attrs: Not set
.... .... ..0. .... = Plugin Auth LENENC Client Data: Not set
.... .... .0.. .... = Client can handle expired passwords: Not set
.... .... 0... .... = Session variable tracking: Not set
.... ...0 .... .... = Deprecate EOF: Not set
0000 000. .... .... = Unused: 0x00


Uint8 						0x0a		Protocol
NULL-terminated-str 					Banner
uint32LE								ThreadId
NULL-terminated-str						Salt 用于客户端加密密码
UInt16LE					0xffff		Server Capabilities
UInt8LE						33			Server Language, Charset, 33: utf8 COLLATE utf8_general_ci
UInt16LE					0x0002		Server Status, 0x0002 status autommit
UInt16LE					0x0008		Extended Server Capalibities
Uint8						21			Authentication Plugin Length, 21 = strlen(mysql_native_password)
10bytes						Unused		str_repeat("\0", 10)
NULL-terminated-str			具体盐值	 salt		
NULL-terminated-str			"mysql_native_password\0"	Authentication Plugin
*/
static void
mysql_dissect_greeting(struct buffer *buf, mysql_conn_data_t *conn_data)
{
    int protocol = buf_readInt8(buf);
    if (protocol == 0xff)
    {
        mysql_dissect_error_packet(buf);
        return;
    }

    // TODO !!!!! protocol ?????
    // assert(protocol == 0x00);
    mysql_set_conn_state(conn_data, LOGIN);

    // null 结尾字符串, Banner
    // TODO free
    char *ver_str = buf_readCStr(buf);
    LOG_INFO("Mysql Server Version: %s", ver_str);
    free(ver_str);

    // TODO 获取 major version
    /* version string */
    conn_data->major_version = 0;
    // char * eos = buf_findChar(buf, '\0');
    // assert(eos);
    // int lenstr = eos - buf_peek(buf);
    // int ver_offset;
    // for (ver_offset = 0; ver_offset < lenstr; ver_offset++) {
    // 	guint8 ver_char = tvb_get_guint8(tvb, offset + ver_offset);
    // 	if (ver_char == '.') break;
    // 	conn_data->major_version = conn_data->major_version * 10 + ver_char - '0';
    // }

    /* 4 bytes little endian thread_id */
    int thread_id = buf_readInt32LE(buf);
    LOG_INFO("Server Thread Id %d", thread_id);

    /* salt string */
    char *salt = buf_readCStr(buf);
    free(salt);

    /* rest is optional */
    if (!buf_readable(buf))
    {
        return;
    }

    /* 2 bytes CAPS */
    conn_data->srv_caps = buf_readInt16LE(buf);
    /* rest is optional */
    if (!buf_readable(buf))
    {
        return;
    }

    // TODO 打印信息

    /* 1 byte Charset */
    int8_t charset = buf_readInt8(buf);
    /* 2 byte ServerStatus */
    int16_t server_status = buf_readInt16LE(buf);
    /* 2 bytes ExtCAPS */
    conn_data->srv_caps_ext = buf_readInt16LE(buf);
    /* 1 byte Auth Plugin Length */
    int8_t auto_plugin_len = buf_readInt8(buf);
    /* 10 bytes unused */
    buf_retrieve(buf, 10);
    /* 4.1+ server: rest of salt */
    if (buf_readable(buf))
    {
        char *rest_of_salt = buf_readCStr(buf);
        free(rest_of_salt);
    }

    /* 5.x server: auth plugin */
    if (buf_readable(buf))
    {
        char *auth_plugin = buf_readCStr(buf);
        LOG_INFO("Mysql Server Auth Plugin: %s", auth_plugin);
        free(auth_plugin);
    }
}

static void
mysql_dissect_error_packet(struct buffer *buf)
{
    int16_t errno = buf_readInt16LE(buf);

    char *sqlstate;
    if (*buf_peek(buf) == '#')
    {
        buf_retrieve(buf, 1);

        sqlstate = buf_readStr(buf, 5);
        free(sqlstate);
    }
    char *errstr = buf_readCStr(buf);
    LOG_ERROR("%s(%d)\n", errstr, errno);
}

static void
mysql_set_conn_state(mysql_conn_data_t *conn_data, mysql_state_t state)
{
    conn_data->state = state;
}

static void
mysql_dissect_login(struct buffer *buf, mysql_conn_data_t *conn_data)
{
    int lenstr;

    /* after login there can be OK or DENIED */
    mysql_set_conn_state(conn_data, RESPONSE_OK);

    /*
UInt16LE				Client Capabilities
UInt16LE				Extended Client Capabilities
UInt32LE				MAX Packet: e.g. 300
UInt8					Charset: utf8 COLLATE utf8_general_ci (33)
Unused		 			23 Bytes 0x00
NullTerminatedString	Username: e.g. root
UInt8LenString			Password: e.g. 71f31c52cab00272caa32423f1714464113b7819
NullTerminatedString	Schema: e.g. test DB
NullTerminatedString	Client Auth Plugin: e.g. mysql_native_password
					* connection attributes
*/

    conn_data->clnt_caps = buf_readInt16LE(buf);

    /* Next packet will be use SSL */
    if (!(conn_data->frame_start_ssl) && conn_data->clnt_caps & MYSQL_CAPS_SL)
    {
        // col_set_str(pinfo->cinfo, COL_INFO, "Response: SSL Handshake");
        // conn_data->frame_start_ssl = pinfo->num;
        // ssl_starttls_ack(ssl_handle, pinfo, mysql_handle);
    }

    uint32_t maxpkt;
    uint8_t charset;
    /* 4.1 protocol */
    if (conn_data->clnt_caps & MYSQL_CAPS_CU)
    {
        /* 2 bytes client caps */
        conn_data->clnt_caps_ext = buf_readInt16LE(buf);
        /* 4 bytes max package */
        maxpkt = buf_readInt32LE(buf);
        /* 1 byte Charset */
        charset = buf_readInt8(buf);
        /* filler 23 bytes */
        buf_retrieve(buf, 23);
    }
    else
    { /* pre-4.1 */
        /* 3 bytes max package */
        maxpkt = buf_readInt32LE24(buf);
    }

    /* User name */
    char *mysql_user = buf_readCStr(buf);
    if (mysql_user) {
        LOG_INFO("Client User %s", mysql_user);
        free(mysql_user);
    }

    /* rest is optional */
    if (!buf_readable(buf))
    {
        return;
    }

    // TODO mysql_pwd
    char *mysql_pwd;
    /* 两种情况: password: ascii or length+ascii */
    if (conn_data->clnt_caps & MYSQL_CAPS_SC)
    {
        uint8_t lenstr = buf_readInt8(buf);
        mysql_pwd = buf_readStr(buf, lenstr);
    }
    else
    {
        mysql_pwd = buf_readCStr(buf);
    }
    free(mysql_pwd);

    char *mysql_schema = NULL;
    /* optional: initial schema */
    if (conn_data->clnt_caps & MYSQL_CAPS_CD)
    {
        mysql_schema = buf_readCStr(buf);
        if (mysql_schema == NULL)
        {
            return;
        }
        free(mysql_schema);
    }

    char *mysql_auth_plugin = NULL;
    /* optional: authentication plugin */
    if (conn_data->clnt_caps_ext & MYSQL_CAPS_PA)
    {
        mysql_set_conn_state(conn_data, AUTH_SWITCH_REQUEST);

        mysql_auth_plugin = buf_readCStr(buf);
        LOG_INFO("Client Auth Plugin %s", mysql_auth_plugin);
        free(mysql_auth_plugin);
    }

    /* optional: connection attributes */
    if (conn_data->clnt_caps_ext & MYSQL_CAPS_CA && buf_readable(buf))
    {
        uint64_t connattrs_length = buf_readFLE(buf, NULL, NULL);
        while (connattrs_length > 0)
        {
            int length = add_connattrs_entry_to_tree(buf);
            connattrs_length -= length;
        }
    }
}

static int
add_connattrs_entry_to_tree(struct buffer *buf) {
	int lenfle;

	char* mysql_connattrs_name = NULL;
	char* mysql_connattrs_value = NULL;
	int name_len = buf_readFLEStr(buf, &mysql_connattrs_name);
	int val_len = buf_readFLEStr(buf, &mysql_connattrs_value);
    LOG_INFO("Client Attributes %s %s", mysql_connattrs_name, mysql_connattrs_value);
	free(mysql_connattrs_name);
	free(mysql_connattrs_value);
	return name_len + val_len;
}

static void
mysql_dissect_auth_switch_response(struct buffer *buf, mysql_conn_data_t *conn_data)
{
	int lenstr;
	LOG_INFO("Auth Switch Response");

	/* Data */
	char *data = buf_readCStr(buf);
    LOG_INFO("%s", data);
	free(data);
}

/*, packet_info *pinfo, */ 
static void
mysql_dissect_request(struct buffer *buf, mysql_conn_data_t *conn_data)
{
	int lenstr;
	uint32_t stmt_id;
	my_stmt_data_t *stmt_data;
	int stmt_pos, param_offset;

	if(conn_data->state == AUTH_SWITCH_RESPONSE){
		mysql_dissect_auth_switch_response(buf, conn_data);
		return;
	}

	int opcode = buf_readInt8(buf);
    LOG_INFO("OPCODE(%02x) %s", opcode, val_to_str(opcode, "Unknown"));

	switch (opcode) {

	case MYSQL_QUIT:
		break;

	case MYSQL_PROCESS_INFO:
		mysql_set_conn_state(conn_data, RESPONSE_TABULAR);
		break;

	case MYSQL_DEBUG:
	case MYSQL_PING:
		mysql_set_conn_state(conn_data, RESPONSE_OK);
		break;

	case MYSQL_STATISTICS:
		mysql_set_conn_state(conn_data, RESPONSE_MESSAGE);
		break;

	case MYSQL_INIT_DB:
	case MYSQL_CREATE_DB:
	case MYSQL_DROP_DB:
        {
		char* mysql_schema = buf_readCStr(buf);
        LOG_INFO("Mysql Schema: %s", mysql_schema);
		free(mysql_schema);
        }
		mysql_set_conn_state(conn_data, RESPONSE_OK);
		break;

	case MYSQL_QUERY:
		{
        char * mysql_query = buf_readCStr(buf);
        LOG_INFO("Mysql Query: %s", mysql_query);
		free(mysql_query);            
        }
		mysql_set_conn_state(conn_data, RESPONSE_TABULAR);
		break;

	case MYSQL_STMT_PREPARE:
		{
        char * mysql_query = buf_readCStr(buf);
        LOG_INFO("Mysql Query: %s", mysql_query);
		free(mysql_query);
        }
		mysql_set_conn_state(conn_data, RESPONSE_PREPARE);
		break;

	case MYSQL_STMT_CLOSE:
        {
		uint32_t mysql_stmt_id = buf_readInt32LE(buf);
		mysql_set_conn_state(conn_data, REQUEST);
        }
		break;

	case MYSQL_STMT_RESET:
        {
		uint32_t mysql_stmt_id = buf_readInt32LE(buf);
		mysql_set_conn_state(conn_data, RESPONSE_OK);
        }
		break;

	case MYSQL_FIELD_LIST:
		{
		char * mysql_table_name = buf_readCStr(buf);
        LOG_INFO("Mysql Table Name: %s", mysql_table_name);
        free(mysql_table_name);
        }
		mysql_set_conn_state(conn_data, RESPONSE_SHOW_FIELDS);
		break;

	case MYSQL_PROCESS_KILL:
        {
		uint32_t mysql_thd_id = buf_readInt32LE(buf);
        }
		mysql_set_conn_state(conn_data, RESPONSE_OK);
		break;

	case MYSQL_CHANGE_USER:
		{
		char * mysql_user = buf_readCStr(buf);
		char * mysql_passwd;
		if (conn_data->clnt_caps & MYSQL_CAPS_SC) {
			int len = buf_readInt8(buf);
			mysql_passwd = buf_readStr(buf, len);
		} else {
			mysql_passwd = buf_readCStr(buf);
		}
		char * mysql_schema = buf_readCStr(buf);
        LOG_INFO("Mysql User %s, Schema %s", mysql_user, mysql_passwd);
		free(mysql_user);
		free(mysql_passwd);
		free(mysql_schema);

		if (buf_readable(buf)) {
			uint8_t charset = buf_readInt8(buf);
			buf_retrieve(buf, 1);
		}
        }
		mysql_set_conn_state(conn_data, RESPONSE_OK);

		char * mysql_client_auth_plugin = NULL;
		/* optional: authentication plugin */
		if (conn_data->clnt_caps_ext & MYSQL_CAPS_PA)
		{
			mysql_set_conn_state(conn_data, AUTH_SWITCH_REQUEST);
			mysql_client_auth_plugin = buf_readCStr(buf);
            LOG_INFO("Mysql Client Auth Plugin %s", mysql_client_auth_plugin);
			free(mysql_client_auth_plugin);
		}

		/* optional: connection attributes */
		if (conn_data->clnt_caps_ext & MYSQL_CAPS_CA)
		{
			uint64_t lenfle;
			int length;
			uint64_t connattrs_length = buf_readFLE(buf, &lenfle, NULL);
			while (connattrs_length > 0) {
				int length = add_connattrs_entry_to_tree(buf);
				connattrs_length -= length;
			}
		}
		break;

	case MYSQL_REFRESH:
        {
		uint8_t mysql_refresh = buf_readInt8(buf);
        }
		mysql_set_conn_state(conn_data, RESPONSE_OK);
		break;

	case MYSQL_SHUTDOWN:
        {
		uint8_t mysql_shutdown = buf_readInt8(buf);
        }
		mysql_set_conn_state(conn_data, RESPONSE_OK);
		break;

	case MYSQL_SET_OPTION:
        {
		uint16_t mysql_option = buf_readInt16LE(buf);
        }
		mysql_set_conn_state(conn_data, RESPONSE_OK);
		break;

	case MYSQL_STMT_FETCH:
        {
		uint32_t mysql_stmt_id = buf_readInt32LE(buf);
		uint32_t mysql_num_rows = buf_readInt32LE(buf);
        }
		mysql_set_conn_state(conn_data, RESPONSE_TABULAR);
		break;

	case MYSQL_STMT_SEND_LONG_DATA:
        {
		uint32_t mysql_stmt_id = buf_readInt32LE(buf);
		uint16_t mysql_param = buf_readInt16(buf);
        }

		if (buf_readable(buf)) {
			char * mysql_payload = buf_readStr(buf, buf_readable(buf));
            LOG_INFO("Mysql Payload %s", mysql_payload); // TODO null str ???
			free(mysql_payload);
		}
		mysql_set_conn_state(conn_data, REQUEST);
		break;

	case MYSQL_STMT_EXECUTE:
        {
		uint32_t mysql_stmt_id = buf_readInt32LE(buf);
		uint8_t mysql_exec = buf_readInt8(buf);
		uint32_t mysql_exec_iter = buf_readInt32LE(buf);
        }

		// TODO STMT !!!!!
		// stmt_data = (my_stmt_data_t *)wmem_tree_lookup32(conn_data->stmts, stmt_id);
		// if (stmt_data != NULL) {
		// 	if (stmt_data->nparam != 0) {
		// 		uint8_t stmt_bound;
		// 		offset += (stmt_data->nparam + 7) / 8; /* NULL bitmap */
		// 		proto_tree_add_item(req_tree, hf_mysql_new_parameter_bound_flag, tvb, offset, 1, ENC_NA);
		// 		stmt_bound = tvb_get_guint8(tvb, offset);
		// 		offset += 1;
		// 		if (stmt_bound == 1) {
		// 			param_offset = offset + stmt_data->nparam * 2;
		// 			for (stmt_pos = 0; stmt_pos < stmt_data->nparam; stmt_pos++) {
		// 				if (!mysql_dissect_exec_param(req_tree, tvb, &offset, &param_offset,
		// 							      stmt_data->param_flags[stmt_pos], pinfo))
		// 					break;
		// 			}
		// 			offset = param_offset;
		// 		}
		// 	}
		// } else
        {
			if (buf_readable(buf)) {
				char * mysql_payload = buf_readStr(buf, buf_readable(buf));
                LOG_INFO("Mysql Payload %s", mysql_payload); // TODO null str ???
				free(mysql_payload);
			}
		}

        /*
		if (buf_readable(buf)) {
			char * mysql_payload = buf_readStr(buf, buf_readable(buf));
			free(mysql_payload);
		}
        */

		mysql_set_conn_state(conn_data, RESPONSE_TABULAR);
		break;

	case MYSQL_BINLOG_DUMP:
        {
		uint32_t mysql_binlog_position = buf_readInt32LE(buf);
		uint16_t mysql_binlog_flags = buf_readInt16(buf); // BIG_ENDIAN !!!
		uint32_t mysql_binlog_server_id = buf_readInt32LE(buf);
        }

		/* binlog file name ? */
		if (buf_readable(buf)) {
			char* mysql_binlog_file_name = buf_readStr(buf, buf_readable(buf));
			LOG_INFO("Mysql Binlog File Name %s", mysql_binlog_file_name);
            free(mysql_binlog_file_name);
		}

		mysql_set_conn_state(conn_data, REQUEST);
		break;

	case MYSQL_TABLE_DUMP:
	case MYSQL_CONNECT_OUT:
	case MYSQL_REGISTER_SLAVE:
		mysql_set_conn_state(conn_data, REQUEST);
		break;

	default:
		mysql_set_conn_state(conn_data, UNDEFINED);
	}
}

static void
mysql_dissect_response(struct buffer *buf, mysql_conn_data_t *conn_data)
{
	uint16_t server_status = 0;
	// uint8_t response_code = buf_readInt8(buf);
    uint8_t response_code = buf_peekInt8(buf);
	
    printf("0x%04x\n", response_code);
	if ( response_code == 0xff ) { // ERR
        buf_retrieve(buf, sizeof(uint8_t));
		mysql_dissect_error_packet(buf);
		mysql_set_conn_state(conn_data, REQUEST);
	} else if (response_code == 0xfe && buf_readable(buf) < 9) { // EOF
	 	// uint8_t mysql_eof = buf_readInt8(buf);

		// /* pre-4.1 packet ends here */
		// if (buf_readable(buf)) {
		// 	uint16_t mysql_num_warn = buf_readInt16LE(buf);
		// 	server_status = buf_readInt16LE(buf);
		// }

		// switch (conn_data->state) {
		// case FIELD_PACKET:
		// 	mysql_set_conn_state(conn_data, ROW_PACKET);
		// 	break;
		// case ROW_PACKET:
		// 	if (server_status & MYSQL_STAT_MU) {
		// 		mysql_set_conn_state(conn_data, RESPONSE_TABULAR);
		// 	} else {
		// 		mysql_set_conn_state(conn_data, REQUEST);
		// 	}
		// 	break;
		// case PREPARED_PARAMETERS:
		// 	if (conn_data->stmt_num_fields > 0) {
		// 		mysql_set_conn_state(conn_data, PREPARED_FIELDS);
		// 	} else {
		// 		mysql_set_conn_state(conn_data, REQUEST);
		// 	}
		// 	break;
		// case PREPARED_FIELDS:
		// 	mysql_set_conn_state(conn_data, REQUEST);
		// 	break;
		// default:
		// 	/* This should be an unreachable case */
		// 	mysql_set_conn_state(conn_data, REQUEST);
		// }
	} else if (response_code == 0x00) { // OK
		if (conn_data->state == RESPONSE_PREPARE) {
			// mysql_dissect_response_prepare(buf, conn_data);
		} else if (buf_readable(buf) > buf_peekFLELen(buf)) {
			mysql_dissect_ok_packet(buf, conn_data);
			// if (conn_data->compressed_state == MYSQL_COMPRESS_INIT) {
			// 	/* This is the OK packet which follows the compressed protocol setup */
			// 	conn_data->compressed_state = MYSQL_COMPRESS_ACTIVE;
			// }
		} else {
			mysql_dissect_result_header(buf, conn_data);
		}
	} else {
		switch (conn_data->state) {
		case RESPONSE_MESSAGE:
			// if ((lenstr = tvb_reported_length_remaining(tvb, offset))) {
			// 	proto_tree_add_item(tree, hf_mysql_message, tvb, offset, lenstr, ENC_ASCII|ENC_NA);
			// 	offset += lenstr;
			// }
			mysql_set_conn_state(conn_data, REQUEST);
			break;

		case RESPONSE_TABULAR:
			mysql_dissect_result_header(buf, conn_data);
			break;

		case FIELD_PACKET:
		case RESPONSE_SHOW_FIELDS:
		case RESPONSE_PREPARE:
		case PREPARED_PARAMETERS:
			mysql_dissect_field_packet(buf, conn_data);
			break;

		case ROW_PACKET:
			// offset = mysql_dissect_row_packet(tvb, offset, tree);
			break;

		case PREPARED_FIELDS:
			// offset = mysql_dissect_field_packet(tvb, offset, tree, conn_data);
			break;

		case AUTH_SWITCH_REQUEST:
			// offset = mysql_dissect_auth_switch_request(tvb, offset, tree, conn_data);
			break;

		default:
			// ti = proto_tree_add_item(tree, hf_mysql_payload, tvb, offset, -1, ENC_NA);
			// expert_add_info(ti, &ei_mysql_unknown_response);
			// offset += tvb_reported_length_remaining(tvb, offset);
			// mysql_set_conn_state(conn_data, UNDEFINED);
            break;
		}
	}
}

static void
mysql_dissect_result_header(struct buffer *buf, mysql_conn_data_t *conn_data)
{
    LOG_INFO("TABULAR");	
    uint64_t num_fields = buf_readFLE(buf, NULL, NULL);
    LOG_INFO("num fields %llu", num_fields);
    if (buf_readable(buf)) {
    	uint64_t extra = buf_readFLE(buf, NULL, NULL);
        LOG_INFO("extra %llu", extra);
    }

	if (num_fields) {
		mysql_set_conn_state(conn_data, FIELD_PACKET);
	} else {
		mysql_set_conn_state(conn_data, ROW_PACKET);
	}
}

static void
mysql_dissect_ok_packet(struct buffer *buf, mysql_conn_data_t *conn_data)
{
	LOG_INFO("OK");
	
	uint64_t affected_rows = buf_readFLE(buf, NULL, NULL);
	LOG_INFO("affected rows %llu", affected_rows);

	uint64_t insert_id = buf_readFLE(buf, NULL, NULL);
	if (insert_id) {
		LOG_INFO("insert id %llu", insert_id);
	}

    if (buf_readable(buf)) {

    }
	// if (tvb_reported_length_remaining(tvb, offset) > 0) {
	// 	offset = mysql_dissect_server_status(tvb, offset, tree, &server_status);

	// 	/* 4.1+ protocol only: 2 bytes number of warnings */
	// 	if (conn_data->clnt_caps & conn_data->srv_caps & MYSQL_CAPS_CU) {
	// 		proto_tree_add_item(tree, hf_mysql_num_warn, tvb, offset, 2, ENC_LITTLE_ENDIAN);
	// 	offset += 2;
	// 	}
	// }

	// if (conn_data->clnt_caps_ext & MYSQL_CAPS_ST) {
	// 	if (tvb_reported_length_remaining(tvb, offset) > 0) {
	// 		guint64 session_track_length;
	// 		proto_item *tf;
	// 		proto_item *session_track_tree = NULL;
	// 		int length;

	// 		offset += tvb_get_fle(tvb, offset, &lenstr, NULL);
	// 		/* first read the optional message */
	// 		if (lenstr) {
	// 			proto_tree_add_item(tree, hf_mysql_message, tvb, offset, (gint)lenstr, ENC_ASCII|ENC_NA);
	// 			offset += (int)lenstr;
	// 		}

	// 		/* session state tracking */
	// 		if (server_status & MYSQL_STAT_SESSION_STATE_CHANGED) {
	// 			fle = tvb_get_fle(tvb, offset, &session_track_length, NULL);
	// 			tf = proto_tree_add_item(tree, hf_mysql_session_track_data, tvb, offset, -1, ENC_NA);
	// 			session_track_tree = proto_item_add_subtree(tf, ett_session_track_data);
	// 			proto_tree_add_uint64(tf, hf_mysql_session_track_data_length, tvb, offset, fle, session_track_length);
	// 			offset += fle;

	// 			while (session_track_length > 0) {
	// 				length = add_session_tracker_entry_to_tree(tvb, pinfo, session_track_tree, offset);
	// 				offset += length;
	// 				session_track_length -= length;
	// 			}
	// 		}
	// 	}
	// } else {
	// 	/* optional: message string */
	// 	if (tvb_reported_length_remaining(tvb, offset) > 0) {
	// 		lenstr = tvb_reported_length_remaining(tvb, offset);
	// 		proto_tree_add_item(tree, hf_mysql_message, tvb, offset, (gint)lenstr, ENC_ASCII|ENC_NA);
	// 		offset += (int)lenstr;
	// 	}
	// }

	mysql_set_conn_state(conn_data, REQUEST);
}

static void
mysql_dissect_field_packet(struct buffer* buf, mysql_conn_data_t *conn_data)
{
    char* str;
    buf_readFLEStr(buf, &str); LOG_INFO("catalog %s", str); free(str);
    buf_readFLEStr(buf, &str); LOG_INFO("db %s", str); free(str);
    buf_readFLEStr(buf, &str); LOG_INFO("table %s", str); free(str);
    buf_readFLEStr(buf, &str); LOG_INFO("org_table %s", str); free(str);
    buf_readFLEStr(buf, &str); LOG_INFO("name %s", str); free(str);
    buf_readFLEStr(buf, &str); LOG_INFO("org_name %s", str); free(str);

    buf_retrieve(buf, 1);

    uint16_t field_charset = buf_readInt16LE(buf);
    uint32_t field_length = buf_readInt32LE(buf);
    uint8_t field_type = buf_readInt8(buf);
    // TODO
    uint16_t flags = buf_readInt16LE(buf);
    uint8_t field_decimal = buf_readInt8(buf);

    buf_retrieve(buf, 2);

    // TODO !!!!


	// offset = mysql_field_add_lestring(tvb, offset, tree, hf_mysql_fld_catalog);
	// offset = mysql_field_add_lestring(tvb, offset, tree, hf_mysql_fld_db);
	// offset = mysql_field_add_lestring(tvb, offset, tree, hf_mysql_fld_table);
	// offset = mysql_field_add_lestring(tvb, offset, tree, hf_mysql_fld_org_table);
	// offset = mysql_field_add_lestring(tvb, offset, tree, hf_mysql_fld_name);
	// offset = mysql_field_add_lestring(tvb, offset, tree, hf_mysql_fld_org_name);
	// offset +=1; /* filler */

	// proto_tree_add_item(tree, hf_mysql_fld_charsetnr, tvb, offset, 2, ENC_LITTLE_ENDIAN);
	// offset += 2; /* charset */

	// proto_tree_add_item(tree, hf_mysql_fld_length, tvb, offset, 4, ENC_LITTLE_ENDIAN);
	// offset += 4; /* length */

	// proto_tree_add_item(tree, hf_mysql_fld_type, tvb, offset, 1, ENC_NA);
	// offset += 1; /* type */

	// proto_tree_add_bitmask_with_flags(tree, tvb, offset, hf_mysql_fld_flags, ett_field_flags, mysql_fld_flags, ENC_LITTLE_ENDIAN, BMT_NO_APPEND);
	// offset += 2; /* flags */

	// proto_tree_add_item(tree, hf_mysql_fld_decimals, tvb, offset, 1, ENC_NA);
	// offset += 1; /* decimals */

	// offset += 2; /* filler */

	// /* default (Only use for show fields) */
	// if (tree && tvb_reported_length_remaining(tvb, offset) > 0) {
	// 	offset = mysql_field_add_lestring(tvb, offset, tree, hf_mysql_fld_default);
	// }
	// return offset;
}
