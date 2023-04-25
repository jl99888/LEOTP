//=====================================================================
// 
// Based on:
// KCP - A Better ARQ Protocol Implementation
// skywind3000 (at) gmail.com, 2010-2011
//  
//=====================================================================

#ifndef __LEOTP_H__
#define __LEOTP_H__

#define USE_CACHE
#define HBH_CC

#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <cmath>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <list>
#include <memory> // for shared_ptr

#include "generality.h"
#include "log.h"

using namespace std;

#define LEOTP_ROLE_REQUESTER 10
#define LEOTP_ROLE_RESPONDER 11
#define LEOTP_ROLE_MIDNODE 12

#define LEOTP_RTT_SCHM_MAXWND 1
#define LEOTP_RTT_SCHM_EXPO 2

#define LEOTP_CC_SCHM_LOSSB 1
#define LEOTP_CC_SCHM_RTTB 2

#define LEOTP_CC_SLOW_START 0
#define LEOTP_CC_CONG_AVOID 1

const IUINT32 LEOTP_OVERHEAD = 19;            //LEOTP header length
const IUINT32 LEOTP_MTU = 1472;               //UDP MTU
const IUINT32 LEOTP_MSS = LEOTP_MTU - LEOTP_OVERHEAD;   //LEOTP MTU
const IUINT32 LEOTP_INT_RANGE_LIMIT = LEOTP_MSS;  

const IUINT32 LEOTP_UPDATE_INTERVAL = 1;    //Unit: ms
const IUINT32 LEOTP_DEADLINK = 8;

const IUINT32 LEOTP_CMD_INT = 80;         // cmd: interest 
const IUINT32 LEOTP_CMD_PUSH = 81;        // cmd: push data

// Retransmission
const int RTTscheme = LEOTP_RTT_SCHM_EXPO;
const IUINT32 LEOTP_RTO_MIN = 20;         
const IUINT32 LEOTP_RTO_DEF = 10000;      
const IUINT32 LEOTP_RTO_MAX = 60000;
const float LEOTP_RTO_EXPO = 1.1;

const IUINT32 LEOTP_HOLE_WAIT=10;   // ms to confirm a hole
const IUINT32 LEOTP_SNHOLE_TIMEOUT = 200; // ms to delete a hole
//const IUINT32 LEOTP_SNHOLE_THRESHOLD = 5; // packets to confirm a hole 

// Congestion control
const int CCscheme = LEOTP_CC_SCHM_RTTB;
const IUINT32 LEOTP_SSTHRESH_INIT = 100; // cwnd threshold for slow start
const float LEOTP_CWND_MIN = 0.2;       
const IUINT32 LEOTP_RTT0 = 30;          // rtt0 for slow start, unit:ms

// RTT-based
const float QueueingThreshold = 3000;   // bytes
const IUINT32 HrttMinWnd = 10000;       // ms

const IUINT32 LEOTP_SNDQ_MAX = 5 * LEOTP_MSS;       // bytes of target sndqueue length
const IUINT32 LEOTP_INTB_MAX = 20000 * LEOTP_MSS;   // bytes of max intbuf length

const IUINT32 LEOTP_WND_RCV = 128;      // for app recv buffer

const float LEOTP_SENDRATE_MIN = 0.1;   // mbps of min sending rate
const float LEOTP_SENDRATE_MAX = 300;   // mbps of max sending rate


//=====================================================================
// SEGMENT
//=====================================================================
struct LeotpSeg
{
    IUINT32 cmd;         //need send,1B
    IINT16 wnd;          //need send,2B
    IUINT32 ts;          //need send,4B
    IUINT32 len;         //need send,4B, to be removed
    IUINT32 rangeStart;  //need send,4B 
    IUINT32 rangeEnd;    //need send,4B 
    IUINT32 xmit;    // do not send, only keep in intbuf
    char data[1];    //need send
};

