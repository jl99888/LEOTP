#include "./include/leotp.h"
#undef LOG_LEVEL
#define LOG_LEVEL DEBUG // SILENT//

#include <iostream>
using namespace std;

//---------------------------------------------------------------------
// create a new LeotpTransCB
//---------------------------------------------------------------------
LeotpTransCB::LeotpTransCB(
    void *_user,
    int (*_outputFunc)(const char *buf, int len, void *user, int dstRole),
    int (*_fetchDataFunc)(char *buf, IUINT32 start, IUINT32 end, void *user),
    int (*_onUnsatInt)(IUINT32 start, IUINT32 end, void *user),
    int _nodeRole) : user(_user),
                     outputFunc(_outputFunc),
                     fetchDataFunc(_fetchDataFunc),
                     onUnsatInt(_onUnsatInt),
                     nodeRole(_nodeRole),
                     rcvNxt(0),
                     cwnd(LEOTP_CWND_MIN),
                     state(0),
                     srtt(0),
                     rttvar(0),
                     rto(LEOTP_RTO_DEF),
                     hopSrtt(0),
                     hopRttvar(0),
                     nextFlushTs(0),
                     updated(0),
                     dataNextRangeStart(0),
                     ccState(LEOTP_CC_SLOW_START),
                     ccDataLen(0),
                     dataOutputLimit(LEOTP_MSS),
                     rmt_sndq_rest(LEOTP_SNDQ_MAX),
                     sndQueueBytes(0),
                     intHopOwd(-1),
                     rmtSendRate(LEOTP_SENDRATE_MIN),
                     intBufBytes(0),
                     lastCwndDecrTs(0),
                     lastThrpUpdateTs(0),
                     recvedBytesThisHRTT(0),
                     recvedBytesLastHRTT(0),
                     hasLossEvent(false),
                     thrpLastPeriod(0),
                     conseqTimeout(0),
                     lastSendIntTs(0),
                     lastFlushTs(0),
                     intOutputLimit(LEOTP_SNDQ_MAX) // LEOTP_SNDQ_MAX
{
    stat.init();
    void *tmp = malloc(LEOTP_MTU * 3);
    assert(tmp != NULL);
    tmpBuffer = shared_ptr<char>(static_cast<char *>(tmp));
}

// allocate a new intcp segment
shared_ptr<LeotpSeg> LeotpTransCB::createSeg(int size)
{
    void *tmp = malloc(sizeof(LeotpSeg) + size);
    assert(tmp != NULL);
    return shared_ptr<LeotpSeg>(static_cast<LeotpSeg *>(tmp));
}

// output segment, size include kcp header
int LeotpTransCB::output(const void *data, int size, int dstRole)
{
    // LOG(DEBUG, "size %d", size-LEOTP_OVERHEAD);
    if (size == 0)
        return 0;
    return outputFunc((const char *)data, size, user, dstRole);
}
#ifdef HBH_CC
int LeotpTransCB::outputInt(IUINT32 rangeStart, IUINT32 rangeEnd)
#else
int LeotpTransCB::outputInt(IUINT32 rangeStart, IUINT32 rangeEnd,IUINT16 wnd, IUINT32 ts)
#endif
{
    LOG(TRACE, "output int [%u,%u]", rangeStart, rangeEnd);
    shared_ptr<LeotpSeg> segPtr = createSeg(0);
    // segPtr->len = 0;
    segPtr->cmd = LEOTP_CMD_INT;
    segPtr->rangeStart = rangeStart;
    segPtr->rangeEnd = rangeEnd;
    lastSendIntTs = _getMillisec();
#ifdef HBH_CC
    segPtr->ts = lastSendIntTs;
    segPtr->wnd = getDataSendRate();
#else
    if(nodeRole==LEOTP_ROLE_REQUESTER){
        segPtr->ts = lastSendIntTs;
        segPtr->wnd = getDataSendRate();
    }
    else{
        segPtr->ts = ts;
        segPtr->wnd = wnd;
    }
#endif
    segPtr->len = 0;
    encodeSeg(tmpBuffer.get(), segPtr.get());
    return output(tmpBuffer.get(), LEOTP_OVERHEAD, LEOTP_ROLE_RESPONDER);
}

//---------------------------------------------------------------------
// encodeSeg
//---------------------------------------------------------------------
char *LeotpTransCB::encodeSeg(char *ptr, const LeotpSeg *seg)
{
    ptr = encode8u(ptr, (IUINT8)seg->cmd);
    ptr = encode16(ptr, seg->wnd);
    ptr = encode32u(ptr, seg->ts);
    ptr = encode32u(ptr, seg->len);
    ptr = encode32u(ptr, seg->rangeStart);
    ptr = encode32u(ptr, seg->rangeEnd);
    return ptr;
}

//---------------------------------------------------------------------
// user/upper level recv: returns size, returns below zero for EAGAIN
//---------------------------------------------------------------------
int LeotpTransCB::recv(char *buffer, int maxBufSize, IUINT32 *startPtr, IUINT32 *endPtr)
{
    list<shared_ptr<LeotpSeg>>::iterator p;
    shared_ptr<LeotpSeg> seg;

    if (rcvQueue.empty())
        return -1;

    if (nodeRole == LEOTP_ROLE_MIDNODE)
    {
        shared_ptr<LeotpSeg> firstSeg = *rcvQueue.begin();
        if (firstSeg->rangeEnd - firstSeg->rangeStart != firstSeg->len)
        {
            LOG(DEBUG, "inconsistent range:rangeStart %u rangeEnd %u length %u", firstSeg->rangeStart, firstSeg->rangeEnd, firstSeg->len);
            rcvQueue.pop_front(); // discard
        }
        else if (firstSeg->len <= maxBufSize)
        {
            *startPtr = firstSeg->rangeStart;
            *endPtr = firstSeg->rangeEnd;
            memcpy(buffer, firstSeg->data, firstSeg->len);
            rcvQueue.pop_front();
        }
    }
    else
    {
        // copy seg->data in rcvQueue to buffer as much as possible
        *startPtr = *endPtr = (*rcvQueue.begin())->rangeStart;
        for (p = rcvQueue.begin(); p != rcvQueue.end();)
        {
            seg = *p;
            if (seg->len + *endPtr - *startPtr > maxBufSize)
                break;
            if (*endPtr != seg->rangeStart)
                break;
            memcpy(buffer, seg->data, seg->len);
            buffer += seg->len;
            *endPtr += seg->len;

            rcvQueue.erase(p++);
        }
    }

    moveToRcvQueue();

    return 0;
}

//---------------------------------------------------------------------
// user/upper level send, returns below zero for error
//---------------------------------------------------------------------
int LeotpTransCB::sendData(const char *buffer, IUINT32 start, IUINT32 end)
{
    int len = end - start;
    shared_ptr<LeotpSeg> seg;

    if (len <= 0)
        return -1;

    while (len > 0)
    {
        int size = len > (int)LEOTP_MSS ? (int)LEOTP_MSS : len;
        seg = createSeg(size);
        assert(seg);
        if (seg == NULL)
        {
            return -2;
        }
        if (buffer && len > 0)
        {
            memcpy(seg->data, buffer, size);
        }
        seg->cmd = LEOTP_CMD_PUSH;
        seg->len = size;
        seg->rangeStart = start;
        seg->rangeEnd = start + size;
        seg->wnd = 0;
        seg->ts = 0;
        start += size;
        sndQueue.push_back(seg);
        sndQueueBytes += (LEOTP_OVERHEAD+ seg->len);
        LOG(TRACE, "sendData [%d,%d) ,ts %u, sndQ %u,", seg->rangeStart, seg->rangeEnd,_getMillisec(),sndQueueBytes/LEOTP_MSS);
        buffer += size;
        len -= size;
    }

    return 0;
}

