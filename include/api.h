#ifndef __API_H__
#define __API_H__

#include <csignal>
#include "udp_leotp.h"

int chdirProgramDir();
void startRequester(Cache *cachePtr, ByteMap<shared_ptr<LeotpSess>> *sessMapPtr, 
        void *(*onNewSess)(void* _sessPtr),
        const char* ipStrReq, const char* ipStrResp, uint16_t respPort);
void startResponder(Cache *cachePtr, ByteMap<shared_ptr<LeotpSess>> *sessMapPtr, 
        void *(*onNewSess)(void* _sessPtr), int (*onUnsatInt)(IUINT32 start, IUINT32 end, void *user),
        const char* ipStr, uint16_t port);
void startMidnode(Cache *cachePtr, ByteMap<shared_ptr<LeotpSess>> *sessMapPtr, 
        void *(*onNewSess)(void* _sessPtr),
        uint16_t port);

void flushBeforeExit();
        
#endif