struct ByteRange
{
    IUINT32 startByte, endByte, ts;
#ifndef HBH_CC
    IUINT16 wnd;
#endif
};

struct Hole
{
    IUINT32 startByte, endByte; //byte level
    IUINT32 ts;
    int count;
};

class StatInfo
{
public:
    int ssid;
    IUINT32 startTs;

    int xmit;
    IUINT32 lastPrintTs;
    int recvedUDP; // Mbps
    int recvedLEOTP;
    int sentLEOTP;
    int cntTimeout,cntIntHole,cntDataHole;

    void reset(){
        int ssidTmp=ssid;
        IUINT32 startTsTmp=startTs;
        memset(this,0,sizeof(*this));
        ssid = ssidTmp;
        startTs = startTsTmp;
        lastPrintTs = _getMillisec();
    }
    void init(){
        IUINT32 current = _getMillisec();
        ssid = current%10000;
        startTs = current;
        reset();
    }
};

struct RcvBufItr
{
    IUINT32 startByte, endByte;
    list<shared_ptr<LeotpSeg>>::iterator itr;
};

//---------------------------------------------------------------------
// LeotpTransCB
//---------------------------------------------------------------------
class LeotpTransCB
{
private:
	int state;
	IUINT32 updated, nextFlushTs, lastFlushTs;
    StatInfo stat;

   	int nodeRole;

    //requester
    list<ByteRange> intQueue;
    list<shared_ptr<LeotpSeg>> intBuf;
    list<shared_ptr<LeotpSeg>> rcvBuf;
    list<RcvBufItr> rcvBufItrs;

	IUINT32 rcvNxt; // for ordered data receiving
    list<shared_ptr<LeotpSeg>> rcvQueue;

    //responder
    list<ByteRange> pendingInts;
    list<shared_ptr<LeotpSeg>> sndQueue;
    shared_ptr<char> tmpBuffer;

    /* ------------ Loss Recovery --------------- */
    // end-to-end timeout
    int srtt, rttvar, rto;
    // maxRtt window
    list<int> rttQueue;
    // exponential
    int conseqTimeout;

    // sequence number holes
    list<Hole> dataHoles;
    IUINT32 dataNextRangeStart;

    /* ----- hop-by-hop Congestion Control ----- */
    // Flow control
    int rmt_sndq_rest;
    int intOutputLimit;

    int ccState;
    int ccDataLen;
    float cwnd;
    
    int intHopOwd, hopSrtt, hopRttvar;
    // if there is no interest to send in short-term future, 
    // requester needs to send empty interest for sendRate notification
    // this is particularly necessary in slow start phase
    IUINT32 lastSendIntTs;
    // throughput calculation for rtt-based CC and app-limited detection
    IUINT32 lastThrpUpdateTs;
    int recvedBytesLastHRTT, recvedBytesThisHRTT;
    float thrpLastPeriod; // Mbps

    // congestion signal
    // loss-based
    bool hasLossEvent;
    // RTT-based
    list<pair<IUINT32,int>> hrttQueue;

    // to avoid severe cwnd decreasing in one hrtt
    IUINT32 lastCwndDecrTs;

    // send rate limitation for queue length control
    int sndQueueBytes, intBufBytes;
    // int rmt_sndq_rest;

    // send rate notification and implementation
    float rmtSendRate; // Mbps
    int dataOutputLimit;

    void *user;
    int (*outputFunc)(const char *buf, int len, void *user, int dstRole);
	// set callback called by responseInterest
	int (*fetchDataFunc)(char *buf, IUINT32 start, IUINT32 end, void *user);
    int (*onUnsatInt)(IUINT32 start, IUINT32 end, void *user);
	// also called by responseInterest


    // allocate a new kcp segment
    shared_ptr<LeotpSeg> createSeg(int size);
    char* encodeSeg(char *ptr, const LeotpSeg *seg);

    // flush pending data
    void flush();
    void flushIntQueue();
    void flushIntBuf();
    void flushData();
    