#ifdef HBH_CC
int LeotpTransCB::request(IUINT32 rangeStart, IUINT32 rangeEnd)
#else
int LeotpTransCB::request(IUINT32 rangeStart, IUINT32 rangeEnd, IUINT16 wnd, IUINT32 ts)    //wnd and ts is for midnode
#endif
{
    if (rangeEnd <= rangeStart)
    {
        LOG(WARN, "rangeStart %d rangeEnd %d", rangeStart, rangeEnd);
        return -2;
    }
    if (intBufBytes >= LEOTP_INTB_MAX)
    {
        return -1;
    }
    ByteRange intr;
    intr.startByte = rangeStart;
    intr.endByte = rangeEnd;
#ifndef HBH_CC
    intr.wnd = wnd;
    intr.ts = ts;
#endif
    intQueue.push_back(intr);
    return 0;
}
//---------------------------------------------------------------------
// update rtt(call when receive data)
//---------------------------------------------------------------------
void LeotpTransCB::updateRTT(IINT32 rtt, int xmit)
{
    //LOG(SILENT,"update srtt %d",rtt);
    if (xmit > 1)
    {
        LOG(TRACE, "retrans packet rtt %d", rtt);
    }
    if (rtt <= 0)
    {
        return;
    }
    if (srtt == 0)
    {
        srtt = rtt;
        rttvar = rtt / 2;
        rto = _ibound_(LEOTP_RTO_MIN, rtt * 3, LEOTP_RTO_MAX);
        return;
    }
    int rttForUpdate = 0, doUpdate = 1;
    if (RTTscheme == LEOTP_RTT_SCHM_MAXWND)
    {
        // Scheme 1: max-window filter
        // NOTE this will result to a smaller rttVar during rtt oscillation
        // get old maxRtt
        int maxRttOld = -1;
        for (int r : rttQueue)
        {
            maxRttOld = max(maxRttOld, r);
        }
        // update rtt queue
        rttQueue.push_back(rtt);
        while (rttQueue.size() > 5)
        { // TODO more adaptive value?
            rttQueue.pop_front();
        }
        // get new maxRtt
        int maxRtt = -1;
        for (int r : rttQueue)
        {
            maxRtt = max(maxRtt, r);
        }
        if (maxRttOld != maxRtt)
        {
            rttForUpdate = maxRtt;
        }
        else
        {
            doUpdate = 0;
        }
    }
    else if (RTTscheme == LEOTP_RTT_SCHM_EXPO)
    {
        // Scheme 2: multiply rttvar & srtt by a factor for the timeout interests
        if (xmit <= 1)
        {
            rttForUpdate = rtt;
        }
        else
        {
            doUpdate = 0;
        }
    }

    // basic update logic
    if (doUpdate)
    {
        long delta = rttForUpdate - srtt;
        if (delta < 0)
            delta = -delta;
        rttvar = (3 * rttvar + delta) / 4;
        srtt = (7 * srtt + rttForUpdate) / 8;
        if (srtt < 1)
            srtt = 1;
        IINT32 rtoTmp = srtt + _imax_(LEOTP_UPDATE_INTERVAL, 4 * rttvar);
        rto = _ibound_(LEOTP_RTO_MIN, rtoTmp, LEOTP_RTO_MAX);
    }

    LOG(TRACE, "rtt %d srtt %d val %d rto %d", rtt, srtt, rttvar, rto);
}

void LeotpTransCB::updateHopRTT(IINT32 hop_rtt)
{
    LOG(TRACE, "rtt=%d hopsrtt=%d", hop_rtt, hopSrtt);
    if (hopSrtt == 0)
    {
        hopSrtt = hop_rtt;
        hopRttvar = hop_rtt / 2;
    }
    else
    {
        long delta = hop_rtt - hopSrtt;
        if (delta < 0)
            delta = -delta;
        hopRttvar = (3 * hopRttvar + delta) / 4;
        hopSrtt = (7 * hopSrtt + hop_rtt) / 8;
        if (hopSrtt < 1)
            hopSrtt = 1;
    }
}


#ifdef HBH_CC
void LeotpTransCB::parseInt(IUINT32 rangeStart, IUINT32 rangeEnd)
#else
void LeotpTransCB::parseInt(IUINT32 rangeStart,IUINT32 rangeEnd,IUINT16 wnd,IUINT32 ts)
#endif
{
    // TODO priority
    if (rangeEnd <= rangeStart)
    {
        LOG(TRACE, "rangeEnd <= rangeStart");
        return;
    }

    IUINT32 sentEnd = rangeStart;
    if (nodeRole != LEOTP_ROLE_REQUESTER)
    {
        // first, try to fetch data
        IUINT32 segStart, segEnd;
        int fetchLen;
        for (segStart = rangeStart; segStart < rangeEnd; segStart += LEOTP_MSS)
        {
            segEnd = _imin_(rangeEnd, segStart + LEOTP_MSS);
            fetchLen = fetchDataFunc(tmpBuffer.get(), segStart, segEnd, user);
            sentEnd = segStart + fetchLen;
            if (fetchLen == 0)
                break;
            // push fetched data(less than mtu) to sndQueue
            sendData(tmpBuffer.get(), segStart, segStart + fetchLen);
            // if this seg is not completed due to data miss
            if (fetchLen < segEnd - segStart)
            {
                break;
            }
        }
    }

    // rest range
    if (sentEnd < rangeEnd)
    {
        // NOTE in midnode, if cache has [3,10], interest is [0,10], the whole cache is wasted;
        if (nodeRole == LEOTP_ROLE_RESPONDER)
        {
            // append interest to pendingInts
            if (rangeEnd <= rangeStart)
            {
                LOG(WARN, "rangeStart %d rangeEnd %d", rangeStart, rangeEnd);
                return;
            }
            ByteRange ir;
            ir.ts = _getMillisec();
            ir.startByte = sentEnd;
            ir.endByte = rangeEnd;
            pendingInts.push_back(ir);
            LOG(TRACE, "unsat [%d,%d)", sentEnd, rangeEnd);
            onUnsatInt(sentEnd, rangeEnd, user);
        }
        else if (nodeRole == LEOTP_ROLE_REQUESTER)
        {
#ifdef HBH_CC
            outputInt(sentEnd, rangeEnd);
#else
            outputInt(sentEnd, rangeEnd,0,0);
#endif
        }
        else
        { // LEOTP_ROLE_MIDNODE
            LOG(TRACE, "output int [%d,%d)", sentEnd, rangeEnd);
#ifdef HBH_CC
            request(sentEnd, rangeEnd);
#else
            request(sentEnd, rangeEnd, wnd, ts);
#endif
        }
    }
}

