/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "php_swoole_cxx.h"
#include "swoole_http.h"

#ifdef SW_USE_HTTP2
#include "swoole_http_v2_client.h"

#include <vector>

using namespace swoole;
using swoole::coroutine::Socket;

static zend_class_entry *swoole_http2_client_coro_ce;
static zend_object_handlers swoole_http2_client_coro_handlers;

static zend_class_entry *swoole_http2_client_coro_exception_ce;
static zend_object_handlers swoole_http2_client_coro_exception_handlers;

static zend_class_entry *swoole_http2_request_ce;
static zend_object_handlers swoole_http2_request_handlers;

zend_class_entry *swoole_http2_response_ce;
static zend_object_handlers swoole_http2_response_handlers;

class http2_client
{
public:
    char *host;
    size_t host_len;
    int port;
    uint8_t ssl;
    double timeout;
    zval *object;
    zval _object;

    swoole::coroutine::Socket *client;

    nghttp2_hd_inflater *inflater;
    nghttp2_hd_deflater *deflater;

    uint32_t stream_id; // the next send stream id
    uint32_t last_stream_id; // the last received stream id

    swHttp2_settings local_settings;
    swHttp2_settings remote_settings;

    swHashMap *streams;

    http2_client(const char *_host, size_t _host_len, int _port, zval *_zobject)
    {
        host = estrndup(_host, _host_len);
        host_len = _host_len;
        port = _port;
        timeout = Socket::default_read_timeout;
        object = _zobject;
        sw_copy_to_stack(object, _object);

        ssl = false;
        inflater = nullptr;
        deflater = nullptr;
        last_stream_id = 0;
        stream_id = 0;
        client = nullptr;
        streams = nullptr;
    }

    ~http2_client()
    {
        close();
        if (host)
        {
            efree(host);
        }
    }

    bool connect();

    bool close()
    {
        if (!client)
        {
            return false;
        }

        client->close();
        delete client;
        client = nullptr;

        if (streams)
        {
            swHashMap_free(streams);
            streams = NULL;
        }
        if (inflater)
        {
            nghttp2_hd_inflate_del(inflater);
            inflater = NULL;
        }
        if (deflater)
        {
            nghttp2_hd_deflate_del(deflater);
            deflater = NULL;
        }
        zend_update_property_bool(swoole_http2_client_coro_ce, object, ZEND_STRL("connected"), 0);
        return true;
    }
    int parse_header(http2_client_stream *stream , int flags, char *in, size_t inlen);
    enum swReturnType parse_frame(zval *return_value);
};

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http2_client_coro_construct, 0, 0, 1)
    ZEND_ARG_INFO(0, host)
    ZEND_ARG_INFO(0, port)
    ZEND_ARG_INFO(0, ssl)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http2_client_coro_set, 0, 0, 1)
    ZEND_ARG_ARRAY_INFO(0, settings, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http2_client_coro_stats, 0, 0, 0)
    ZEND_ARG_INFO(0, key)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http2_client_coro_isStreamExist, 0, 0, 1)
    ZEND_ARG_INFO(0, stream_id)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http2_client_coro_send, 0, 0, 1)
    ZEND_ARG_INFO(0, request)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http2_client_coro_write, 0, 0, 2)
    ZEND_ARG_INFO(0, stream_id)
    ZEND_ARG_INFO(0, data)
    ZEND_ARG_INFO(0, end_stream)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http2_client_coro_recv, 0, 0, 0)
    ZEND_ARG_INFO(0, timeout)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http2_client_coro_goaway, 0, 0, 0)
    ZEND_ARG_INFO(0, error_code)
    ZEND_ARG_INFO(0, debug_data)
ZEND_END_ARG_INFO()

static PHP_METHOD(swoole_http2_client_coro, __construct);
static PHP_METHOD(swoole_http2_client_coro, __destruct);
static PHP_METHOD(swoole_http2_client_coro, set);
static PHP_METHOD(swoole_http2_client_coro, connect);
static PHP_METHOD(swoole_http2_client_coro, stats);
static PHP_METHOD(swoole_http2_client_coro, isStreamExist);
static PHP_METHOD(swoole_http2_client_coro, send);
static PHP_METHOD(swoole_http2_client_coro, write);
static PHP_METHOD(swoole_http2_client_coro, recv);
static PHP_METHOD(swoole_http2_client_coro, ping);
static PHP_METHOD(swoole_http2_client_coro, goaway);
static PHP_METHOD(swoole_http2_client_coro, close);

static uint32_t http2_client_send_request(zval *zobject, zval *request);
static void http2_client_stream_free(void *ptr);
static ssize_t http2_client_send_setting(Socket *cli, swHttp2_settings  *settings);
static ssize_t http2_client_send_window_update(Socket *cli, int stream_id, uint32_t size);

static inline http2_client_stream* http2_client_stream_get(http2_client *h2c, uint32_t stream_id)
{
    return (http2_client_stream*) swHashMap_find_int(h2c->streams, stream_id);
}

