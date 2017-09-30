#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "php.h"
#include "ext/standard/info.h"
#include "php_network.h"
#include "php_foolsock.h"

#define PHP_FOOLSOCK_NAME "foolsock persistent connection"

static int le_type_foolsock;

zend_class_entry *foolsock_ce;

// functions 不在类里面(全局的)
const zend_function_entry foolsock_functions[] = {
    PHP_FE_END
};

// 定义方法列表
const zend_function_entry foolsock_methods[] = {
    ZEND_ME(foolsock, __construct, NULL, ZEND_ACC_PUBLIC)
    ZEND_ME(foolsock, pconnect, NULL, ZEND_ACC_PUBLIC)
    ZEND_ME(foolsock, read, NULL, ZEND_ACC_PUBLIC)
    ZEND_ME(foolsock, write, NULL, ZEND_ACC_PUBLIC)
    ZEND_ME(foolsock, pclose, NULL, ZEND_ACC_PUBLIC)
    PHP_FE_END
};


zend_module_entry foolsock_module_entry = {
    STANDARD_MODULE_HEADER, // Module的头部信息
    "foolsock",             // Module的名字
    foolsock_functions,     // 方法列表
    PHP_MINIT(foolsock),
    PHP_MSHUTDOWN(foolsock),
    PHP_RINIT(foolsock),
    PHP_RSHUTDOWN(foolsock),
    PHP_MINFO(foolsock),
    PHP_FOOLSOCK_VERSION,
    STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_FOOLSOCK
ZEND_GET_MODULE(foolsock)
#endif

/*{{{ static foolsock_t* create_new_resource(char* host, unsigned short port TSRMLS_DC)
 */
static foolsock_t *create_new_resource(char *host, unsigned short port TSRMLS_DC) {

    // __zend_malloc 分配一个持久的内存
    size_t host_len = strlen(host);
    foolsock_t *resource = (foolsock_t *) pemalloc(sizeof(foolsock_t), 1);
    if (resource == NULL) {
        return NULL;
    }
    memset(resource, 0, sizeof(*resource));

    // 设置host/port
    resource->host = pemalloc(host_len + 1, 1);
    memcpy(resource->host, host, host_len);
    resource->host[host_len] = '\0';

    // 设置port
    resource->port = port;
    return resource;
}
/*}}}*/

/*{{{ static void foolsock_free(foolsock_t* fs TSRMLS_DC)
 */
static void foolsock_free(foolsock_t *fs TSRMLS_DC) {
    // 关闭stream
    if (fs->stream != NULL) {
        php_stream_pclose(fs->stream);
    }

    // 如何区分persistent 和 普通的xxx
    pefree(fs->host, 1);
    pefree(fs, 1);
}
/*}}}*/

/*{{{ static struct timeval convert_timeoutms_to_ts(long msecs)
 */
static struct timeval convert_timeoutms_to_ts(long msecs) {
    struct timeval tv;
    int secs = 0;

    // ms --> ts
    secs = (int) (msecs / 1000);
    tv.tv_sec = secs;
    tv.tv_usec = (int) (((msecs - (secs * 1000)) * 1000) % 1000000);
    return tv;
}
/*}}}*/

/*{{{ static int get_stream(foolsock_t* f_obj TSRMLS_DC)
 */
static int get_stream(foolsock_t *f_obj TSRMLS_DC) {

    // 如何实现持久操作?
    // Key的定义
    char *hash_key;
    spprintf(&hash_key, 0, "foolsock:%s:%d", f_obj->host, f_obj->port);

    // 根据hash_key获取持久化的连接
    switch (php_stream_from_persistent_id(hash_key, &(f_obj->stream) TSRMLS_CC)) {

        case PHP_STREAM_PERSISTENT_SUCCESS:
            // 判断是否出现EOF
            if (php_stream_eof(f_obj->stream)) {
                php_stream_pclose(f_obj->stream);
                f_obj->stream = NULL;
                break;
            }
        case PHP_STREAM_PERSISTENT_FAILURE:
            break;
        default:
            break;
    }

    struct timeval tv = convert_timeoutms_to_ts(f_obj->timeoutms);

    // 创建SocketStream
    if (!f_obj->stream) {
        int socktype = SOCK_STREAM;
        f_obj->stream = php_stream_sock_open_host(f_obj->host, f_obj->port, socktype, &tv, hash_key);
    }
    efree(hash_key);

    if (!f_obj->stream) {
        // 报告失败
        return 0;
    }

    php_stream_auto_cleanup(f_obj->stream);
    php_stream_set_option(f_obj->stream, PHP_STREAM_OPTION_READ_TIMEOUT, 0, &tv);
    php_stream_set_option(f_obj->stream, PHP_STREAM_OPTION_WRITE_BUFFER, PHP_STREAM_BUFFER_NONE, NULL);
    php_stream_set_chunk_size(f_obj->stream, 8192);

    return 1;
}
/*}}}*/


/*{{{ public function FoolSock::__construct(string $host, string $port)
 */
PHP_METHOD (foolsock, __construct) {
    long port;
    char *host;
    int host_len;

    // 读取 port, host参数
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sl|l", &host, &host_len, &port) == FAILURE) {
        RETURN_FALSE;
    }

    int re_conn = 0;
    zend_resource *le;

    foolsock_t *f_obj = NULL;
    zend_string *hash_key_str;

    hash_key_str = strpprintf(100, "foolsock_connect:%s:%d", host, port);

    HashTable *ht = &EG(persistent_list);

    // 从全局的 persistent_list 中读取: hash_key
    le = (zend_resource *) zend_hash_find(ht, hash_key_str);

    if ((le == NULL) || (le->type != le_type_foolsock || le->ptr == NULL)) {
        if (le != NULL) {
            // 0. 删除hash对应的数据(Just in case)
            zend_hash_del(ht, hash_key_str);
            le = NULL;
        }
        // 1. 读取失败，则创建新的f_obj, 并且添加到hashmap中
        f_obj = create_new_resource(host, (unsigned short)port TSRMLS_CC);
        if (NULL == f_obj) {
            zend_string_release(hash_key_str);
            RETURN_FALSE;
        }

        // 2. 创建资源
        zend_resource new_le;
        new_le.type = le_type_foolsock;
        new_le.ptr = f_obj;

        zend_hash_update(ht, hash_key_str, &new_le);
        add_property_resource(getThis(), CLASS_PROPERTY_RESOURCE, &new_le);

        // 重连
        re_conn = 1;

    } else {
        // 直接读取已有的结果
        f_obj = (foolsock_t *) le->ptr;
        // 如何将this和resource_id关联呢?
        add_property_resource(getThis(), CLASS_PROPERTY_RESOURCE, le);
    }

    zend_string_release(hash_key_str);

}
/*}}}*/