void LeotpTransCB::notifyNewData(const char *buffer, IUINT32 dataStart, IUINT32 dataEnd)
{
    if (pendingInts.empty())
        return;
    list<ByteRange>::iterator p, next;
    LeotpSeg *seg;
    for (p = pendingInts.begin(); p != pendingInts.end(); p = next)
    {
        next = p;
        next++;
        int intStart = p->startByte, intEnd = p->endByte, ts = p->ts;
        // check if the union is not empty
        if (_itimediff(intStart, dataEnd) < 0 && _itimediff(intEnd, dataStart) > 0)
        {
            IUINT32 maxStart = _imax_(intStart, dataStart);
            IUINT32 minEnd = _imin_(intEnd, dataEnd);
            LOG(TRACE, "satisfy pending int: [%d,%d)", maxStart, minEnd);
            sendData(buffer + maxStart - dataStart, maxStart, minEnd);
            if (maxStart == intStart && minEnd == intEnd)
            {
                pendingInts.erase(p);
            }
            else if (minEnd == intEnd)
            {
                // partly sent
                p->endByte = maxStart;
            }
            else
            {
                p->startByte = minEnd;
                if (maxStart != intStart)
                {
                    ByteRange ir;
                    ir.ts = ts;
                    ir.startByte = intStart;
                    ir.endByte = maxStart;
                    pendingInts.insert(p, ir);
                }
            }
        }
    }
}

// data header doesn't provide other information
void LeotpTransCB::sendDataHeader(IUINT32 rangeStart, IUINT32 rangeEnd)
{
    LOG(TRACE," generate header [%u,%u]",rangeStart,rangeEnd);
    shared_ptr<LeotpSeg> segPtr = createSeg(0);
    segPtr->cmd = LEOTP_CMD_PUSH;
    segPtr->rangeStart = rangeStart;
    segPtr->rangeEnd = rangeEnd;
    segPtr->len = 0;
    segPtr->ts = 0;
    segPtr->wnd = 0;
    sndQueue.push_back(segPtr);
    sndQueueBytes += LEOTP_OVERHEAD;
}

void LeotpTransCB::insertDataHole(IUINT32 rangeStart, IUINT32 rangeEnd)
{
    Hole newHole;
    newHole.count = 0;
    newHole.startByte = rangeStart;
    newHole.endByte = rangeEnd;
    newHole.ts = _getMillisec();
    dataHoles.push_back(newHole);
    return;
}

void LeotpTransCB::deleteDataHole(IUINT32 rangeStart, IUINT32 rangeEnd)
{
    list<Hole>::iterator iter, next;
    for (iter = dataHoles.begin(); iter != dataHoles.end(); iter = next)
    {
        next = iter;
        next++;
        if (iter->startByte >= rangeEnd)
            break;
        if (iter->endByte <= rangeStart)
            continue;
        if (iter->endByte <= rangeEnd)
        {
            if (iter->startByte >= rangeStart) // rangeStart<=holeStart<holeEnd<=rangeEnd
                dataHoles.erase(iter);
            else
            { // holeStart<rangeStart<holeEnd<=rangeEnd
                iter->endByte = rangeStart;
            }
        }
        else
        {
            if (iter->startByte >= rangeStart) // rangeStart<=holeStart<rangeEnd<holeEnd
                iter->startByte = rangeEnd;
            else
            { // holeStart<rangeStart<rangeEnd<holeEnd
                Hole newHole = *iter;
                iter->endByte = rangeStart;
                newHole.startByte = rangeEnd;
                dataHoles.insert(next, newHole);
            }
        }
    }
    return;
}



void LeotpTransCB::updateIntBuf(IUINT32 rangeStart, IUINT32 rangeEnd)
{
    IUINT32 intRangeStart, intRangeEnd;
    list<shared_ptr<LeotpSeg>>::iterator iter, next;
    for (iter = intBuf.begin(); iter != intBuf.end(); iter = next)
    {
        next = iter;
        next++;
        intRangeStart = (*iter)->rangeStart;
        intRangeEnd = (*iter)->rangeEnd;
        if (intRangeStart >= rangeEnd)
            break;
        if (intRangeEnd <= rangeStart)
            continue;
        if (intRangeEnd <= rangeEnd)
        {
            if (intRangeStart >= rangeStart) // rangeStart<=intStart<intEnd<=rangeEnd
                (*iter)->ts = _getMillisec();
            else
            { // intStart<rangeStart<intEnd<=rangeEnd
                shared_ptr<LeotpSeg> newseg = createSeg(0);
                memcpy(newseg.get(), (*iter).get(), sizeof(LeotpSeg));
                (*iter)->rangeEnd = rangeStart;
                newseg->rangeStart = rangeStart;
                newseg->ts = _getMillisec();
                intBuf.insert(next, newseg);
            }
        }
        else
        {
            if (intRangeStart >= rangeStart)
            { // rangeStart<=IntStart<rangeEnd<IntEnd
                shared_ptr<LeotpSeg> newseg = createSeg(0);
                memcpy(newseg.get(), (*iter).get(), sizeof(LeotpSeg));
                (*iter)->rangeStart = rangeEnd;
                newseg->rangeEnd = rangeEnd;
                newseg->ts = _getMillisec();
                intBuf.insert(iter, newseg);
            }
            else
            { // intStart<rangeStart<rangeEnd<intEnd
                shared_ptr<LeotpSeg> newseg1 = createSeg(0);
                shared_ptr<LeotpSeg> newseg2 = createSeg(0);
                memcpy(newseg1.get(), (*iter).get(), sizeof(LeotpSeg));
                memcpy(newseg2.get(), (*iter).get(), sizeof(LeotpSeg));
                newseg1->rangeEnd = rangeStart;
                newseg2->rangeStart = rangeEnd;
                (*iter)->rangeStart = rangeStart;
                (*iter)->rangeEnd = rangeEnd;
                (*iter)->ts = _getMillisec();
                intBuf.insert(iter, newseg1);
                intBuf.insert(next, newseg2);
            }
        }
    }
}

bool LeotpTransCB::detectDataHole(IUINT32 rangeStart, IUINT32 rangeEnd)
{
#ifndef USE_CACHE
    if (nodeRole==LEOTP_ROLE_MIDNODE)
        return false;
#endif

    if (rangeStart == dataNextRangeStart)
    { // normal packet
        dataNextRangeStart = rangeEnd;
        return false;
    }
    else if (rangeStart < dataNextRangeStart)
    { // retransmission packet
        deleteDataHole(rangeStart, rangeEnd);
        return false;
    }
    else
    { // find a hole
        if(dataNextRangeStart==0||rangeStart-dataNextRangeStart>100*LEOTP_MSS){          //abnormal hole
            dataNextRangeStart=rangeEnd;
            return false;
        }
        insertDataHole(dataNextRangeStart, rangeStart);
        if (nodeRole == LEOTP_ROLE_MIDNODE)
        {
            sendDataHeader(dataNextRangeStart, rangeStart);
        }
        dataNextRangeStart = rangeEnd;
        return true;
    }
}

