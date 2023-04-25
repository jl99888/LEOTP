#include "./include/udp_leotp.h"
#include <iostream>
#undef LOG_LEVEL
#define LOG_LEVEL DEBUG

//for debug
double udp_recv_time = 0;
double update_time = 0;
double input_time = 0;

/***************** util functions *****************/

struct sockaddr_in toAddr(in_addr_t IP, uint16_t port) {
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = IP;
    addr.sin_port = port;
    return addr;
}

void writeIPstr(char *ret, in_addr_t IP)
{
    int a,b,c,d;

    a = (0x000000FF & IP);
    b = (0x0000FF00 & IP) >> 8;
    c = (0x00FF0000 & IP) >> 16;
    d = (0xFF000000 & IP) >> 24;

    snprintf(ret,16,"%d.%d.%d.%d",a,b,c,d);
}

/***************** Quad *****************/
// numbers stored in Quad is in type for network ( hton() )
// quadruple for end2end communication
Quad::Quad(in_addr_t _reqAddrIP, uint16_t _reqAddrPort, in_addr_t _respAddrIP, uint16_t _respAddrPort):
reqAddrIP(_reqAddrIP),
reqAddrPort(_reqAddrPort),
respAddrIP(_respAddrIP),
respAddrPort(_respAddrPort)
{
    toChars();
}
Quad::Quad(struct sockaddr_in requesterAddr, struct sockaddr_in responderAddr):
        reqAddrIP(requesterAddr.sin_addr.s_addr), reqAddrPort(requesterAddr.sin_port), respAddrIP(responderAddr.sin_addr.s_addr), respAddrPort(responderAddr.sin_port){
    toChars();
}

Quad Quad::reverse(){
    return Quad(respAddrIP,respAddrPort,reqAddrIP,reqAddrPort);
}
void Quad::toChars(){
    int offset = 0;
    
    memcpy(this->chars+offset, &this->reqAddrIP, sizeof(this->reqAddrIP));
    offset += sizeof(this->reqAddrIP);
    memcpy(this->chars+offset, &this->reqAddrPort, sizeof(this->reqAddrPort));
    offset += sizeof(this->reqAddrPort);
    memcpy(this->chars+offset, &this->respAddrIP, sizeof(this->respAddrIP));
    offset += sizeof(this->respAddrIP);
    memcpy(this->chars+offset, &this->respAddrPort, sizeof(this->respAddrPort));
}

struct sockaddr_in Quad::getReqAddr(){
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = reqAddrIP;
    addr.sin_port = reqAddrPort;
    return addr;
}
struct sockaddr_in Quad::getRespAddr(){
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = respAddrIP;
    addr.sin_port = respAddrPort;
    return addr;
}
bool Quad::operator == (Quad const& quad2) const {
    return memcmp(this->chars, quad2.chars, QUAD_STR_LEN)==0 ? true : false;
}



/***************** INTCP session *****************/

int createSocket(in_addr_t IP, uint16_t port, bool reusePort, uint16_t *finalPort){
    int socketFd = -1;
    if((socketFd=socket(AF_INET,SOCK_DGRAM,0))<0){
        LOG(ERROR, "create socket fail");
        return -1;
    }
    int optval=1;
    if(reusePort){
        setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
    }
    setsockopt(socketFd, SOL_IP, IP_TRANSPARENT, &optval, sizeof(int));
    setsockopt(socketFd, SOL_IP, IP_RECVORIGDSTADDR, &optval, sizeof(int));
    sockaddr_in selfAddr;
    selfAddr.sin_family = AF_INET;
    selfAddr.sin_addr.s_addr = IP;
    uint16_t realPortH = ntohs(port);
    for(;realPortH < ntohs(port)+REUSE_PORT_RANGE; realPortH++){
        selfAddr.sin_port = htons(realPortH);
        if(bind(socketFd, (struct sockaddr *)&selfAddr, AddrLen) != -1){
            LOG(TRACE,"%d",realPortH);
            break;
        }
    }
    // which means all the ports in portRange bind fail
    if(realPortH == ntohs(port)+REUSE_PORT_RANGE){
        LOG(ERROR, "bind fail");
        abort();
    }

    if(finalPort != nullptr)
        *finalPort = htons(realPortH);
    return socketFd;
}