    int output(const void *data, int size, int dstRole);
#ifdef HBH_CC
    int outputInt(IUINT32 rangeStart, IUINT32 rangeEnd);
#else
    int outputInt(IUINT32 rangeStart, IUINT32 rangeEnd, IUINT16 wnd, IUINT32 ts);
#endif
    void updateRTT(IINT32 rtt, int xmit);
    void updateHopRTT(IINT32 hop_rtt);
    void flushDataHoles();
    void updateIntBuf(IUINT32 rangeStart, IUINT32 rangeEnd);
    void sendDataHeader(IUINT32 rangeStart, IUINT32 rangeEnd);
    void insertDataHole(IUINT32 rangeStart, IUINT32 rangeEnd);
    void deleteDataHole(IUINT32 rangeStart, IUINT32 rangeEnd);
    bool detectDataHole(IUINT32 rangestart, IUINT32 rangeEnd);
    void parseData(shared_ptr<LeotpSeg> newseg,bool data_header);

    // after input
#ifdef HBH_CC
    void parseInt(IUINT32 rangeStart,IUINT32 rangeEnd);
#else
    void parseInt(IUINT32 rangeStart,IUINT32 rangeEnd,IUINT16 wnd,IUINT32 ts);
#endif
    // returns below zero for error
    int sendData(const char *buffer, IUINT32 start, IUINT32 end);

    void moveToRcvQueue();
    
    IINT16 getDataSendRate();
    IINT16 getIntDev();
    
    void updateCwnd(IUINT32 dataLen);
    
    bool allow_cwnd_increase();
    bool allow_cwnd_decrease(IUINT32 current);

//---------------------------------------------------------------------
// API
//---------------------------------------------------------------------
public:
    // from the same connection. 'user' will be passed to the output callback
    LeotpTransCB(void *user, 
			int (*_outputFunc)(const char *buf, int len, void *user, int dstRole), 
			int (*_fetchDataFunc)(char *buf, IUINT32 start, IUINT32 end, void *user),
			int (*_onUnsatInt)(IUINT32 start, IUINT32 end, void *user),
			// bool _isUnreliable,
			int _nodeRole
	);
    LeotpTransCB(){}
    // release kcp control object
    // ~LeotpTransCB();

    // intcp user/upper level request
#ifdef HBH_CC
    int request(IUINT32 rangeStart,IUINT32 rangeEnd);
#else
    int request(IUINT32 rangeStart,IUINT32 rangeEnd, IUINT16 wnd, IUINT32 ts);
#endif
    // when you received a low level packet (eg. UDP packet), call it
    int input(char *data, int size);

    void notifyNewData(const char *buffer, IUINT32 start, IUINT32 end);

    // update state (call it repeatedly, every 10ms-100ms), or you can ask 
    // ikcp_check when to call it again (without ikcp_input/_send calling).
    // 'current' - current timestamp in millisec. 
    void update();

    // Determine when should you invoke ikcp_update:
    // returns when you should invoke ikcp_update in millisec, if there 
    // is no ikcp_input/_send calling. you can call ikcp_update in that
    // time, instead of call update repeatly.
    // Important to reduce unnacessary ikcp_update invoking. use it to 
    // schedule ikcp_update (eg. implementing an epoll-like mechanism, 
    // or optimize ikcp_update when handling massive kcp connections)
    IUINT32 check();

    // user/upper level recv: returns size, returns below zero for EAGAIN
    int recv(char *buffer, int maxBufSize, IUINT32 *startPtr, IUINT32 *endPtr);

    static int judgeSegDst(const char *p, long size);

//---------------------------------------------------------------------
// rarely use
//---------------------------------------------------------------------
    // get how many packet is waiting to be sent
    float getCwnd();
    int getWaitSnd();
    int getRwnd();
    // check the size of next message in the recv queue
    int peekSize();
};


float bytesToMbit(int bytes);
int mbitToBytes(float mbit);

#endif