void LeotpTransCB::flushDataHoles()
{
    IUINT32 current = _getMillisec();
    LOG(TRACE, "dataHoles size %lu", dataHoles.size());
    list<Hole>::iterator iter, next;
    for (iter = dataHoles.begin(); iter != dataHoles.end(); iter = next)
    {
        next = iter;
        next++;
        if (_itimediff(current, iter->ts) >= LEOTP_SNHOLE_TIMEOUT)
            dataHoles.erase(iter);
        if (_itimediff(current, iter->ts) >= LEOTP_HOLE_WAIT)
        {

            LOG(TRACE, " Send retran interest [%u,%u], waiting time %ld ", iter->startByte, iter->endByte,_itimediff(current,iter->ts));
#ifdef HBH_CC
            parseInt(iter->startByte, iter->endByte);
#else
            parseInt(iter->startByte, iter->endByte,0,0);
#endif
            if (nodeRole == LEOTP_ROLE_REQUESTER)
                updateIntBuf(iter->startByte, iter->endByte);
            dataHoles.erase(iter); // only try once
        }
    }
    return;
}


void LeotpTransCB::parseData(shared_ptr<LeotpSeg> dataSeg, bool data_header)
{
    if (nodeRole == LEOTP_ROLE_REQUESTER)
    {

        if (data_header)
            return;
        list<shared_ptr<LeotpSeg>>::iterator intIter, intNext;
        // in requester, need to delete range of intBuf
        int intUseful = 0;
        for (intIter = intBuf.begin(); intIter != intBuf.end(); intIter = intNext)
        {
            shared_ptr<LeotpSeg> intSeg = *intIter;
            intNext = intIter;
            intNext++;
            if (dataSeg->rangeEnd <= intSeg->rangeStart)
            {
                break;
            }
            if (dataSeg->rangeStart < intSeg->rangeEnd && dataSeg->rangeEnd > intSeg->rangeStart)
            {
                LOG(TRACE, "[%d,%d) rtt %d current %u xmit %d", dataSeg->rangeStart, dataSeg->rangeEnd,
                    _getMillisec() - intSeg->ts, _getMillisec(), intSeg->xmit);
                intUseful = 1;
                //-------------------------------
                // insert [the intersection of seg and interest] into rcvBuf
                //-------------------------------
                int intsecStart = _imax_(intSeg->rangeStart, dataSeg->rangeStart);
                int intsecEnd = _imin_(intSeg->rangeEnd, dataSeg->rangeEnd);
                shared_ptr<LeotpSeg> intsecDataSeg = createSeg(intsecEnd - intsecStart);
                intsecDataSeg->rangeStart = intsecStart;
                intsecDataSeg->rangeEnd = intsecEnd;
                intsecDataSeg->len = intsecEnd - intsecStart;
                LOG(TRACE, "satisfy interest range %u, data range %u", intsecEnd - intsecStart, dataSeg->len);
                memcpy(intsecDataSeg->data, dataSeg->data + intsecStart - dataSeg->rangeStart,
                       intsecEnd - intsecStart);
                // NOTE pass information to app layer
                IUINT32 cur_tmp = _getMillisec();
                memcpy(intsecDataSeg->data + sizeof(IUINT32), &intSeg->xmit, sizeof(IUINT32));
                memcpy(intsecDataSeg->data + sizeof(IUINT32) * 2, &cur_tmp, sizeof(IUINT32));

                // TODO entirely rewrite
                IUINT32 t0 = _getUsec(), loop = 0, front = true, t2, t3, t4;
                IUINT32 ns = intsecDataSeg->rangeStart, ne = intsecDataSeg->rangeEnd;

                list<RcvBufItr>::iterator itrNext, itrPrev;
                bool found = false;
                for (itrNext = rcvBufItrs.begin(); itrNext != rcvBufItrs.end(); itrNext++)
                {
                    if (itrNext->startByte >= ne)
                    {
                        found = true;
                        break;
                    }
                }
                itrPrev = found ? itrNext : rcvBufItrs.end();
                bool isPrevContinuous = itrPrev != rcvBufItrs.begin() && (--itrPrev)->endByte == ns;
                if (found)
                {
                    rcvBuf.insert(itrNext->itr, intsecDataSeg);
                    if (itrNext->startByte == ne)
                    {
                        if (isPrevContinuous)
                        {
                            itrPrev->endByte = itrNext->endByte;
                            rcvBufItrs.erase(itrNext);
                        }
                        else
                        {
                            itrNext->startByte = ns;
                        }
                    }
                    else
                    {
                        if (isPrevContinuous)
                        {
                            itrPrev->endByte = ne;
                        }
                        else
                        {
                            auto tmpItr = itrNext->itr;
                            --tmpItr;
                            RcvBufItr ptr = {
                                .startByte = ns,
                                .endByte = ne,
                                .itr = tmpItr};
                            rcvBufItrs.insert(itrNext, ptr);
                        }
                    }
                }
                else
                {
                    rcvBuf.push_back(intsecDataSeg);
                    if (isPrevContinuous)
                    {
                        itrPrev->endByte = ne;
                    }
                    else
                    {
                        auto tmpItr = rcvBuf.end();
                        --tmpItr;
                        RcvBufItr ptr = {
                            .startByte = ns,
                            .endByte = ne,
                            .itr = tmpItr};
                        rcvBufItrs.push_back(ptr);
                    }
                }
                
                IUINT32 t1 = _getUsec();
                if (t1 - t0 > 100)
                {
                    LOG(TRACE, "%d loop %d/%ld fr %d t:%d %d %d %d,h %d t %d", t1 - t0, loop, rcvBuf.size(), front,
                        t2 - t0, t3 - t2, t4 - t3, t1 - t4, (*rcvBuf.begin())->rangeStart, (*--rcvBuf.end())->rangeStart);
                }

                //------------------------------
                // update intBuf
                //------------------------------
                intBufBytes -= intsecEnd - intsecStart;
                stat.recvedLEOTP += intsecEnd - intsecStart;
                conseqTimeout = 0;
                if (dataSeg->rangeStart <= intSeg->rangeStart)
                {
                    if (dataSeg->rangeEnd >= intSeg->rangeEnd)
                    { // range completely received
                        updateRTT(_itimediff(_getMillisec(), intSeg->ts), intSeg->xmit);
                        intBuf.erase(intIter);
                    }
                    else
                    {
                        intSeg->rangeStart = dataSeg->rangeEnd;
                    }
                }
                else if (dataSeg->rangeEnd >= intSeg->rangeEnd)
                {
                    intSeg->rangeEnd = dataSeg->rangeStart;
                }
                else
                {
                    shared_ptr<LeotpSeg> newseg = createSeg(0);
                    memcpy(newseg.get(), intSeg.get(), sizeof(LeotpSeg));
                    intSeg->rangeEnd = dataSeg->rangeStart;
                    newseg->rangeStart = dataSeg->rangeEnd;
                    intBuf.insert(intIter, newseg);
                }
            }
        }
        if (intUseful == 0)
        {
            LOG(TRACE, "useless data recved [%u,%u)", dataSeg->rangeStart, dataSeg->rangeEnd);
        }
    }
    else
    {
        shared_ptr<LeotpSeg> segToForward = createSeg(dataSeg->len);
        // TODO copy char[] pointer??
        memcpy(segToForward.get(), dataSeg.get(), sizeof(LeotpSeg) + dataSeg->len);
        LOG(TRACE,"forward data [%u,%u], ts %u, sndQ %u",segToForward->rangeStart,segToForward->rangeEnd,_getMillisec(),sndQueueBytes/LEOTP_MSS);
        sndQueue.push_back(segToForward);
        sndQueueBytes += (LEOTP_OVERHEAD+segToForward->len);
    #ifdef USE_CACHE
        if (!data_header)
            rcvBuf.push_back(dataSeg);
    #endif

    }

    moveToRcvQueue();
}