static const zend_function_entry swoole_http2_client_methods[] =
{
    PHP_ME(swoole_http2_client_coro, __construct,   arginfo_swoole_http2_client_coro_construct, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, __destruct,    arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, set,           arginfo_swoole_http2_client_coro_set, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, connect,       arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, stats,         arginfo_swoole_http2_client_coro_stats, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, isStreamExist, arginfo_swoole_http2_client_coro_isStreamExist, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, send,          arginfo_swoole_http2_client_coro_send, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, write,         arginfo_swoole_http2_client_coro_write, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, recv,          arginfo_swoole_http2_client_coro_recv, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, goaway,        arginfo_swoole_http2_client_coro_goaway, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, ping,        arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http2_client_coro, close,         arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void swoole_http2_client_coro_init(int module_number)
{
    SW_INIT_CLASS_ENTRY(swoole_http2_client_coro, "Swoole\\Coroutine\\Http2\\Client", NULL, "Co\\Http2\\Client", swoole_http2_client_methods);
    SW_SET_CLASS_SERIALIZABLE(swoole_http2_client_coro, zend_class_serialize_deny, zend_class_unserialize_deny);
    SW_SET_CLASS_CLONEABLE(swoole_http2_client_coro, zend_class_clone_deny);
    SW_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_http2_client_coro, zend_class_unset_property_deny);

    SW_INIT_CLASS_ENTRY_EX(swoole_http2_client_coro_exception, "Swoole\\Coroutine\\Http2\\Client\\Exception", NULL, "Co\\Http2\\Client\\Exception", NULL, swoole_exception);

    SW_INIT_CLASS_ENTRY(swoole_http2_request, "Swoole\\Http2\\Request", "swoole_http2_request", NULL, NULL);
    SW_SET_CLASS_SERIALIZABLE(swoole_http2_request, zend_class_serialize_deny, zend_class_unserialize_deny);
    SW_SET_CLASS_CLONEABLE(swoole_http2_request, zend_class_clone_deny);
    SW_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_http2_request, zend_class_unset_property_deny);

    SW_INIT_CLASS_ENTRY(swoole_http2_response, "Swoole\\Http2\\Response", "swoole_http2_response", NULL, NULL);
    SW_SET_CLASS_SERIALIZABLE(swoole_http2_response, zend_class_serialize_deny, zend_class_unserialize_deny);
    SW_SET_CLASS_CLONEABLE(swoole_http2_response, zend_class_clone_deny);
    SW_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_http2_response, zend_class_unset_property_deny);

    zend_declare_property_long(swoole_http2_client_coro_ce, ZEND_STRL("errCode"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(swoole_http2_client_coro_ce, ZEND_STRL("errMsg"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(swoole_http2_client_coro_ce, ZEND_STRL("sock"), -1, ZEND_ACC_PUBLIC);
    zend_declare_property_long(swoole_http2_client_coro_ce, ZEND_STRL("type"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http2_client_coro_ce, ZEND_STRL("setting"), ZEND_ACC_PUBLIC);
    zend_declare_property_bool(swoole_http2_client_coro_ce, ZEND_STRL("connected"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http2_client_coro_ce, ZEND_STRL("host"), ZEND_ACC_PUBLIC);
    zend_declare_property_long(swoole_http2_client_coro_ce, ZEND_STRL("port"), 0, ZEND_ACC_PUBLIC);

    zend_declare_property_string(swoole_http2_request_ce, ZEND_STRL("path"), "/", ZEND_ACC_PUBLIC);
    zend_declare_property_string(swoole_http2_request_ce, ZEND_STRL("method"), "GET", ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http2_request_ce, ZEND_STRL("headers"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http2_request_ce, ZEND_STRL("cookies"), ZEND_ACC_PUBLIC);
    zend_declare_property_string(swoole_http2_request_ce, ZEND_STRL("data"), "", ZEND_ACC_PUBLIC);
    zend_declare_property_bool(swoole_http2_request_ce, ZEND_STRL("pipeline"), 0, ZEND_ACC_PUBLIC);

    zend_declare_property_long(swoole_http2_response_ce, ZEND_STRL("streamId"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(swoole_http2_response_ce, ZEND_STRL("errCode"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(swoole_http2_response_ce, ZEND_STRL("statusCode"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_bool(swoole_http2_response_ce, ZEND_STRL("pipeline"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http2_response_ce, ZEND_STRL("headers"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http2_response_ce, ZEND_STRL("set_cookie_headers"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http2_response_ce, ZEND_STRL("cookies"), ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http2_response_ce, ZEND_STRL("data"), ZEND_ACC_PUBLIC);

    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_TYPE_DATA", SW_HTTP2_TYPE_DATA);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_TYPE_HEADERS", SW_HTTP2_TYPE_HEADERS);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_TYPE_PRIORITY", SW_HTTP2_TYPE_PRIORITY);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_TYPE_RST_STREAM", SW_HTTP2_TYPE_RST_STREAM);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_TYPE_SETTINGS", SW_HTTP2_TYPE_SETTINGS);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_TYPE_PUSH_PROMISE", SW_HTTP2_TYPE_PUSH_PROMISE);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_TYPE_PING", SW_HTTP2_TYPE_PING);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_TYPE_GOAWAY", SW_HTTP2_TYPE_GOAWAY);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_TYPE_WINDOW_UPDATE", SW_HTTP2_TYPE_WINDOW_UPDATE);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_TYPE_CONTINUATION", SW_HTTP2_TYPE_CONTINUATION);

    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_NO_ERROR", SW_HTTP2_ERROR_NO_ERROR);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_PROTOCOL_ERROR", SW_HTTP2_ERROR_PROTOCOL_ERROR);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_INTERNAL_ERROR", SW_HTTP2_ERROR_INTERNAL_ERROR);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_FLOW_CONTROL_ERROR", SW_HTTP2_ERROR_FLOW_CONTROL_ERROR);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_SETTINGS_TIMEOUT", SW_HTTP2_ERROR_SETTINGS_TIMEOUT);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_STREAM_CLOSED", SW_HTTP2_ERROR_STREAM_CLOSED);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_FRAME_SIZE_ERROR", SW_HTTP2_ERROR_FRAME_SIZE_ERROR);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_REFUSED_STREAM", SW_HTTP2_ERROR_REFUSED_STREAM);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_CANCEL", SW_HTTP2_ERROR_CANCEL);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_COMPRESSION_ERROR", SW_HTTP2_ERROR_COMPRESSION_ERROR);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_CONNECT_ERROR", SW_HTTP2_ERROR_CONNECT_ERROR);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_ENHANCE_YOUR_CALM", SW_HTTP2_ERROR_ENHANCE_YOUR_CALM);
    SW_REGISTER_LONG_CONSTANT("SWOOLE_HTTP2_ERROR_INADEQUATE_SECURITY", SW_HTTP2_ERROR_INADEQUATE_SECURITY);
}

bool http2_client::connect()
{
    client = new Socket(SW_SOCK_TCP);

    client->http2 = 1;
    client->open_length_check = 1;
    client->protocol.get_package_length = swHttp2_get_frame_length;
    client->protocol.package_length_size = SW_HTTP2_FRAME_HEADER_SIZE;
    client->protocol.package_max_length = SW_BUFFER_INPUT_SIZE;

    zval *ztmp;
    HashTable *vht;
    zval *zset = sw_zend_read_property(swoole_http2_client_coro_ce, object, ZEND_STRL("setting"), 0);
    if (zset && ZVAL_IS_ARRAY(zset))
    {
        vht = Z_ARRVAL_P(zset);
        /**
         * timeout
         */
        if (php_swoole_array_get_value(vht, "timeout", ztmp))
        {
            timeout = zval_get_double(ztmp);
        }
        //client settings
        php_swoole_client_set(client, zset);
    }

    client->open_ssl = ssl;

    bool retval = client->connect(host, port, 0);
    if (!retval)
    {
        zend_update_property_long(swoole_http2_client_coro_ce, object, ZEND_STRL("errCode"), client->errCode);
        zend_update_property_string(swoole_http2_client_coro_ce, object, ZEND_STRL("errMsg"), client->errMsg);
        delete client;
        client = nullptr;
        return false;
    }

    stream_id = 1;
    streams = swHashMap_new(8, http2_client_stream_free);
    // [init]: we must set default value, server is not always send all the settings
    swHttp2_init_settings(&local_settings);
    swHttp2_init_settings(&remote_settings);
    int ret = nghttp2_hd_inflate_new(&inflater);
    if (ret != 0)
    {
        swoole_php_error(E_WARNING, "nghttp2_hd_inflate_init() failed with error: %s[%d]", nghttp2_strerror(ret), ret);
        close();
        return false;
    }
    ret = nghttp2_hd_deflate_new(&deflater, local_settings.header_table_size);
    if (ret != 0)
    {
        swoole_php_error(E_WARNING, "nghttp2_hd_deflate_init() failed with error: %s[%d]", nghttp2_strerror(ret), ret);
        close();
        return false;
    }

    if (client->send_all((char *) ZEND_STRL(SW_HTTP2_PRI_STRING))<=0)
    {
        close();
        return false;
    }

    if (http2_client_send_setting(client, &local_settings) <= 0)
    {
        close();
        return false;
    }

    zend_update_property_bool(swoole_http2_client_coro_ce, object, ZEND_STRL("connected"), 1);

    return true;
}

enum swReturnType http2_client::parse_frame(zval *return_value)
{
    char *buf = client->get_read_buffer()->str;
    uint8_t type = buf[3];
    uint8_t flags = buf[4];
    uint32_t stream_id = ntohl((*(int *) (buf + 5))) & 0x7fffffff;
    ssize_t length = swHttp2_get_length(buf);
    buf += SW_HTTP2_FRAME_HEADER_SIZE;

    char frame[SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_FRAME_PING_PAYLOAD_SIZE];

    if (stream_id > last_stream_id)
    {
        last_stream_id = stream_id;
    }

    uint16_t id = 0;
    uint32_t value = 0;

    switch (type)
    {
    case SW_HTTP2_TYPE_SETTINGS:
    {
        if (flags & SW_HTTP2_FLAG_ACK)
        {
            swHttp2FrameTraceLog(recv, "ACK");
            return SW_CONTINUE;
        }

        while (length > 0)
        {
            id = ntohs(*(uint16_t *) (buf));
            value = ntohl(*(uint32_t *) (buf + sizeof(uint16_t)));
            swHttp2FrameTraceLog(recv, "id=%d, value=%d", id, value);
            switch (id)
            {
            case SW_HTTP2_SETTING_HEADER_TABLE_SIZE:
                if (value != remote_settings.header_table_size)
                {
                    remote_settings.header_table_size = value;
                    int ret = nghttp2_hd_deflate_change_table_size(deflater, value);
                    if (ret != 0)
                    {
                        swoole_php_error(E_WARNING,
                                "nghttp2_hd_inflate_change_table_size() failed with error: %s[%d]\n",
                                nghttp2_strerror(ret), ret);
                        close();
                        return SW_ERROR;
                    }
                }
                swTraceLog(SW_TRACE_HTTP2, "setting: header_compression_table_max=%u", value);
                break;
            case SW_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS:
                remote_settings.max_concurrent_streams = value;
                swTraceLog(SW_TRACE_HTTP2, "setting: max_concurrent_streams=%u", value);
                break;
            case SW_HTTP2_SETTINGS_INIT_WINDOW_SIZE:
                remote_settings.window_size = value;
                swTraceLog(SW_TRACE_HTTP2, "setting: init_send_window=%u", value);
                break;
            case SW_HTTP2_SETTINGS_MAX_FRAME_SIZE:
                remote_settings.max_frame_size = value;
                swTraceLog(SW_TRACE_HTTP2, "setting: max_frame_size=%u", value);
                break;
            case SW_HTTP2_SETTINGS_MAX_HEADER_LIST_SIZE:
                if (value != remote_settings.max_header_list_size)
                {
                    remote_settings.max_header_list_size = value;
                    int ret = nghttp2_hd_inflate_change_table_size(inflater, value);
                    if (ret != 0)
                    {
                        swoole_php_error(E_WARNING,
                                "nghttp2_hd_inflate_change_table_size() failed with error: %s[%d]\n",
                                nghttp2_strerror(ret), ret);
                        close();
                        return SW_ERROR;
                    }
                }
                swTraceLog(SW_TRACE_HTTP2, "setting: max_header_list_size=%u", value);
                break;
            default:
                // disable warning and ignore it because some websites are not following http2 protocol totally
                // swWarn("unknown option[%d]: %d", id, value);
                break;
            }
            buf += sizeof(id) + sizeof(value);
            length -= sizeof(id) + sizeof(value);
        }

        swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_SETTINGS, 0, SW_HTTP2_FLAG_ACK, stream_id);
        if (!client->send_all(frame, SW_HTTP2_FRAME_HEADER_SIZE))
        {
            swoole_php_error(E_WARNING, "send failed");
            return SW_ERROR;
        }
        return SW_CONTINUE;
    }
    case SW_HTTP2_TYPE_WINDOW_UPDATE:
    {
        value = ntohl(*(uint32_t *) buf);
        swHttp2FrameTraceLog(recv, "window_size_increment=%d", value);
        if (stream_id == 0)
        {
            remote_settings.window_size += value;
        }
        else
        {
            http2_client_stream *stream = http2_client_stream_get(this, stream_id);
            if (stream)
            {
                stream->remote_window_size += value;
            }
        }
        return SW_CONTINUE;
    }
    case SW_HTTP2_TYPE_PING:
    {
        swHttp2FrameTraceLog(recv, "ping");
        if (!(flags & SW_HTTP2_FLAG_ACK))
        {
            swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_PING, SW_HTTP2_FRAME_PING_PAYLOAD_SIZE, SW_HTTP2_FLAG_ACK,
                    stream_id);
            memcpy(frame + SW_HTTP2_FRAME_HEADER_SIZE, buf + SW_HTTP2_FRAME_HEADER_SIZE,
                    SW_HTTP2_FRAME_PING_PAYLOAD_SIZE);
            client->send_all(frame, SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_FRAME_PING_PAYLOAD_SIZE);
        }
        return SW_CONTINUE;
    }
    case SW_HTTP2_TYPE_GOAWAY:
    {
        uint32_t server_last_stream_id = ntohl(*(uint32_t *) (buf));
        buf += 4;
        value = ntohl(*(uint32_t *) (buf));
        buf += 4;
        swHttp2FrameTraceLog(recv, "last_stream_id=%d, error_code=%d, opaque_data=[%.*s]", server_last_stream_id, value,
                (int) (length - SW_HTTP2_GOAWAY_SIZE), buf);

        // update goaway error code and error msg
        zend_update_property_long(swoole_http2_client_coro_ce, object, ZEND_STRL("errCode"), value);
        zend_update_property_stringl(swoole_http2_client_coro_ce, object, ZEND_STRL("errMsg"), buf,
                length - SW_HTTP2_GOAWAY_SIZE);
        zend_update_property_long(swoole_http2_client_coro_ce, object, ZEND_STRL("serverLastStreamId"),
                server_last_stream_id);
        close(); // will trigger onClose and resume
        return SW_CONTINUE;
    }
    case SW_HTTP2_TYPE_RST_STREAM:
    {
        value = ntohl(*(uint32_t *) (buf));
        swHttp2FrameTraceLog(recv, "error_code=%d", value);

        // delete and free quietly
        swHashMap_del_int(streams, stream_id);
        return SW_CONTINUE;
        break;
    }
    default:
    {
        swHttp2FrameTraceLog(recv, "");
    }
    }

    http2_client_stream *stream = http2_client_stream_get(this, stream_id);
    // stream has closed
    if (stream == NULL)
    {
        swoole_php_error(E_WARNING, "no stream[%d]", stream_id);
        return SW_ERROR;
    }
    if (type == SW_HTTP2_TYPE_HEADERS)
    {
        parse_header(stream, flags, buf, length);
    }
    else if (type == SW_HTTP2_TYPE_DATA)
    {
        if (length > 0)
        {
            if (!stream->buffer)
            {
                stream->buffer = swString_new(SW_HTTP2_DATA_BUFFER_SIZE);
            }
#ifdef SW_HAVE_ZLIB
            if (stream->gzip)
            {
                if (php_swoole_zlib_uncompress(&stream->gzip_stream, stream->gzip_buffer, buf, length) == SW_ERR)
                {
                    swoole_php_error(E_WARNING, "uncompress failed", stream_id);
                    return SW_ERROR;
                }
                swString_append_ptr(stream->buffer, stream->gzip_buffer->str, stream->gzip_buffer->length);
            }
            else
#endif
            {
                swString_append_ptr(stream->buffer, buf, length);
            }

            // now we control the connection flow only (not stream)
            // our window size is unlimited, so we don't worry about subtraction overflow
            local_settings.window_size -= length;
            stream->local_window_size -= length;
            if (local_settings.window_size < (SW_HTTP2_MAX_WINDOW_SIZE / 4))
            {
                http2_client_send_window_update(client, 0, SW_HTTP2_MAX_WINDOW_SIZE - local_settings.window_size);
                local_settings.window_size = SW_HTTP2_MAX_WINDOW_SIZE;
            }
            if (stream->local_window_size < (SW_HTTP2_MAX_WINDOW_SIZE / 4))
            {
                http2_client_send_window_update(client, stream_id,
                        SW_HTTP2_MAX_WINDOW_SIZE - stream->local_window_size);
                stream->local_window_size = SW_HTTP2_MAX_WINDOW_SIZE;
            }
        }
    }

    uint8_t stream_type = stream->type;
    if ((type == SW_HTTP2_TYPE_DATA && stream_type == SW_HTTP2_STREAM_PIPELINE) // pipeline data frame
    || (stream_type == SW_HTTP2_STREAM_NORMAL && (flags & SW_HTTP2_FLAG_END_STREAM)) // normal end frame
            || type == SW_HTTP2_TYPE_RST_STREAM || type == SW_HTTP2_TYPE_GOAWAY // rst and goaway frame
                    )
    {
        zval _zresponse = stream->_response_object;
        zval *zresponse = &_zresponse;

        if (type == SW_HTTP2_TYPE_RST_STREAM)
        {
            zend_update_property_long(swoole_http2_response_ce, zresponse, ZEND_STRL("statusCode"), -3);
            zend_update_property_long(swoole_http2_response_ce, zresponse, ZEND_STRL("errCode"), value);
        }
        else if (stream_type == SW_HTTP2_STREAM_PIPELINE && !(flags & SW_HTTP2_FLAG_END_STREAM))
        {
            zend_update_property_bool(swoole_http2_response_ce, zresponse, ZEND_STRL("pipeline"), 1);
        }

        if (stream->buffer)
        {
            zend_update_property_stringl(swoole_http2_response_ce, stream->response_object, ZEND_STRL("data"), stream->buffer->str, stream->buffer->length);
            swString_clear(stream->buffer);
        }

        Z_ADDREF_P(zresponse);
        swHashMap_del_int(streams, stream_id);
        RETVAL_ZVAL(zresponse, 0, 0);
        return SW_READY;
    }

    return SW_CONTINUE;
}


#ifdef SW_HAVE_ZLIB
int php_swoole_zlib_uncompress(z_stream *stream, swString *buffer, char *body, int length)
{
    int status = 0;

    stream->avail_in = length;
    stream->next_in = (Bytef *) body;
    stream->total_in = 0;
    stream->total_out = 0;

#if 0
    printf(SW_START_LINE"\nstatus=%d\tavail_in=%ld,\tavail_out=%ld,\ttotal_in=%ld,\ttotal_out=%ld\n", status,
            stream->avail_in, stream->avail_out, stream->total_in, stream->total_out);
#endif

    swString_clear(buffer);

    while (1)
    {
        stream->avail_out = buffer->size - buffer->length;
        stream->next_out = (Bytef *) (buffer->str + buffer->length);

        status = inflate(stream, Z_SYNC_FLUSH);

#if 0
        printf("status=%d\tavail_in=%ld,\tavail_out=%ld,\ttotal_in=%ld,\ttotal_out=%ld,\tlength=%ld\n", status,
                stream->avail_in, stream->avail_out, stream->total_in, stream->total_out, buffer->length);
#endif
        if (status >= 0)
        {
            buffer->length = stream->total_out;
        }
        if (status == Z_STREAM_END)
        {
            return SW_OK;
        }
        else if (status == Z_OK)
        {
            if (buffer->length + 4096 >= buffer->size)
            {
                if (swString_extend(buffer, buffer->size * 2) < 0)
                {
                    return SW_ERR;
                }
            }
            if (stream->avail_in == 0)
            {
                return SW_OK;
            }
        }
        else
        {
            return SW_ERR;
        }
    }
    return SW_ERR;
}
#endif

static PHP_METHOD(swoole_http2_client_coro, __construct)
{
    char *host;
    size_t host_len;
    zend_long port = 80;
    zend_bool ssl = SW_FALSE;

    ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 3)
        Z_PARAM_STRING(host, host_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(port)
        Z_PARAM_BOOL(ssl)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    if (host_len == 0)
    {
        zend_throw_exception(swoole_http2_client_coro_exception_ce, "host is empty", SW_ERROR_INVALID_PARAMS);
        RETURN_FALSE;
    }

    http2_client *h2c = new http2_client(host, host_len, port, getThis());
    if (ssl)
    {
#ifdef SW_USE_OPENSSL
        h2c->ssl = 1;
#else
        zend_throw_exception_ex(
            swoole_http2_client_coro_exception_ce,
            EPROTONOSUPPORT, "you must configure with `enable-openssl` to support ssl connection"
        );
        RETURN_FALSE;
#endif
    }

    swoole_set_object(getThis(), h2c);

    zend_update_property_stringl(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("host"), host, host_len);
    zend_update_property_long(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("port"), port);
}

static PHP_METHOD(swoole_http2_client_coro, set)
{
    zval *zset;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &zset) == FAILURE)
    {
        RETURN_FALSE;
    }
    if (Z_TYPE_P(zset) != IS_ARRAY)
    {
        RETURN_FALSE;
    }
    zval *zsetting = sw_zend_read_property_array(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("setting"), 0);
    php_array_merge(Z_ARRVAL_P(zsetting), Z_ARRVAL_P(zset));
    RETURN_TRUE;
}

