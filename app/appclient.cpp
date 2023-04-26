#include "../include/api.h"
#include <unistd.h>
#include "config.h"
#include <thread>
#include <sys/time.h>
//#include <getopt.h>
#undef LOG_LEVEL
#define LOG_LEVEL DEBUG


using namespace std;

//extern char* optarg;
char clientAddr[20] = "10.0.1.1";
char serverAddr[20] = "10.0.100.2";

IUINT32 _round_up(IUINT32 x,IUINT32 y){
    return ((x+y-1)/y)*y;
}

void request_func(LeotpSess * _sessPtr){
    int sendStart = 0, ret;
    while(1){
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
            fflush(stdout);
            pos += REQ_LEN;
        }
    }
    return nullptr;
}

int main(int argc,char** argv){
    int ch;
    
    while((ch=getopt(argc,argv,"c:s:"))!=-1){
        switch(ch){
            case 'c':
                strncpy(clientAddr, optarg, 19);
                break;
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
    LOG(INFO,"entering LEOTP appclient\n");
    LOG(INFO,"request from client %s -> server %s\n",clientAddr, serverAddr);
    startRequester(&cache,&sessMap,onNewSess,
        (const char*)clientAddr,(const char*)serverAddr, DEFAULT_SERVER_PORT);
    return 0;
}