// reordering in requester: queueing in order of interest
//  (suppose interest is in order now)
//  move available data from rcvBuf -> rcvQueue
void LeotpTransCB::moveToRcvQueue()
{
    // TODO add rcvBufItrs logic
    while (!rcvBuf.empty())
    {
        if (nodeRole == LEOTP_ROLE_MIDNODE)
        {
            // LOG(DEBUG,"rq size %ld rw %u",rcvQueue.size(), LEOTP_WND_RCV);
            if (rcvQueue.size() < LEOTP_WND_RCV)
            {
                rcvQueue.splice(rcvQueue.end(), rcvBuf, rcvBuf.begin(), rcvBuf.end());
            }
            else
            {
                break;
            }
        }
        else
        {
            shared_ptr<LeotpSeg> seg = *rcvBuf.begin();
            if (seg->rangeStart == rcvNxt && rcvQueue.size() < LEOTP_WND_RCV)
            {
                rcvNxt = seg->rangeEnd;
                rcvQueue.splice(rcvQueue.end(), rcvBuf, rcvBuf.begin());
            }
            else
            {
                break;
            }
        }
    }
    if (nodeRole == LEOTP_ROLE_REQUESTER)
    {
        while (!rcvBufItrs.empty() && rcvNxt >= rcvBufItrs.begin()->endByte)
        {
            rcvBufItrs.erase(rcvBufItrs.begin());
        }
        if (rcvNxt > rcvBufItrs.begin()->startByte)
        {
            rcvBufItrs.begin()->startByte = rcvNxt;
        }
    }
}

// for debug
double parse_data_time = 0;
double detect_time = 0;
IUINT32 last_print_time = 0;
//---------------------------------------------------------------------
// input data
//---------------------------------------------------------------------
int LeotpTransCB::input(char *data, int size)
{
    if (data == NULL || (int)size < (int)LEOTP_OVERHEAD)
        return -1;

    // when receiving udp packet, we use judgeDst() to get info from the
    // first intcp seg, to decide which IntcpSess it should be inputed to.
    // if multiple intcp segs are concatenated in this single udp packet,
    // and they have different dst, there will be error.
    // so, now we only allow one intcp seg per input().
    IUINT32 ts, len;
    IUINT32 rangeStart, rangeEnd; // intcp
    IINT16 wnd;
    IUINT8 cmd;
    shared_ptr<LeotpSeg> seg;
    bool data_header;

    char *dataOrg = data;
    long sizeOrg = size;
    while (1)
    {
        IUINT32 current = _getMillisec();
        if (size < (int)LEOTP_OVERHEAD)
            break;
        data = decode8u(data, &cmd);
        data = decode16(data, &wnd);
        data = decode32u(data, &ts);
        data = decode32u(data, &len);
        data = decode32u(data, &rangeStart);
        data = decode32u(data, &rangeEnd);
        size -= LEOTP_OVERHEAD;

        if ((long)size < (long)len || (int)len < 0)
            return -2;

        if (cmd != LEOTP_CMD_PUSH && cmd != LEOTP_CMD_INT)
            return -3;

        if (cmd == LEOTP_CMD_INT)
        {
#ifdef HBH_CC
            intHopOwd = _getMillisec() - ts;
            rmtSendRate = float(wnd) / 100;
#else
            if(ts!=0&&wnd>0){
                intHopOwd = _getMillisec() - ts;
                rmtSendRate = float(wnd) / 100;
            }
#endif
            if(nodeRole==LEOTP_ROLE_RESPONDER)
                LOG(TRACE, "%u recv int [%u,%u) %u rSR %.1f",
                    _getMillisec(), rangeStart, rangeEnd, rangeEnd - rangeStart, rmtSendRate);
            if (!(rangeStart == 0 && rangeEnd == 0))
            {
                LOG(TRACE,"recv int [%u,%u]",rangeStart,rangeEnd);
        #ifdef HBH_CC
                parseInt(rangeStart,rangeEnd);
        #else
                parseInt(rangeStart,rangeEnd,wnd,ts);
        #endif
            }
        }
        else if (cmd == LEOTP_CMD_PUSH)
        {
            bool data_header = (bool)(len == 0);
            if((!data_header) && (wnd!=0)){
                rmt_sndq_rest = wnd * LEOTP_MSS; // TODO for midnode, ignore this part
                LOG(TRACE, "%d", wnd);
            }
            if (current > ts)
            {
#ifdef HBH_CC
                updateHopRTT(current - ts);
                LOG(TRACE,"hopRTT %u",current - ts);
#else
                if(ts!=0){
                    updateHopRTT(current - ts);
                    LOG(TRACE,"hopRTT %u",current - ts);
                }
#endif
            }
            else
            {
                LOG(TRACE, "_getMillisec()>ts");
            }
            if (hopSrtt != 0)
            { // only begin calculating throughput when hoprtt exists
                recvedBytesThisHRTT += (LEOTP_OVERHEAD+len);
                stat.recvedUDP += len;
                if (lastThrpUpdateTs == 0)
                    lastThrpUpdateTs = current;
                if (_itimediff(current, lastThrpUpdateTs) > hopSrtt)
                { // hopSrtt
                    recvedBytesLastHRTT = recvedBytesThisHRTT;
                    thrpLastPeriod = bytesToMbit(recvedBytesThisHRTT) / (current - lastThrpUpdateTs) * 1000;
                    LOG(TRACE, "receive rate = %.2fMbps", thrpLastPeriod);
                    recvedBytesThisHRTT = 0;
                    lastThrpUpdateTs = current;
                }
            }
            updateCwnd(len);
            if (current - lastSendIntTs > hopSrtt * 0.9)
            {
#ifdef HBH_CC
                outputInt(0, 0);
#else
                outputInt(0, 0, 0, 0);
#endif
            }
            IUINT32 cf = _getUsec();
            bool foundDataHole = detectDataHole(rangeStart, rangeEnd);
            IUINT32 df = _getUsec();
            detect_time += ((double)(df - cf)) / 1000000;
            hasLossEvent = hasLossEvent || foundDataHole;

            seg = createSeg(len);
            seg->cmd = cmd;
            seg->wnd = wnd;
            seg->ts = ts;
            seg->len = len;
            seg->rangeStart = rangeStart;
            seg->rangeEnd = rangeEnd;
            if (data_header)
            {
                LOG(TRACE, " recv header [%u,%u]", rangeStart, rangeEnd);
                parseData(seg, data_header);
            }
            else if (rangeEnd - rangeStart != len)
            {
                LOG(DEBUG, "inconsistent data [%u,%u] len %u", rangeStart, rangeEnd, len);
                break;
            }
            else
            {
                LOG(TRACE, "recv data [%d,%d), ts %u", rangeStart, rangeEnd,_getMillisec());
                cf = _getUsec();
                memcpy(seg->data, data, len);
                parseData(seg, data_header);
                df = _getUsec();
                parse_data_time += ((double)(df - cf)) / 1000000;
            }
            if (df - last_print_time > 1000000)
            {
                LOG(TRACE, "detect hole time %.2f parse data time %.2f", detect_time, parse_data_time);
                last_print_time = df;
            }
        }
        else
        {
            return -3;
        }

        data += len;
        size -= len;
    }

    return 0;
}