static ssize_t http2_client_send_window_update(Socket *cli, int stream_id, uint32_t size)
{
    char frame[SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_WINDOW_UPDATE_SIZE];
    swTraceLog(SW_TRACE_HTTP2, "[" SW_ECHO_YELLOW "] stream_id=%d, size=%d", "WINDOW_UPDATE", stream_id, size);
    *(uint32_t*) ((char *)frame + SW_HTTP2_FRAME_HEADER_SIZE) = htonl(size);
    swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_WINDOW_UPDATE, SW_HTTP2_WINDOW_UPDATE_SIZE, 0, stream_id);
    return cli->send_all(frame, SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_WINDOW_UPDATE_SIZE);
}

static ssize_t http2_client_send_setting(Socket *cli, swHttp2_settings *settings)
{
    uint16_t id = 0;
    uint32_t value = 0;

    char frame[SW_HTTP2_FRAME_HEADER_SIZE + 18];
    memset(frame, 0, sizeof(frame));
    swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_SETTINGS, 18, 0, 0);

    char *p = frame + SW_HTTP2_FRAME_HEADER_SIZE;
    /**
     * MAX_CONCURRENT_STREAMS
     */
    id = htons(SW_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
    memcpy(p, &id, sizeof(id));
    p += 2;
    value = htonl(settings->max_concurrent_streams);
    memcpy(p, &value, sizeof(value));
    p += 4;
    /**
     * MAX_FRAME_SIZE
     */
    id = htons(SW_HTTP2_SETTINGS_MAX_FRAME_SIZE);
    memcpy(p, &id, sizeof(id));
    p += 2;
    value = htonl(settings->max_frame_size);
    memcpy(p, &value, sizeof(value));
    p += 4;
    /**
     * INIT_WINDOW_SIZE
     */
    id = htons(SW_HTTP2_SETTINGS_INIT_WINDOW_SIZE);
    memcpy(p, &id, sizeof(id));
    p += 2;
    value = htonl(settings->window_size);
    memcpy(p, &value, sizeof(value));
    p += 4;

    swTraceLog(SW_TRACE_HTTP2, "[" SW_ECHO_GREEN "]\t[length=%d]", swHttp2_get_type(SW_HTTP2_TYPE_SETTINGS), 18);
    return cli->send_all(frame, SW_HTTP2_FRAME_HEADER_SIZE + 18);
}