// this is for [requester]
// explicitly called by app-layer
LeotpSess::LeotpSess(in_addr_t reqAddrIP, in_addr_t respAddrIP, 
        uint16_t respAddrPort, Cache* _cachePtr,
        void *(*onNewSess)(void* _sessPtr)):
nodeRole(LEOTP_ROLE_REQUESTER),
cachePtr(_cachePtr)
{
    uint16_t reqAddrPort;
    socketFd_toResp = createSocket(reqAddrIP, htons(DEFAULT_CLIENT_PORT), false, &reqAddrPort);
    if(socketFd_toResp == -1){
        abort();
    }
    socketFd_toReq = -1;

    requesterAddr = toAddr(reqAddrIP, reqAddrPort);
    responderAddr = toAddr(respAddrIP, respAddrPort);

    //general
    Quad quad(requesterAddr,responderAddr);
    memcpy(nameChars, quad.chars, QUAD_STR_LEN);
    lock.lock();
    transCB = createTransCB(this, nodeRole, nullptr);
    lock.unlock();
    pthread_create(&transUpdaterThread, NULL, TransUpdateLoop, this);
    pthread_create(&onNewSessThread, NULL, onNewSess, this);
    return;
}

// this is for [responder]
// this is called when receiving a new Quad
LeotpSess::LeotpSess(Quad quad, int listenFd, Cache* _cachePtr,
        void *(*onNewSess)(void* _sessPtr), int (*onUnsatInt)(IUINT32 start, IUINT32 end, void *user)):
nodeRole(LEOTP_ROLE_RESPONDER),
cachePtr(_cachePtr)
{
    requesterAddr = quad.getReqAddr();
    responderAddr = quad.getRespAddr();
    
    socketFd_toReq = listenFd;
    socketFd_toResp = -1;


    memcpy(nameChars, quad.chars, QUAD_STR_LEN);
    lock.lock();
    transCB = createTransCB(this, nodeRole, onUnsatInt);
    lock.unlock();
    pthread_create(&transUpdaterThread, NULL, TransUpdateLoop, this);
    pthread_create(&onNewSessThread, NULL, onNewSess, this);
    return;
}

// this is for [midnode]
// this is called when receiving a new Quad
LeotpSess::LeotpSess(Quad quad, Cache* _cachePtr,
        void *(*onNewSess)(void* _sessPtr)):
nodeRole(LEOTP_ROLE_MIDNODE),
cachePtr(_cachePtr)
{
    requesterAddr = quad.getReqAddr();
    responderAddr = quad.getRespAddr();
    
    socketFd_toReq = createSocket(responderAddr.sin_addr.s_addr, responderAddr.sin_port, true, nullptr);
    socketFd_toResp = createSocket(requesterAddr.sin_addr.s_addr, requesterAddr.sin_port, true, nullptr);
    if(socketFd_toReq==-1 || socketFd_toResp)
    memcpy(nameChars, quad.chars, QUAD_STR_LEN);
    lock.lock();
    transCB = createTransCB(this, nodeRole, nullptr);
    lock.unlock();
    pthread_create(&transUpdaterThread, NULL, TransUpdateLoop, this);
    pthread_create(&onNewSessThread, NULL, onNewSess, this);
    return;
}

int LeotpSess::inputUDP(char *recvBuf, int recvLen){
    IUINT32 bf = _getMillisec();
    lock.lock();
    IUINT32 af = _getMillisec();
    if(af- bf > 10){
        LOG(TRACE,"input wait %d",af-bf);
    }
    IUINT32 cf = _getUsec();
    int ret = transCB->input(recvBuf, recvLen);
    IUINT32 df = _getUsec();
    input_time += ((double)(df-cf))/1000000;
    if(df- cf > 500){
        LOG(TRACE,"input use %d",df-cf);
    }
    
    lock.unlock();
    sleep(0);
    return ret;
}

int LeotpSess::request(int rangeStart, int rangeEnd){
    lock.lock();
#ifdef HBH_CC
    int ret = transCB->request(rangeStart,rangeEnd);
#else
    int ret = transCB->request(rangeStart,rangeEnd,0,0);
#endif
    lock.unlock();
    return ret;
}