//---------------------------------------------------------------------
// flush
//---------------------------------------------------------------------

void LeotpTransCB::flushIntQueue()
{
    while (!intQueue.empty())
    {
        shared_ptr<LeotpSeg> newseg = createSeg(0);
        assert(newseg);
        newseg->len = 0;
        newseg->cmd = LEOTP_CMD_INT;
        newseg->xmit = 0;

        bool first = true;
        // NOTE assume that rangeEnd of interest in intQueue is in order
        for (list<ByteRange>::iterator iter = intQueue.begin(); iter != intQueue.end();)
        {
            if (first)
            {
                newseg->rangeStart = iter->startByte;
                newseg->rangeEnd = _imin_(iter->endByte, newseg->rangeStart + LEOTP_INT_RANGE_LIMIT);
#ifndef HBH_CC
                if(nodeRole==LEOTP_ROLE_MIDNODE){
                    newseg->wnd = iter->wnd;
                    newseg->ts = iter->ts;
                }
#endif
                first = false;
            }
            else
            {
                if (iter->startByte == newseg->rangeEnd)
                {
                    LOG(TRACE, "%u %u %u %u", newseg->rangeStart, newseg->rangeEnd, iter->startByte, iter->endByte);
                    newseg->rangeEnd = _imin_(iter->endByte,
                                              newseg->rangeStart + LEOTP_INT_RANGE_LIMIT);
                }
                else
                {
                    break;
                }
            }
            if (iter->endByte <= newseg->rangeEnd)
            {
                intQueue.erase(iter++);
            }
            else
            {
                iter->startByte = newseg->rangeEnd;
                break;
            }
        }
        // intRangeLimit -= newseg->rangeEnd-newseg->rangeStart;
        intBufBytes += newseg->rangeEnd - newseg->rangeStart;
        intBuf.push_back(newseg);
    }
}

void LeotpTransCB::flushIntBuf()
{
    if (intBufBytes == 0)
        return;

    IUINT32 current = _getMillisec();
    IUINT32 flushIntv = LEOTP_UPDATE_INTERVAL;
    if (lastFlushTs != 0)
    {
        flushIntv = current - lastFlushTs;
    }
    int newOutput;
    if (srtt != 0)
    {
        // TODO thrpLastPeriod -> thrp at producer
        newOutput = float(rmt_sndq_rest) * flushIntv / srtt + mbitToBytes(thrpLastPeriod) * flushIntv / 1000;
        newOutput = max(newOutput, mbitToBytes(LEOTP_SENDRATE_MIN) * int(flushIntv) / 1000);
        intOutputLimit += newOutput;
    }
    LOG(TRACE, "%d %d %d %d %ld %d", rmt_sndq_rest, flushIntv, srtt, intOutputLimit, intQueue.size(), intBufBytes);
    char *sentEnd = tmpBuffer.get();
    int sizeToSend = 0;
    // from intBuf to udp
    list<shared_ptr<LeotpSeg>>::iterator p, next;

    int cntAll = 0, cntTimeout = 0, cntRetransed = 0, cntNeedSend = 0;
    int intBufSize = intBuf.size();
    bool reach_limit = false;
    int loop = 0;
    for (p = intBuf.begin(); p != intBuf.end(); p = next)
    {
        loop++;
        cntAll++;
        next = p;
        next++;
        IUINT32 current = _getMillisec();
        shared_ptr<LeotpSeg> segPtr = *p;
        IUINT32 segRto;
        int needsend = 0;
        if (nodeRole == LEOTP_ROLE_MIDNODE)
        {
            needsend = 1;
        }
        else
        {
            // RTO mechanism
            if (segPtr->xmit >= 2)
            {
                cntRetransed++;
            }
            if (segPtr->xmit == 0)
            {
                needsend = 1;
            }
            else
            {
                // NOTE RTO function: segRto=f(rto, xmit)
                segRto = rto * (pow(1.5, segPtr->xmit - 1) + 1); // + 1000;
                if (_itimediff(current, segPtr->ts) >= segRto)
                {
                    needsend = 1;
                }
            }
        }

        if (needsend)
        {
            cntNeedSend += 1;
            if (nodeRole == LEOTP_ROLE_REQUESTER)
            {
                if (intOutputLimit < segPtr->rangeEnd - segPtr->rangeStart)
                { // intOutputLimit<segPtr->rangeEnd - segPtr->rangeStart
                    LOG(TRACE, "intOutputLimit %d bytes seglen %d qsize %ld",
                        intOutputLimit, segPtr->rangeEnd - segPtr->rangeStart, sndQueue.size());
                    reach_limit = true;
                    break;
                }
                else
                {
                    intOutputLimit -= segPtr->rangeEnd - segPtr->rangeStart;
                    LOG(TRACE, "->%d", intOutputLimit);
                }
            }
            if (segPtr->xmit > 0)
            {
                if (segPtr->xmit > 0)
                { // 1
                    LOG(TRACE, "----- Timeout [%d,%d) xmit %d cur %u rto %d -----",
                        segPtr->rangeStart, segPtr->rangeEnd, segPtr->xmit, _getMillisec(), rto);
                }
                if (segPtr->xmit >= LEOTP_DEADLINK)
                { // || segRto>=LEOTP_RTO_MAX) {
                    state = -1;
                    LOG(ERROR, "dead link");
                    exit(0);
                }
                hasLossEvent = true;
                cntTimeout++;
                stat.xmit++;
            }
            // clear hole
            // DEBUG
            deleteDataHole(segPtr->rangeStart, segPtr->rangeEnd);



#ifdef HBH_CC
            outputInt(segPtr->rangeStart, segPtr->rangeEnd);
#else
            outputInt(segPtr->rangeStart, segPtr->rangeEnd,segPtr->wnd,segPtr->ts);
#endif
            segPtr->xmit++;
            segPtr->ts = current;

            if (nodeRole == LEOTP_ROLE_MIDNODE)
            {
                intBufBytes -= (*p)->rangeEnd - (*p)->rangeStart;
                intBuf.erase(p);
            }
        }
    }
    LOG(TRACE, "loop %d", loop);
    if (cntTimeout > 0)
        LOG(TRACE, "flushIntBuf Intbuf size %d cntAll %d needSend %d retransed %d timeout %d", intBufSize, cntAll, cntNeedSend, cntRetransed, cntTimeout);
    if (srtt != 0 && !reach_limit)
    {
        intOutputLimit = min(intOutputLimit, newOutput);
    }
    stat.cntTimeout += cntTimeout;
    if (cntTimeout > 0)
    {
        LOG(TRACE, "RTO %d %d/%d/%d", rto, cntTimeout, cntRetransed, cntAll);
        conseqTimeout++;
    }
    if (RTTscheme == LEOTP_RTT_SCHM_EXPO && cntTimeout > 0 && conseqTimeout < 10)
    {
        srtt = srtt * LEOTP_RTO_EXPO;
        rto = rto * LEOTP_RTO_EXPO;
    }
}