int http2_client::parse_header(http2_client_stream *stream , int flags, char *in, size_t inlen)
{
    zval *zresponse = stream->response_object;

    if (flags & SW_HTTP2_FLAG_PRIORITY)
    {
        //int stream_deps = ntohl(*(int *) (in));
        //uint8_t weight = in[4];
        in += 5;
        inlen -= 5;
    }

    zval *zheaders = sw_zend_read_property_array(swoole_http2_response_ce, zresponse, ZEND_STRL("headers"), 0);
    zval *zcookies = sw_zend_read_property_array(swoole_http2_response_ce, zresponse, ZEND_STRL("cookies"), 0);
    zval *zset_cookie_headers = sw_zend_read_property_array(swoole_http2_response_ce, zresponse, ZEND_STRL("set_cookie_headers"), 0);

    ssize_t rv;
    for (;;)
    {
        nghttp2_nv nv;
        int inflate_flags = 0;
        size_t proclen;

        rv = nghttp2_hd_inflate_hd(inflater, &nv, &inflate_flags, (uchar *) in, inlen, 1);
        if (rv < 0)
        {
            swoole_php_error(E_WARNING, "inflate failed, Error: %s[%zd]", nghttp2_strerror(rv), rv);
            return SW_ERR;
        }

        proclen = (size_t) rv;
        in += proclen;
        inlen -= proclen;

        //swTraceLog(SW_TRACE_HTTP2, "Header: %s[%d]: %s[%d]", nv.name, nv.namelen, nv.value, nv.valuelen);

        if (inflate_flags & NGHTTP2_HD_INFLATE_EMIT)
        {
            if (nv.name[0] == ':')
            {
                if (strncasecmp((char *) nv.name + 1, "status", nv.namelen -1) == 0)
                {
                    zend_update_property_long(swoole_http2_response_ce, zresponse, ZEND_STRL("statusCode"), atoi((char *) nv.value));
                    continue;
                }
            }
#ifdef SW_HAVE_ZLIB
            else if (strncasecmp((char *) nv.name, "content-encoding", nv.namelen) == 0 && strncasecmp((char *) nv.value, "gzip", nv.valuelen) == 0)
            {
                http2_client_init_gzip_stream(stream);
                if (Z_OK != inflateInit2(&stream->gzip_stream, MAX_WBITS + 16))
                {
                    swWarn("inflateInit2() failed");
                    return SW_ERR;
                }
            }
#endif
            else if (strncasecmp((char *) nv.name, "set-cookie", nv.namelen) == 0)
            {
                if (SW_OK != http_parse_set_cookies((char *) nv.value, nv.valuelen, zcookies, zset_cookie_headers))
                {
                    return SW_ERR;
                }
            }
            add_assoc_stringl_ex(zheaders, (char *) nv.name, nv.namelen, (char *) nv.value, nv.valuelen);
        }

        if (inflate_flags & NGHTTP2_HD_INFLATE_FINAL)
        {
            nghttp2_hd_inflate_end_headers(inflater);
            break;
        }

        if ((inflate_flags & NGHTTP2_HD_INFLATE_EMIT) == 0 && inlen == 0)
        {
            break;
        }
    }

    return SW_OK;
}

