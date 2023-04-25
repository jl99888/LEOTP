#include "../include/api.h"
#include "config.h"
#include <string.h>
#include <sys/time.h>
#undef LOG_LEVEL
#define LOG_LEVEL DEBUG


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
    LOG(INFO,"");
    /*
    IntcpSess *sessPtr = (IntcpSess*)_sessPtr;
    char dataBuf[TOTAL_DATA_LEN];
    
    int start = 0;
    while(1){
         memset(dataBuf,0,REQ_LEN);
         *((IUINT32 *)dataBuf) = getMillisec();
         sessPtr->insertData(dataBuf,start,start+REQ_LEN);
         LOG(TRACE,"insert %d %d\n",start,start+REQ_LEN);
         start += REQ_LEN;
         usleep(1*1000*REQ_INTV);
     }
    */
    return nullptr;
}

int main(){
    flushBeforeExit();
    Cache cache(QUAD_STR_LEN);
    ByteMap<shared_ptr<LeotpSess>> sessMap;
    LOG(INFO,"entering intcps");
    fflush(stdout);
    startResponder(&cache,&sessMap,onNewSess,provideData,
            "10.0.100.2", DEFAULT_SERVER_PORT);

    // udpRecvLoop(&args);
    return 0;
}
