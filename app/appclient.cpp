#include "../include/api.h"
#include <unistd.h>
#include "config.h"
#include <thread>
#include <sys/time.h>
//#include <getopt.h>
#undef LOG_LEVEL
#define LOG_LEVEL DEBUG
//#define FLOW_TEST

using namespace std;

//extern char* optarg;
char clientAddr[20] = "10.0.1.1";

IUINT32 _round_up(IUINT32 x,IUINT32 y){
    return ((x+y-1)/y)*y;
}

void request_func(LeotpSess * _sessPtr){
    int sendStart = 0, ret;
    while(1){
#ifdef FLOW_TEST
        if(sendStart > TOTAL_DATA_LEN)
            break;
#endif
        //if(sendStart<10000)
        ret = _sessPtr->request(sendStart, sendStart+REQ_LEN);
        if(ret == -1){// intBuf is full
            LOG(TRACE,"intBuf is full");
        } else {
            LOG(TRACE,"request range [%d,%d)",sendStart,sendStart+REQ_LEN);
            sendStart += REQ_LEN;
        }
        usleep(1000*REQ_INTV);
    }
}

void *onNewSess(void* _sessPtr){
    LeotpSess *sessPtr = (LeotpSess*)_sessPtr;

    thread t(request_func,sessPtr);
    t.detach();
    
    int ret;
    char recvBuf[MaxBufSize];
    IUINT32 start,end;
    IUINT32 rcn=0;
    IUINT32 printTime = 0, startTime = _getMillisec();
    IUINT32 recvedBytes = 0;         //bytes
    const IUINT32 CheckInterval = 1000;

    int loops = 0;
    //DEBUG 1->0
    while(1){
        usleep(10);//sleep 0.01ms
        
        ret = sessPtr->recvData(recvBuf,MaxBufSize,&start,&end);
        if(ret==0)
            recvedBytes += (end-start);
        IUINT32 curTime = _getMillisec();
        if(printTime==0||curTime-printTime>CheckInterval){
            if(printTime!=0){
                //NOTE
                // printf("%4ds %.2f Mbits/sec receiver\n",
                //         int((curTime - startTime)/1000),
                //         bytesToMbit(recvedBytes)*1000/(curTime-printTime)
                // );
            }
            recvedBytes = 0;
            printTime = curTime;
        }
        if(ret<0)
            continue;
        recvBuf[end-start]='\0';
        
        
        IUINT32 pos = _round_up(start,REQ_LEN);
        while(1){
            if(pos+sizeof(IUINT32)*2>end)
                break;
            //LOG(TRACE,"%d %d %d\n",pos,start,end);
            IUINT32 sendTime = *((IUINT32 *)(recvBuf+pos-start));
            IUINT32 xmit = *((IUINT32 *)(recvBuf+pos-start+sizeof(IUINT32)));
            IUINT32 recvTime = *((IUINT32 *)(recvBuf+pos-start+sizeof(IUINT32)*2));
            IUINT32 firstTs = *((IUINT32 *)(recvBuf+pos-start+sizeof(IUINT32)*3));
            curTime = _getMillisec();
            // LOG(TRACE, "recv [%d,%d)\n", start, end);

            // if(recvTime<1000){
            //LOG(TRACE,"recv [%d,%d) xmit %u intcpRtt %u owd_noOrder %u sendTime %u recvTime %u curTime %u owd_obs %u\n",pos,pos+REQ_LEN,xmit,recvTime-firstTs,recvTime-sendTime,sendTime,recvTime,curTime, curTime-sendTime);
                // abort();
            // }
            fflush(stdout);
            pos += REQ_LEN;
        }
    }
    return nullptr;
}

int main(int argc,char** argv){
    int ch;
    
    while((ch=getopt(argc,argv,"c:"))!=-1){
        switch(ch){
            case 'c':
                strncpy(clientAddr,optarg,19);
                break;
            default:
                printf("unkown option\n");
                break;
        }
    }
    
    flushBeforeExit();
    Cache cache(QUAD_STR_LEN);
    ByteMap<shared_ptr<LeotpSess>> sessMap;
    LOG(INFO,"entering intcpc\n");
    LOG(INFO,"client ip:%s\n",clientAddr);
    startRequester(&cache,&sessMap,onNewSess,
        (const char*)clientAddr,"10.0.100.2",DEFAULT_SERVER_PORT);
    return 0;
}