/*{{{ public function FoolSock::pconnect([int $timeoutms])
 */
PHP_METHOD (foolsock, pconnect) {
    zval *resource;
    int resource_type;
    foolsock_t *f_obj;
    long timeoutms = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &timeoutms) == FAILURE) {
        RETURN_FALSE;
    }

    // 1. 读取this的_resource属性
    zval rv;
    resource = zend_read_property(foolsock_ce, getThis(), ZEND_STRL(CLASS_PROPERTY_RESOURCE), false, &rv);
    if (resource == NULL) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid Resource");
        RETURN_FALSE;
    }

    // 2. 读取其中的socket
    //    需要先通过connection来调用
    f_obj = (foolsock_t *) Z_RES_P(resource)->ptr;

    // 检查是否为空
    if (f_obj == NULL) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid Resource");
        RETURN_FALSE;
    }

    // 检查类型
    if (Z_RES_P(resource)->type != le_type_foolsock) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid Resource Type");
        RETURN_FALSE;
    }

    // 设置timeout ms
    f_obj->timeoutms = timeoutms;

    // 如何获取stream呢?
    int stream_r = get_stream(f_obj TSRMLS_CC);
    if (!stream_r) {
        RETURN_FALSE;
    }
    RETURN_TRUE;
}
/*}}}*/

/*{{{ public function FoolSock::write(string $msg)
 */
PHP_METHOD (foolsock, write) {
    zval *resource;
    foolsock_t *f_obj;
    char *msg;
    int msg_len, res;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &msg, &msg_len) == FAILURE) {
        RETURN_FALSE;
    }

    zval rv;
    resource = zend_read_property(foolsock_ce, getThis(), ZEND_STRL(CLASS_PROPERTY_RESOURCE), 0, &rv);
    if (resource == NULL) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid Resource");
        RETURN_FALSE;
    }

    f_obj = (foolsock_t *) Z_RES_P(resource)->ptr;

    if (f_obj == NULL) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid Resource");
        RETURN_FALSE;
    }

    if (Z_RES_P(resource)->type != le_type_foolsock) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid Resource Type");
        RETURN_FALSE;
    }

    if (f_obj->stream == NULL) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Socket Not Connected");
        RETURN_FALSE;
    }

    // 写数据到stream中
    res = php_stream_write(f_obj->stream, msg, (size_t) msg_len);
    if (res != msg_len) {
        RETURN_FALSE;
    } else {
        RETURN_LONG(res);
    }
}
/*}}}*/

