/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ep.hh"
#include <memcached/extension.h>

extern "C" {
    typedef bool (*RESPONSE_HANDLER_T)(const void *, int , const char *);
}

class GetlExtension: public EXTENSION_ASCII_PROTOCOL_DESCRIPTOR {
public:
    GetlExtension(EventuallyPersistentStore *kvstore, GET_SERVER_API get_server_api);

    void initialize();

    bool executeGetl(int argc, token_t *argv, void *cookie,
                     RESPONSE_HANDLER_T response_handler);

private:
    SERVER_HANDLE_V1 *serverApi;
    EventuallyPersistentStore *backend;
};