// sndQueue -> send straightforward;
void LeotpTransCB::flushData()
{
    IUINT32 current = _getMillisec();
    IUINT32 flushIntv = LEOTP_UPDATE_INTERVAL;
    if (lastFlushTs != 0)
    {
        flushIntv = current - lastFlushTs;
    }

    // TODO CC -- cwnd/sendingRate; design token bucket
    LOG(TRACE, "%.1f %u", rmtSendRate, flushIntv);
    int newOutput = mbitToBytes(rmtSendRate * flushIntv / 1000);
    dataOutputLimit += newOutput;
    LOG(TRACE, "dataOutputLimit %d bytes %ld", dataOutputLimit, sndQueue.size());

    char *sentEnd = tmpBuffer.get();
    int sizeToSend = 0;

    bool reach_limit = false;
    list<shared_ptr<LeotpSeg>>::iterator p, next;
    shared_ptr<LeotpSeg> segPtr;
#ifndef HBH_CC
    if(nodeRole==LEOTP_ROLE_MIDNODE)
        dataOutputLimit = 65536;
#endif
    for (p = sndQueue.begin(); p != sndQueue.end(); p = next)
    {
        next = p;
        next++;
        segPtr = *p;
        if (dataOutputLimit < segPtr->len+LEOTP_OVERHEAD)
        {
            LOG(TRACE, "dataOutputLimit %d bytes seglen %d qsize %ld", dataOutputLimit, segPtr->len, sndQueue.size());

            reach_limit = true;
            break;
        }
        else
        {
        #ifdef HBH_CC
            dataOutputLimit -= (LEOTP_OVERHEAD+segPtr->len);
        #else
            if(nodeRole==LEOTP_ROLE_RESPONDER){
                dataOutputLimit -= (LEOTP_OVERHEAD+segPtr->len);
            }
        #endif
            stat.sentLEOTP += segPtr->len;
        }

        sizeToSend = (int)(sentEnd - tmpBuffer.get());
        if (sizeToSend + (LEOTP_OVERHEAD + segPtr->len) > LEOTP_MTU)
        {
            output(tmpBuffer.get(), sizeToSend, LEOTP_ROLE_REQUESTER);
            sentEnd = tmpBuffer.get();
        }

#ifdef HBH_CC
        segPtr->ts = _getMillisec() - intHopOwd;
#else
        if(nodeRole==LEOTP_ROLE_RESPONDER)
            segPtr->ts = _getMillisec() - intHopOwd;
#endif

        // make sure the midnode doesn't change wnd from responder;
        // TODO if it's from cache, wnd=0
        if (nodeRole == LEOTP_ROLE_RESPONDER)
        {
            segPtr->wnd = getIntDev();
            LOG(TRACE, "%d %d", sndQueueBytes, segPtr->wnd);
        }
        LOG(TRACE, "flushData [%d,%d), ts %u, wnd %d, sndQ %u", segPtr->rangeStart, segPtr->rangeEnd, segPtr->ts, segPtr->wnd, sndQueueBytes/LEOTP_MSS);
        sentEnd = encodeSeg(sentEnd, segPtr.get());
        memcpy(sentEnd, segPtr->data, segPtr->len);
        sentEnd += segPtr->len;
        sndQueueBytes -= (segPtr->len+LEOTP_OVERHEAD);
        sndQueue.erase(p);
    }
    // if cwnd is not enough for data, the remain wnd can be used for next loop
    if (!reach_limit)
    {
        dataOutputLimit = min(dataOutputLimit, newOutput); // 0;
    }
    // flush remain segments
    sizeToSend = (int)(sentEnd - tmpBuffer.get());
    if (sizeToSend > 0)
    {
        output(tmpBuffer.get(), sizeToSend, LEOTP_ROLE_REQUESTER);
    }
}

void LeotpTransCB::flush()
{
    IUINT32 tmp = _getMillisec();
    // 'update' haven't been called.
    if (updated == 0)
        return;

    if (nodeRole != LEOTP_ROLE_RESPONDER)
    {
        flushIntQueue();
        flushIntBuf();
        flushDataHoles();
    }
    if (nodeRole != LEOTP_ROLE_REQUESTER)
    {
        flushData();
    }
    lastFlushTs = tmp;
}

//---------------------------------------------------------------------
// update state (call it repeatedly, every 10ms-100ms), or you can ask
// check when to call it again (without input/_send calling).
// 'current' - current timestamp in millisec.
//---------------------------------------------------------------------
void LeotpTransCB::update()
{
    IUINT32 current = _getMillisec();
    if (current - stat.lastPrintTs > 1000)
    {
        rcvBuf.clear();
        rcvBufItrs.clear();

        if (nodeRole == LEOTP_ROLE_REQUESTER)
        {
            LOG(SILENT, "%u. %4d %d C %.1f ↑%.1f ↓%.1f+%.2f iQ %ld iB %d rB %ld T %d D %d Hthrp %.2f",
                current, srtt, hopSrtt,
                cwnd,
                float(getDataSendRate()) / 100,
                bytesToMbit(stat.recvedLEOTP) * 1000 / (current - stat.lastPrintTs),
                bytesToMbit(stat.recvedUDP - stat.recvedLEOTP) * 1000 / (current - stat.lastPrintTs),
                intQueue.size(),
                intBufBytes / LEOTP_MSS,
                rcvBuf.size(),
                stat.cntTimeout, stat.cntDataHole,
                thrpLastPeriod);
            // NOTE
            printf("  %4ds %.2f Mbits/sec receiver\n",
                   (current - stat.startTs) / 1000,
                   bytesToMbit(stat.recvedLEOTP) * 1000 / (current - stat.lastPrintTs));
            fflush(stdout);
        }
        if (nodeRole == LEOTP_ROLE_MIDNODE)
        {
            LOG(SILENT, "CC| %d C %.1f ↑%.1f ↓%.1f I %d D %d Hthrp %.2f",
                hopSrtt,
                cwnd,
                float(getDataSendRate()) / 100,
                bytesToMbit(stat.recvedUDP) * 1000 / (current - stat.lastPrintTs),
                stat.cntIntHole, stat.cntDataHole,
                thrpLastPeriod);
            fflush(stdout);
        }
        if (nodeRole != LEOTP_ROLE_REQUESTER)
        {
            LOG(SILENT, "%4d r↑%.1f sent %.1f sQ %d I %d",
                // stat.ssid,
                (current - stat.startTs) / 1000,
                rmtSendRate,
                bytesToMbit(stat.sentLEOTP) * 1000 / (current - stat.lastPrintTs),
                sndQueueBytes / LEOTP_MSS,
                stat.cntIntHole);
            fflush(stdout);
        }
        stat.reset();
    }

    if (updated == 0)
    {
        updated = 1;
        nextFlushTs = current;
    }

    IINT32 slap = _itimediff(current, nextFlushTs);

    if (slap >= 0 || slap < -10000)
    {
        flush();
        if (slap >= LEOTP_UPDATE_INTERVAL || slap < -10000)
        {
            nextFlushTs = current + LEOTP_UPDATE_INTERVAL;
        }
        else
        {
            nextFlushTs = nextFlushTs + LEOTP_UPDATE_INTERVAL;
        }
    }
}

//---------------------------------------------------------------------
// Determine when should you invoke update:
// returns when you should invoke update in millisec, if there
// is no input/_send calling. you can call update in that
// time, instead of call update repeatly.
// Important to reduce unnacessary update invoking. use it to
// schedule update (eg. implementing an epoll-like mechanism,
// or optimize update when handling massive kcp connections)
//---------------------------------------------------------------------
IUINT32 LeotpTransCB::check()
{
    IUINT32 currentU = _getMillisec();
    if (updated == 0)
    {
        return currentU;
    }
    IUINT32 _ts_flush = nextFlushTs;
    if (_itimediff(currentU, _ts_flush) >= 0 ||
        _itimediff(currentU, _ts_flush) < -10000)
    {
        return currentU;
    }

    IUINT32 tmin = _ts_flush; //_ts_flush>currentU is guaranteed
    
    tmin = _imin_(tmin, currentU + LEOTP_UPDATE_INTERVAL);
    return tmin;
}