int LeotpSess::recvData(char *recvBuf, int maxBufSize, IUINT32 *startPtr, IUINT32 *endPtr){
    IUINT32 bf = _getMillisec();
    lock.lock();
    IUINT32 af = _getMillisec();
    if(af- bf > 10){
        LOG(TRACE,"recv wait %d",af-bf);
    }
        IUINT32 cf = _getMillisec();
    int ret = transCB->recv(recvBuf,maxBufSize, startPtr, endPtr);
        IUINT32 df = _getMillisec();
        if(df- cf > 3){
            LOG(DEBUG,"recv use %d",df-cf);
        }
    lock.unlock();
    return ret;
}

void LeotpSess::insertData(const char *sendBuf, int start, int end){
    int ret=cachePtr->insert(nameChars,start,end,sendBuf);
    assert(ret==0);
    transCB->notifyNewData(sendBuf,start,end);
}

void* TransUpdateLoop(void *args){
    LeotpSess *sessPtr = (LeotpSess*)args;

    IUINT32 now, updateTime;

    while(1){
        IUINT32 bf = _getMillisec();
        sessPtr->lock.lock();
        IUINT32 af = _getMillisec();
        if(af- bf > 10){
            LOG(DEBUG,"update wait %d",af-bf);
        }
        updateTime = sessPtr->transCB->check();
        now = _getMillisec();
        if (updateTime <= now) {
            IUINT32 cf = _getUsec();
            sessPtr->transCB->update();
            IUINT32 df = _getUsec();
            if(df- cf > 3000){
                LOG(TRACE,"update use %d",df-cf);
            }
            update_time += ((double)(df-cf))/1000000;
            sessPtr->lock.unlock();
            sleep(0);
        } else {
            sessPtr->lock.unlock();
            usleep((updateTime - now)*1000);
        }
    }
    return nullptr;
}
shared_ptr<LeotpTransCB> createTransCB(const LeotpSess *sessPtr, int nodeRole, int (*onUnsatInt)(IUINT32 start, IUINT32 end, void *user)){
    return shared_ptr<LeotpTransCB>(new LeotpTransCB((void*)sessPtr, udpSend, fetchData, onUnsatInt, nodeRole));
}

int udpSend(const char* buf,int len, void* user, int dstRole){
    LeotpSess* sess = (LeotpSess*)user;
    if(sess->nodeRole == dstRole){
        LOG(ERROR, "sess->nodeRole == dstRole");
        abort();
        return -1;
    }
    struct sockaddr_in *dstAddrPtr;
    int outputFd;
    if(dstRole==LEOTP_ROLE_RESPONDER){
        dstAddrPtr = &sess->responderAddr;
        outputFd = sess->socketFd_toResp;
    }else if(dstRole==LEOTP_ROLE_REQUESTER){
        //in midnode, udpSend_default send to requester and udpSend_toResp send to responder
        dstAddrPtr = &sess->requesterAddr;
        outputFd = sess->socketFd_toReq;
    } else {
        LOG(ERROR, "dstRole must be an endpoint");
        abort();
        return -1;
    }
    if(outputFd == -1){
        LOG(ERROR, "outputFd == -1");
        return -1;
    }
    int sendbyte = sendto(outputFd, buf, len, 0, 
            (struct sockaddr*)dstAddrPtr, AddrLen);

    char recvIP[25];
    writeIPstr(recvIP, dstAddrPtr->sin_addr.s_addr);
    LOG(TRACE, "send %d bytes to %s:%d",len, recvIP, ntohs(dstAddrPtr->sin_port));
    return sendbyte;
}

int fetchData(char *buf, IUINT32 start, IUINT32 end, void *user){
    LeotpSess* sess = (LeotpSess*)user;
    int readlen = sess->cachePtr->read(sess->nameChars, start, end, buf);
    return readlen;
}

/***************** multi-session management *****************/


bool addrCmp(struct sockaddr_in addr1, struct sockaddr_in addr2){
    return (addr1.sin_addr.s_addr == addr2.sin_addr.s_addr) && (addr1.sin_port == addr2.sin_port);
}


