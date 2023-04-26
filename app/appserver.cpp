#include "../include/api.h"
#include "config.h"
#include <string.h>
#include <sys/time.h>
#undef LOG_LEVEL
#define LOG_LEVEL DEBUG

char serverAddr[20] = "10.0.100.2";

int provideData(IUINT32 start, IUINT32 end, void *_sessPtr){
    LOG(TRACE,"insert [%d,%d)",start,end);
    LeotpSess *sessPtr = (LeotpSess*)_sessPtr;
    char *dataBuf = new char[end-start];
    int pos =0;
    memset(dataBuf,0,end-start);
    while(1){
        if(pos+REQ_LEN>end-start)
            break;
        *((IUINT32 *)(dataBuf+pos)) = _getMillisec();
        pos+=REQ_LEN;
    }
    sessPtr->insertData(dataBuf,start,end);
    delete dataBuf;
    
    return 0;
}

void *onNewSess(void* _sessPtr){
    // do nothing
    LOG(INFO,"");
    return nullptr;
}

int main(int argc,char** argv){
    int ch;
    while((ch=getopt(argc,argv,"s:"))!=-1){
        switch(ch){
            case 's':
                strncpy(serverAddr,optarg, 19);
                break;
            default:
                printf("unkown option\n");
                break;
        }
    }

    flushBeforeExit();
    Cache cache(QUAD_STR_LEN);
    ByteMap<shared_ptr<LeotpSess>> sessMap;
    LOG(INFO,"entering LEOTP appserver");
    fflush(stdout);
    startResponder(&cache,&sessMap,onNewSess,provideData,
            (const char*)serverAddr, DEFAULT_SERVER_PORT);

    // udpRecvLoop(&args);
    return 0;
}