static ssize_t http2_client_build_header(zval *zobject, zval *zrequest, char *buffer)
{
    http2_client *h2c = (http2_client *) swoole_get_object(zobject);
    zval *zmethod = sw_zend_read_property(swoole_http2_request_ce, zrequest, ZEND_STRL("method"), 0);
    zval *zpath = sw_zend_read_property(swoole_http2_request_ce, zrequest, ZEND_STRL("path"), 0);
    zval *zheaders = sw_zend_read_property(swoole_http2_request_ce, zrequest, ZEND_STRL("headers"), 0);
    zval *zcookies = sw_zend_read_property(swoole_http2_request_ce, zrequest, ZEND_STRL("cookies"), 0);
    http2::headers headers(8 + php_swoole_array_length_safe(zheaders) + php_swoole_array_length_safe(zcookies));
    bool find_host = 0;

    if (Z_TYPE_P(zmethod) != IS_STRING || Z_STRLEN_P(zmethod) == 0)
    {
        headers.add(ZEND_STRL(":method"), ZEND_STRL("GET"));
    }
    else
    {
        headers.add(ZEND_STRL(":method"), Z_STRVAL_P(zmethod), Z_STRLEN_P(zmethod));
    }
    if (Z_TYPE_P(zpath) != IS_STRING || Z_STRLEN_P(zpath) == 0)
    {
        headers.add(ZEND_STRL(":path"), "/", 1);
    }
    else
    {
        headers.add(ZEND_STRL(":path"), Z_STRVAL_P(zpath), Z_STRLEN_P(zpath));
    }
    if (h2c->ssl)
    {
        headers.add(ZEND_STRL(":scheme"), ZEND_STRL("https"));
    }
    else
    {
        headers.add(ZEND_STRL(":scheme"), ZEND_STRL("http"));
    }
    // Host
    headers.reserve_one();

    if (Z_TYPE_P(zheaders) == IS_ARRAY)
    {
        HashTable *ht = Z_ARRVAL_P(zheaders);
        zend_string *key = NULL;
        zval *value = NULL;

        ZEND_HASH_FOREACH_STR_KEY_VAL(ht, key, value)
        {
            zend::string str_value(value);
            if (UNEXPECTED(!key || *ZSTR_VAL(key) == ':' || str_value.len() == 0))
            {
                continue;
            }
            if (strncasecmp("host", ZSTR_VAL(key), ZSTR_LEN(key)) == 0)
            {
                headers.add(HTTP2_CLIENT_HOST_HEADER_INDEX, ZEND_STRL(":authority"), str_value.val(), str_value.len());
                find_host = true;
            }
            else
            {
                headers.add(ZSTR_VAL(key), ZSTR_LEN(key), str_value.val(), str_value.len());
            }
        }
        ZEND_HASH_FOREACH_END();
    }
    if (!find_host)
    {
        headers.add(HTTP2_CLIENT_HOST_HEADER_INDEX,ZEND_STRL(":authority"), h2c->host, h2c->host_len);
    }

    // http cookies
    if (Z_TYPE_P(zcookies) == IS_ARRAY)
    {
        zend_string *key;
        zval *value = NULL;
        char *encoded_value;
        int encoded_value_len;
        swString *buffer = SwooleTG.buffer_stack;

        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(zcookies), key, value)
            if (UNEXPECTED(!key))
            {
                continue;
            }
            zend::string str_value(value);
            if (str_value.len() == 0)
            {
                continue;
            }
            swString_clear(buffer);
            swString_append_ptr(buffer, ZSTR_VAL(key), ZSTR_LEN(key));
            swString_append_ptr(buffer, "=", 1);
            encoded_value = sw_php_url_encode(str_value.val(), str_value.len(), &encoded_value_len);
            if (encoded_value)
            {
                swString_append_ptr(buffer, encoded_value, encoded_value_len);
                efree(encoded_value);
                headers.add(ZEND_STRL("cookie"), buffer->str, buffer->length);
            }
        ZEND_HASH_FOREACH_END();
    }

    size_t buflen = nghttp2_hd_deflate_bound(h2c->deflater, headers.get(), headers.len());
    if (buflen > h2c->remote_settings.max_header_list_size)
    {
        swoole_php_error(E_WARNING, "header cannot bigger than remote max_header_list_size %u", h2c->remote_settings.max_header_list_size);
        return -1;
    }
    ssize_t rv = nghttp2_hd_deflate_hd(h2c->deflater, (uchar *) buffer, buflen, headers.get(), headers.len());
    if (rv < 0)
    {
        swWarn("nghttp2_hd_deflate_hd() failed with error: %s", nghttp2_strerror((int) rv));
        return -1;
    }
    return rv;
}