/*{{{ public function FoolSock::read(int $size)
 */
PHP_METHOD (foolsock, read) {
    long size;
    foolsock_t *f_obj;
    char *response_buf;
    zval *resource;

    // read($size)
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &size) == FAILURE) {
        RETURN_FALSE;
    }

    if (size <= 0) {
        RETURN_TRUE;
    }

    // CLASS_PROPERTY_RESOURCE --> resource
    zval rv;
    resource = zend_read_property(foolsock_ce, getThis(), ZEND_STRL(CLASS_PROPERTY_RESOURCE), 0, &rv);
    if (resource == NULL) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid Resource");
        RETURN_FALSE;
    }

    // resource --> f_obj
    f_obj = (foolsock_t *) Z_RES_P(resource)->ptr;

    if (f_obj == NULL) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid Resource");
        RETURN_FALSE;
    }

    if (Z_RES_P(resource)->type != le_type_foolsock) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid Resource Type");
        RETURN_FALSE;
    }

    if (f_obj->stream == NULL) {
        php_error_docref(NULL TSRMLS_CC, E_WARNING, "Socket Not Connected");
        RETURN_FALSE;
    }

    // php的内存管理?
    response_buf = emalloc(size + 1);
    int r = php_stream_read(f_obj->stream, response_buf, size);
    if (r <= 0) {
        if (errno == EAGAIN || errno == EINPROGRESS) {
            RETURN_TRUE;
        } else {
            RETURN_FALSE;
        }
    }
    response_buf[r] = '\0';
    RETURN_STRINGL(response_buf, r);
}
/*}}}*/

/*{{{ public function FoolSock::pclose()
 */
PHP_METHOD (foolsock, pclose) {
    zval *resource;
    foolsock_t *f_obj;

    zval rv;
    resource = zend_read_property(foolsock_ce, getThis(), ZEND_STRL(CLASS_PROPERTY_RESOURCE), 0, &rv);
    if (resource == NULL) {
        RETURN_TRUE;
    }

    // 找到 f_obj
    f_obj = (foolsock_t *) Z_RES_P(resource)->ptr;
    if (f_obj == NULL) {
        RETURN_TRUE;
    }

    // 如果资源类型不一致，则直接返回？？
    if (Z_RES_P(resource)->type != le_type_foolsock) {
        RETURN_TRUE;
    }

    // 关闭stream
    if (f_obj->stream != NULL) {
        php_stream_pclose(f_obj->stream);
        f_obj->stream = NULL;
    }

    RETURN_TRUE;
}

// 析构函数, unset(le_type_foolsock)时会被调用
static void foolsock_dtor(zend_resource *rsrc TSRMLS_DC) {


    foolsock_t *f_obj = (foolsock_t *) rsrc->ptr;
    if (rsrc->type == le_type_foolsock && f_obj != NULL) {
        // 关闭stream
        if (f_obj->stream != NULL) {
            php_stream_pclose(f_obj->stream);
            f_obj->stream = NULL;
        }
    }
    foolsock_free(f_obj TSRMLS_CC);
}

PHP_MINIT_FUNCTION (foolsock) {
    // 注册Methods
    zend_class_entry ce;
    INIT_CLASS_ENTRY(ce, "FoolSock", foolsock_methods);

    foolsock_ce = zend_register_internal_class(&ce TSRMLS_CC);

    // 返回资源变量
    // php中unset时，会调用: foolsock_dtor
    // 资源类型的变量在实现中也是有类型区分的！为了区分不同类型的资源，比如一个是文件句柄，一个是mysql链接，
    // 我们需要为其赋予不同的分类名称。首先，我们需要先把这个分类添加到程序中去。
    le_type_foolsock = zend_register_list_destructors_ex(
        NULL,           // 第一个回调函数会在脚本中的相应类型的资源变量被释放掉的时候触发，比如作用域结束了，或者被unset()掉了。
        foolsock_dtor,  // 第二个回调函数则是用在一个类似于长链接类型的资源上的，也就是这个资源创建后会一直存在于内存中，
        // 而不会在request结束后被释放掉。它将会在Web服务器进程终止时调用，相当于在MSHUTDOWN阶段被内核调用。
        PHP_FOOLSOCK_NAME, module_number);

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION (foolsock) {
    // Module Shutdown时，直接返回OK
    return SUCCESS;
}

PHP_RINIT_FUNCTION (foolsock) {
    // 请求开始时OK
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION (foolsock) {
    // 请求结束时OK
    return SUCCESS;
}

PHP_MINFO_FUNCTION (foolsock) {
    php_info_print_table_start();
    php_info_print_table_header(2, "foolsock support", "enabled");
    php_info_print_table_end();
}