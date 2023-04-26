#include "../include/api.h"
#undef LOG_LEVEL
#define LOG_LEVEL DEBUG

void *onNewSess(void* _sessPtr){
    LeotpSess *sessPtr = (LeotpSess*)_sessPtr;
    char reqIP[30],respIP[30];
    writeIPstr(reqIP, sessPtr->requesterAddr.sin_addr.s_addr);
    writeIPstr(respIP, sessPtr->responderAddr.sin_addr.s_addr);
    LOG(INFO,"new sess req %s:%d resp %s:%d",
            reqIP,
            ntohs(sessPtr->requesterAddr.sin_port),
            respIP,
            ntohs(sessPtr->responderAddr.sin_port)
    );
    
    char recvBuf[MaxBufSize];
    IUINT32 start,end;
    while(1){
        while(sessPtr->recvData(recvBuf, MaxBufSize, &start, &end) == 0){
            if(end-start>MaxBufSize){
                LOG(DEBUG,"abnormal range: rangeStart %u rangeEnd %u length %u",start,end,end-start);
            }
            else
                sessPtr->cachePtr->insert(sessPtr->nameChars,start,end,recvBuf);
        }
        usleep(1000);
    }
    
    return nullptr;
}

int main(int argc,char **argv){
    flushBeforeExit();
    if(argc==2 && argv[1][0]=='c'){
        chdirProgramDir();
        char cmd[50];
        sprintf(cmd,"iptables -t mangle -F");
        system(cmd);
        LOG(INFO,"ip table cleared.");
        return 0;
    }

    Cache cache(QUAD_STR_LEN);
    ByteMap<shared_ptr<LeotpSess>> sessMap;
    LOG(INFO,"entering LEOTP midnode\n");
    fflush(stdout);
    startMidnode(&cache,&sessMap,onNewSess,DEFAULT_MID_PORT);
    return 0;
}