static void http2_client_stream_free(void *ptr)
{
    http2_client_stream *stream = (http2_client_stream *) ptr;
    if (stream->buffer)
    {
        swString_free(stream->buffer);
    }
#ifdef SW_HAVE_ZLIB
    if (stream->gzip)
    {
        inflateEnd(&stream->gzip_stream);
        swString_free(stream->gzip_buffer);
    }
#endif
    if (stream->response_object)
    {
        zval_ptr_dtor(stream->response_object);
        stream->response_object = NULL;
    }
    efree(stream);
}

static uint32_t http2_client_send_request(zval *zobject, zval *req)
{
    http2_client *h2c = (http2_client *) swoole_get_object(zobject);
    Socket *cli = h2c->client;
    ssize_t length;

    zval *zheaders = sw_zend_read_property_array(swoole_http2_request_ce, req, ZEND_STRL("headers"), 0);
    zval *zdata = sw_zend_read_property(swoole_http2_request_ce, req, ZEND_STRL("data"), 0);
    zval *zpipeline = sw_zend_read_property(swoole_http2_request_ce, req, ZEND_STRL("pipeline"), 0);
    bool is_data_empty = !zend_is_true(zdata);

    if (Z_TYPE_P(zdata) == IS_ARRAY)
    {
        add_assoc_stringl_ex(zheaders, ZEND_STRL("content-type"), (char *) ZEND_STRL("application/x-www-form-urlencoded"));
    }

    /**
     * send header
     */
    char* buffer = SwooleTG.buffer_stack->str;
    length = http2_client_build_header(zobject, req, buffer + SW_HTTP2_FRAME_HEADER_SIZE);
    if (length <= 0)
    {
        swWarn("http2_client_build_header() failed");
        return 0;
    }

    // malloc
    http2_client_stream *stream = (http2_client_stream *) ecalloc(1, sizeof(http2_client_stream));
    // init
    stream->response_object = &stream->_response_object;
    object_init_ex(stream->response_object, swoole_http2_response_ce);
    stream->stream_id = h2c->stream_id;
    stream->type = Z_BVAL_P(zpipeline) ? SW_HTTP2_STREAM_PIPELINE : SW_HTTP2_STREAM_NORMAL;
    stream->remote_window_size = SW_HTTP2_DEFAULT_WINDOW_SIZE;
    stream->local_window_size = SW_HTTP2_DEFAULT_WINDOW_SIZE;

    // add to map
    swHashMap_add_int(h2c->streams, stream->stream_id, stream);

    if (is_data_empty)
    {
        //pipeline
        if (stream->type == SW_HTTP2_STREAM_PIPELINE)
        {
            swHttp2_set_frame_header(buffer, SW_HTTP2_TYPE_HEADERS, length, SW_HTTP2_FLAG_END_HEADERS, stream->stream_id);
        }
        else
        {
            swHttp2_set_frame_header(buffer, SW_HTTP2_TYPE_HEADERS, length, SW_HTTP2_FLAG_END_STREAM | SW_HTTP2_FLAG_END_HEADERS, stream->stream_id);
        }
    }
    else
    {
        swHttp2_set_frame_header(buffer, SW_HTTP2_TYPE_HEADERS, length, SW_HTTP2_FLAG_END_HEADERS, stream->stream_id);
    }

    zend_update_property_long(swoole_http2_response_ce, stream->response_object, ZEND_STRL("streamId"), stream->stream_id);

    swTraceLog(SW_TRACE_HTTP2, "[" SW_ECHO_GREEN ", STREAM#%d] length=%zd", swHttp2_get_type(SW_HTTP2_TYPE_HEADERS), stream->stream_id, length);
    if (!cli->send_all(buffer, length + SW_HTTP2_FRAME_HEADER_SIZE))
    {
        return 0;
    }

    /**
     * send body
     */
    if (!is_data_empty)
    {
        char *p;
        size_t len;
        smart_str formstr_s = { NULL, 0 };
        uint8_t send_flag;
        uint32_t send_len;
        zend::string str_zpost_data;

        int flag = stream->type == SW_HTTP2_STREAM_PIPELINE ? 0 : SW_HTTP2_FLAG_END_STREAM;
        if (Z_TYPE_P(zdata) == IS_ARRAY)
        {
            p = sw_http_build_query(zdata, &len, &formstr_s);
            if (p == NULL)
            {
                swoole_php_error(E_WARNING, "http_build_query failed");
                return 0;
            }
        }
        else
        {
            str_zpost_data = zdata;
            p = str_zpost_data.val();
            len = str_zpost_data.len();
        }

        swTraceLog(SW_TRACE_HTTP2, "[" SW_ECHO_GREEN ", END, STREAM#%d] length=%zu", swHttp2_get_type(SW_HTTP2_TYPE_DATA), stream->stream_id, len);

        while (len > 0)
        {
            if (len > h2c->remote_settings.max_frame_size)
            {
                send_len = h2c->remote_settings.max_frame_size;
                send_flag = 0;
            }
            else
            {
                send_len = len;
                send_flag = flag;
            }
            swHttp2_set_frame_header(buffer, SW_HTTP2_TYPE_DATA, send_len, send_flag, stream->stream_id);
            if (cli->send_all(buffer, SW_HTTP2_FRAME_HEADER_SIZE) < 0)
            {
                return 0;
            }
            if (cli->send_all(p, send_len) < 0)
            {
                return 0;
            }
            len -= send_len;
            p += send_len;
        }

        if (formstr_s.s)
        {
            smart_str_free(&formstr_s);
        }
    }

    h2c->stream_id += 2;

    return stream->stream_id;
}

