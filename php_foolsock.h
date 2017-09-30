#ifndef PHP_FOOLSOCK_H
#define PHP_FOOLSOCK_H

// 声明 foolsock_module_entry，以及指针
extern zend_module_entry foolsock_module_entry;
#define phpext_foolsock_ptr &foolsock_module_entry

#define PHP_FOOLSOCK_VERSION "0.1.0"

#ifdef PHP_WIN32
#	define PHP_FOOLSOCK_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_FOOLSOCK_API __attribute__ ((visibility("default")))
#else
#	define PHP_FOOLSOCK_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

// 定义module的初始化函数&结束函数
PHP_MINIT_FUNCTION (foolsock);

PHP_MSHUTDOWN_FUNCTION (foolsock);

// 定义请求的初始化函数&结束函数
PHP_RINIT_FUNCTION (foolsock);

PHP_RSHUTDOWN_FUNCTION (foolsock);

//声明模块信息函数,即可以在phpinfo看到的信息
PHP_MINFO_FUNCTION (foolsock);

//构造函数，其他接口
PHP_METHOD (foolsock, __construct);

PHP_METHOD (foolsock, pconnect);

PHP_METHOD (foolsock, read);

PHP_METHOD (foolsock, write);

PHP_METHOD (foolsock, pclose);

#ifdef ZTS
#define FOOLSOCK_G(v) TSRMG(foolsock_globals_id, zend_foolsock_globals *, v)
#else
#define FOOLSOCK_G(v) (foolsock_globals.v)
#endif

#endif	/* PHP_FOOLSOCK_H */

#define CLASS_PROPERTY_RESOURCE "_resource"

typedef struct _foolsock_s {
    php_stream *stream;

    char *host;
    unsigned short port;

    long timeoutms;

} foolsock_t;