void *udpRecvLoop(void *_args){
    struct udpRecvLoopArgs *args = (struct udpRecvLoopArgs *)_args;
    char recvBuf[1500]; //for UDP, 1500 is proper
    int recvLen;

    // first, create a socket for listening
    int listenFd;
    if (args->listenFd == -1){
        listenFd = createSocket(
                args->listenAddr.sin_addr.s_addr, args->listenAddr.sin_port, false, nullptr);
    } else {
        listenFd = args->listenFd;
    }

    // prepare for udp recv
    struct sockaddr_in sendAddr, recvAddr;
    struct sockaddr_in requesterAddr;
    struct sockaddr_in responderAddr;
    char cmbuf[100];

    struct iovec iov;
    iov.iov_base = recvBuf;
    iov.iov_len = sizeof(recvBuf) - 1;
    struct msghdr mhdr;
    mhdr.msg_name = &sendAddr;
    mhdr.msg_namelen = AddrLen;
    mhdr.msg_control = cmbuf;
    mhdr.msg_controllen = 100;
    mhdr.msg_iovlen = 1;
    mhdr.msg_iov = &iov;

    shared_ptr<LeotpSess> sessPtr;
    IUINT32 lastLoop = -1, timeSum1=0, timeSum2=0, timeSum3=0, timeTmp;

    int recvedUDPlen = 0;
    while(1){
        IUINT32 cf = _getUsec();
        recvLen = recvmsg(listenFd, &mhdr, 0);
        IUINT32 df = _getUsec();
        udp_recv_time += ((double)(df-cf))/1000000;
        for(struct cmsghdr *cmsg = CMSG_FIRSTHDR(&mhdr); cmsg != NULL; cmsg = CMSG_NXTHDR(&mhdr, cmsg)){
            if(cmsg->cmsg_level != SOL_IP || cmsg->cmsg_type != IP_ORIGDSTADDR) continue;
            memcpy(&recvAddr, CMSG_DATA(cmsg), sizeof(struct sockaddr_in));
        }
        LOG(TRACE, "recv udp len=%d",recvLen);
        recvedUDPlen += recvLen;
        // now we get: data in recvBuf, recvLen, sendAddr, recvAddr
        //
        bool isEndp = addrCmp(recvAddr, args->listenAddr);
        int segDstRole = LeotpTransCB::judgeSegDst(recvBuf, recvLen);
        if(segDstRole == LEOTP_ROLE_RESPONDER){
            requesterAddr = sendAddr;
            responderAddr = recvAddr;
        } else if (segDstRole == LEOTP_ROLE_REQUESTER) {
            requesterAddr = recvAddr;
            responderAddr = sendAddr;
        } else {
            LOG(WARN,"recv not-INTCP packet");
            //TODO dst of some pkts can be midnode in future.
            continue;
        }
        
        Quad quad(requesterAddr, responderAddr);
     
        int ret = args->sessMapPtr->readValue(quad.chars, QUAD_STR_LEN, &sessPtr);
        if (ret == -1){
            //if the endpoint receives a intcp DATA packet from unknown session, ignores it.
            if(isEndp && segDstRole==LEOTP_ROLE_REQUESTER){
          
                LOG(WARN,"requester recvs an unknown packet");
                continue;
            }
            // if not exist, create one.
            char sendIPstr[25];
            writeIPstr(sendIPstr, sendAddr.sin_addr.s_addr);
            LOG(TRACE,"establish: %s:%d", sendIPstr, ntohs(sendAddr.sin_port));
            if(isEndp){
                //new responder session
                //TODO when to release session?
                sessPtr = shared_ptr<LeotpSess>(new LeotpSess(quad, listenFd, args->cachePtr, args->onNewSess, args->onUnsatInt));
            } else {
                //new midnode session
                sessPtr = shared_ptr<LeotpSess>(new LeotpSess(quad, args->cachePtr, args->onNewSess));
            }
             //nodeRole=server
            args->sessMapPtr->setValue(quad.chars, QUAD_STR_LEN, sessPtr);
        }
        sessPtr->inputUDP(recvBuf, recvLen);
        lastLoop = _getUsec();
        if(lastLoop - timeTmp > 1000000){
            LOG(TRACE,"udp_recv_time %.2fs update_time %.2fs input_time %.2fs",udp_recv_time,update_time,input_time);
            timeTmp = _getUsec();
        }
        //lastLoop = _getUsec();
    }

    return nullptr;
}