// rate limitation on sending data
IINT16 LeotpTransCB::getDataSendRate()
{
    float rate;
    if (hopSrtt == 0)
    { // haven't receive feedback
        rate = LEOTP_SENDRATE_MIN;
    }
    else
    {
        // suppose rcvBuf and rcvQueue is always big enough
        // NOTE hopSrtt -> minHrtt
        int minHrtt = 999999;
        for (auto pr : hrttQueue)
        {
            minHrtt = min(minHrtt, pr.second);
        }
        rate = bytesToMbit(cwnd * LEOTP_MSS) / minHrtt * 1000; // Mbps
        
        if (nodeRole != LEOTP_ROLE_REQUESTER)
        { // MIDNODE
            float rateForQueue = (bytesToMbit(LEOTP_SNDQ_MAX) - bytesToMbit(sndQueueBytes)) / hopSrtt * 1000 + rmtSendRate;
            LOG(TRACE, "rate %.2f rateForQueue %.2f rmtSendRate %.2f", rate, rateForQueue,rmtSendRate);
            rate = min(rate, rateForQueue);
        }
        rate = max(rate, LEOTP_SENDRATE_MIN);
    }
    rate = min(rate, LEOTP_SENDRATE_MAX);
    // DEBUG
    //return IINT16(18*100);
    return IINT16(rate * 100);
    
}
// deviation of sendQueueBytes - LEOTP_SNDQ_MAX
IINT16 LeotpTransCB::getIntDev()
{
    return IINT16(LEOTP_SNDQ_MAX / LEOTP_MSS) - IINT16(sndQueueBytes / LEOTP_MSS);
}

// cc
void LeotpTransCB::updateCwnd(IUINT32 dataLen)
{
    IUINT32 current = _getMillisec();

    bool congSignal;
    int minHrtt = 99999999;
    if (CCscheme == LEOTP_CC_SCHM_LOSSB)
    {
        congSignal = hasLossEvent;
        hasLossEvent = false;
    }
    else if (CCscheme == LEOTP_CC_SCHM_RTTB)
    {
        while (!hrttQueue.empty() && current - hrttQueue.begin()->first > HrttMinWnd)
        {
            hrttQueue.pop_front();
        }
        // NOTE avoid too long hrttQueue
        if (current > (hrttQueue.rbegin())->first + hopSrtt / 2)
        {
            hrttQueue.push_back(pair<IUINT32, int>(current, hopSrtt));
        }
        for (auto pr : hrttQueue)
        {
            minHrtt = min(minHrtt, pr.second);
        }
        if (thrpLastPeriod == -1)
        {
            congSignal = false;
        }
        else
        {
            congSignal = mbitToBytes(thrpLastPeriod) * (hopSrtt - minHrtt) / 1000 > QueueingThreshold;
        }
    }
    float cwndOld = cwnd; // for debug

    ccDataLen += dataLen;

    if (ccState == LEOTP_CC_SLOW_START)
    {
        if (congSignal || cwnd >= LEOTP_SSTHRESH_INIT)
        { // entering ca
            ccDataLen = cwnd * LEOTP_MSS;
            ccState = LEOTP_CC_CONG_AVOID;
        }
        else
        {
            // NOTE 5.0
            cwnd = (ccDataLen / LEOTP_MSS) * (pow(2, min(5.0, double(hopSrtt) / LEOTP_RTT0)) - 1);
        }
    }
    if (ccState == LEOTP_CC_CONG_AVOID && allow_cwnd_decrease(current))
    {
        if (congSignal)
        {
            // NOTE cwnd decrease function
            if (CCscheme == LEOTP_CC_SCHM_LOSSB)
            {
                cwnd = max(cwnd / 2, LEOTP_CWND_MIN);
            }
            else if (CCscheme == LEOTP_CC_SCHM_RTTB)
            {
                float cwndNew = (float(mbitToBytes(thrpLastPeriod)) / 1000 * minHrtt / LEOTP_MSS) * 0.8;
                LOG(TRACE, "--- hopSrtt %d minHrtt %d C %.1f->%.1f ↓%.1f delta %u --- ", hopSrtt, minHrtt, cwnd, cwndNew, thrpLastPeriod, hopSrtt - minHrtt);
                cwnd = max(cwndNew, LEOTP_CWND_MIN);
            }
            lastCwndDecrTs = current;
            ccDataLen = 0;
            congSignal = false;
        }
        else
        {
            bool allowInc = allow_cwnd_increase();
            if (ccDataLen > cwnd * LEOTP_MSS / 10 && allowInc)
            {
                cwnd += 1; // 0.1
                ccDataLen = 0;
            }
            else if (ccDataLen > 5 * cwnd * LEOTP_MSS / 10 && (!allowInc))
            {
                cwnd = max(LEOTP_CWND_MIN, cwnd - 0.1f);
                ccDataLen = 0;
            }
        }
    }
    if (cwndOld != cwnd)
    {
        LOG(TRACE, "%u cwnd %.1f", current, cwnd);
    }
}

bool LeotpTransCB::allow_cwnd_increase()
{
    if (thrpLastPeriod == 0 || cwnd == 0)
        return true;
    if (thrpLastPeriod < (bytesToMbit(cwnd * LEOTP_MSS) / hopSrtt * 1000) / 2)
        return false;
    return true;
}

bool LeotpTransCB::allow_cwnd_decrease(IUINT32 current)
{
    if (lastCwndDecrTs == 0 || hopSrtt == 0)
        return true;
    // NOTE rtt*2
    if (_itimediff(current, lastCwndDecrTs) < hopSrtt * 2)
        return false;
    return true;
}

// assume that there is exactly one INTCP packet in one recvUDP()
int LeotpTransCB::judgeSegDst(const char *data, long size)
{
    if (data == nullptr || (int)size < (int)LEOTP_OVERHEAD)
        return -1;
    IUINT8 cmd;
    // have to use a typecasting here, not good
    decode8u((char *)data, &cmd);
    if (cmd == LEOTP_CMD_INT)
    {
        return LEOTP_ROLE_RESPONDER;
    }
    else
    {
        return LEOTP_ROLE_REQUESTER;
    }
}

// peek data size
int LeotpTransCB::peekSize()
{

    if (rcvQueue.empty())
        return -1; // recv_queue

    return (*rcvQueue.begin())->len;
}

int LeotpTransCB::getRwnd()
{
    if (rcvQueue.size() < LEOTP_WND_RCV)
    {
        return LEOTP_WND_RCV - rcvQueue.size();
    }
    return 0;
}

int LeotpTransCB::getWaitSnd()
{
    return sndQueue.size();
}

float LeotpTransCB::getCwnd()
{
    return cwnd;
}

//--- Mega=1000*1000 ---//

float bytesToMbit(int bytes)
{
    return float(bytes) / 125000;
}
int mbitToBytes(float mbit)
{
    return mbit * 125000;
}