static int http2_client_send_data(http2_client *h2c, uint32_t stream_id, zval *data, zend_bool end)
{
    Socket *cli = h2c->client;

    char buffer[SW_HTTP2_FRAME_HEADER_SIZE];
    http2_client_stream *stream = http2_client_stream_get(h2c, stream_id);
    if (stream == NULL || stream->type != SW_HTTP2_STREAM_PIPELINE)
    {
        return -1;
    }

    int flag = end ? SW_HTTP2_FLAG_END_STREAM : 0;

    if (Z_TYPE_P(data) == IS_ARRAY)
    {
        size_t len;
        smart_str formstr_s = { 0 };
        char *formstr = sw_http_build_query(data, &len, &formstr_s);
        if (formstr == NULL)
        {
            swoole_php_error(E_WARNING, "http_build_query failed");
            return -1;
        }
        memset(buffer, 0, SW_HTTP2_FRAME_HEADER_SIZE);
        swHttp2_set_frame_header(buffer, SW_HTTP2_TYPE_DATA, len, flag, stream_id);
        swTraceLog(SW_TRACE_HTTP2, "[" SW_ECHO_GREEN ", END, STREAM#%d] length=%zu", swHttp2_get_type(SW_HTTP2_TYPE_DATA), stream_id, len);
        cli->send_all(buffer, SW_HTTP2_FRAME_HEADER_SIZE);
        cli->send_all(formstr, len);
        smart_str_free(&formstr_s);
    }
    else if (Z_TYPE_P(data) == IS_STRING)
    {
        swHttp2_set_frame_header(buffer, SW_HTTP2_TYPE_DATA, Z_STRLEN_P(data), flag, stream_id);
        swTraceLog(SW_TRACE_HTTP2, "[" SW_ECHO_GREEN ", END, STREAM#%d] length=%zu", swHttp2_get_type(SW_HTTP2_TYPE_DATA), stream_id, Z_STRLEN_P(data));
        cli->send_all(buffer, SW_HTTP2_FRAME_HEADER_SIZE);
        cli->send_all(Z_STRVAL_P(data), Z_STRLEN_P(data));
    }
    else
    {
        swoole_php_error(E_WARNING, "unknown data type[%d]", Z_TYPE_P(data) );
        return -1;
    }
    return SW_OK;
}

static PHP_METHOD(swoole_http2_client_coro, send)
{
    zval *request;
    http2_client *h2c = (http2_client *) swoole_get_object(getThis());
    if (!h2c->streams)
    {
        zend_update_property_long(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errCode"), (SwooleG.error = SW_ERROR_CLIENT_NO_CONNECTION));
        zend_update_property_string(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errMsg"), "client is not connected to server");
        swoole_php_error(E_WARNING, "client is not connected to server");
        RETURN_FALSE;
    }
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &request) == FAILURE)
    {
        RETURN_FALSE;
    }
    if (Z_TYPE_P(request) != IS_OBJECT || !instanceof_function(Z_OBJCE_P(request), swoole_http2_request_ce))
    {
        swoole_php_fatal_error(E_ERROR, "object is not instanceof swoole_http2_request");
        RETURN_FALSE;
    }

    uint32_t stream_id = http2_client_send_request(getThis(), request);
    if (stream_id == 0)
    {
        RETURN_FALSE;
    }
    else
    {
        RETURN_LONG(stream_id);
    }
}

static PHP_METHOD(swoole_http2_client_coro, recv)
{
    http2_client *h2c = (http2_client *) swoole_get_object(getThis());
    Socket *cli = h2c->client;
    if (!h2c->streams)
    {
        zend_update_property_long(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errCode"), (SwooleG.error = SW_ERROR_CLIENT_NO_CONNECTION));
        zend_update_property_string(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errMsg"), "client is not connected to server");
        swoole_php_error(E_WARNING, "client is not connected to server");
        RETURN_FALSE;
    }

    double timeout = h2c->timeout;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "|d", &timeout) == FAILURE)
    {
        RETURN_FALSE;
    }

    for (;;)
    {
        ssize_t retval = cli->recv_packet(timeout);
        if (retval <= 0)
        {
            zend_update_property_long(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errCode"), cli->errCode);
            zend_update_property_string(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errMsg"), cli->errMsg);
            RETURN_FALSE;
        }

        enum swReturnType ret = h2c->parse_frame(return_value);
        if (ret == SW_CONTINUE)
        {
            continue;
        }
        else if (ret == SW_READY)
        {
            break;
        }
        else
        {
            printf("error\n");
            RETURN_FALSE;
        }
    }
}

static PHP_METHOD(swoole_http2_client_coro, __destruct)
{
    SW_PREVENT_USER_DESTRUCT();

    http2_client *h2c = (http2_client *) swoole_get_object(getThis());
    if (!h2c)
    {
        return;
    }
    delete h2c;
    swoole_set_object(getThis(), nullptr);
}

static PHP_METHOD(swoole_http2_client_coro, close)
{
    http2_client *h2c = (http2_client *) swoole_get_object(getThis());
    RETURN_BOOL(h2c->close());
}

static PHP_METHOD(swoole_http2_client_coro, connect)
{
    http2_client *h2c = (http2_client *) swoole_get_object(getThis());

    if (h2c->client)
    {
        swoole_php_fatal_error(E_WARNING, "The HTTP2 connection has already been established");
        RETURN_FALSE;
    }
    else
    {
        RETURN_BOOL(h2c->connect());
    }
}

static sw_inline void http2_settings_to_array(swHttp2_settings *settings, zval* zarray)
{
    array_init(zarray);
    add_assoc_long_ex(zarray, ZEND_STRL("header_table_size"), settings->header_table_size);
    add_assoc_long_ex(zarray, ZEND_STRL("window_size"), settings->window_size);
    add_assoc_long_ex(zarray, ZEND_STRL("max_concurrent_streams"), settings->max_concurrent_streams);
    add_assoc_long_ex(zarray, ZEND_STRL("max_frame_size"), settings->max_frame_size);
    add_assoc_long_ex(zarray, ZEND_STRL("max_header_list_size"), settings->max_header_list_size);
}

static PHP_METHOD(swoole_http2_client_coro, stats)
{
    http2_client *h2c = (http2_client *) swoole_get_object(getThis());
    zval _zarray, *zarray = &_zarray;
    swString key;
    bzero(&key, sizeof(key));
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "|s", &key.str, &key.length) == FAILURE)
    {
        RETURN_FALSE;
    }
    if (key.length > 0)
    {
        if (strcmp(key.str, "current_stream_id") == 0)
        {
            RETURN_LONG(h2c->stream_id);
        }
        else if (strcmp(key.str, "last_stream_id") == 0)
        {
            RETURN_LONG(h2c->last_stream_id);
        }
        else if (strcmp(key.str, "local_settings") == 0)
        {
            http2_settings_to_array(&h2c->local_settings, zarray);
            RETURN_ZVAL(zarray, 0, 0);
        }
        else if (strcmp(key.str, "remote_settings") == 0)
        {
            http2_settings_to_array(&h2c->remote_settings, zarray);
            RETURN_ZVAL(zarray, 0, 0);
        }
        else if (strcmp(key.str, "active_stream_num") == 0)
        {
            RETURN_LONG(h2c->streams ? swHashMap_count(h2c->streams) : 0);
        }
    }
    else
    {
        array_init(return_value);
        add_assoc_long_ex(return_value, ZEND_STRL("current_stream_id"), h2c->stream_id);
        add_assoc_long_ex(return_value, ZEND_STRL("last_stream_id"), h2c->last_stream_id);
        http2_settings_to_array(&h2c->local_settings, zarray);
        add_assoc_zval_ex(return_value, ZEND_STRL("local_settings"), zarray);
        http2_settings_to_array(&h2c->remote_settings, zarray);
        add_assoc_zval_ex(return_value, ZEND_STRL("remote_settings"), zarray);
        add_assoc_long_ex(return_value, ZEND_STRL("active_stream_num"), h2c->streams ? swHashMap_count(h2c->streams) : 0);
    }
}

static PHP_METHOD(swoole_http2_client_coro, isStreamExist)
{
    zend_long stream_id = 0;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &stream_id) == FAILURE)
    {
        RETURN_FALSE;
    }
    if (stream_id < 0)
    {
        RETURN_FALSE;
    }

    http2_client *h2c = (http2_client *) swoole_get_object(getThis());
    if (!h2c->client)
    {
        RETURN_FALSE;
    }
    else
    {
        if (stream_id == 0)
        {
            RETURN_TRUE;
        }
        if (!h2c->streams)
        {
            RETURN_FALSE;
        }
    }
    http2_client_stream *stream = http2_client_stream_get(h2c, stream_id);
    RETURN_BOOL(stream ? 1 : 0);
}

static PHP_METHOD(swoole_http2_client_coro, write)
{
    http2_client *h2c = (http2_client *) swoole_get_object(getThis());

    if (!h2c->streams)
    {
        zend_update_property_long(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errCode"), (SwooleG.error = SW_ERROR_CLIENT_NO_CONNECTION));
        zend_update_property_string(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errMsg"), "client is not connected to server");
        swoole_php_error(E_WARNING, "client is not connected to server");
        RETURN_FALSE;
    }

    long stream_id;
    zval *data;
    zend_bool end = 0;
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "lz|b", &stream_id, &data, &end) == FAILURE)
    {
        RETURN_FALSE;
    }
    SW_CHECK_RETURN(http2_client_send_data(h2c, stream_id, data, end));
}

static PHP_METHOD(swoole_http2_client_coro, ping)
{
    http2_client *h2c = (http2_client *) swoole_get_object(getThis());
    Socket *cli = h2c->client;

    if (!h2c->streams)
    {
        zend_update_property_long(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errCode"), (SwooleG.error = SW_ERROR_CLIENT_NO_CONNECTION));
        zend_update_property_string(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errMsg"), "client is not connected to server");
        swoole_php_error(E_WARNING, "client is not connected to server");
        RETURN_FALSE;
    }

    char frame[SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_FRAME_PING_PAYLOAD_SIZE];
    swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_PING, SW_HTTP2_FRAME_PING_PAYLOAD_SIZE, SW_HTTP2_FLAG_NONE, 0);
    SW_CHECK_RETURN(cli->send_all(frame, SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_FRAME_PING_PAYLOAD_SIZE));
}

/**
 * +-+-------------------------------------------------------------+
 * |R|                  Last-Stream-ID (31)                        |
 * +-+-------------------------------------------------------------+
 * |                      Error Code (32)                          |
 * +---------------------------------------------------------------+
 * |                  Additional Debug Data (*)                    |
 * +---------------------------------------------------------------+
 */
static PHP_METHOD(swoole_http2_client_coro, goaway)
{
    http2_client *h2c = (http2_client *) swoole_get_object(getThis());
    Socket *cli = h2c->client;

    int ret;
    char* frame;
    uint8_t error_code = SW_HTTP2_ERROR_NO_ERROR;
    char* debug_data = NULL;
    long  debug_data_len = 0;

    if (!h2c->streams)
    {
        zend_update_property_long(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errCode"), (SwooleG.error = SW_ERROR_CLIENT_NO_CONNECTION));
        zend_update_property_string(swoole_http2_client_coro_ce, getThis(), ZEND_STRL("errMsg"), "client is not connected to server");
        swoole_php_error(E_WARNING, "client is not connected to server");
        RETURN_FALSE;
    }

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "|ls", &error_code, &debug_data, &debug_data_len) == FAILURE)
    {
        RETURN_FALSE;
    }

    size_t length = SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_GOAWAY_SIZE + debug_data_len;
    frame = (char *) emalloc(length);
    bzero(frame, length);
    swHttp2_set_frame_header(frame, SW_HTTP2_TYPE_GOAWAY, SW_HTTP2_GOAWAY_SIZE + debug_data_len, error_code, 0);
    *(uint32_t*) (frame + SW_HTTP2_FRAME_HEADER_SIZE) = htonl(h2c->last_stream_id);
    *(uint32_t*) (frame + SW_HTTP2_FRAME_HEADER_SIZE + 4) = htonl(error_code);
    memcpy(frame + SW_HTTP2_FRAME_HEADER_SIZE + SW_HTTP2_GOAWAY_SIZE, debug_data, debug_data_len);
    swTraceLog(SW_TRACE_HTTP2, "[" SW_ECHO_GREEN "] Send: last-sid=%d, error-code=%d", swHttp2_get_type(SW_HTTP2_TYPE_GOAWAY), h2c->last_stream_id, error_code);

    ret = cli->send_all(frame, length);
    efree(frame);
    SW_CHECK_RETURN(ret);
}

#endif
