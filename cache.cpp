#include "./include/cache.h"
#undef LOG_LEVEL
#define LOG_LEVEL DEBUG

using namespace std;

#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))

int Cache::checksum(const char* keyChars){
    int cs=0;
    for(int i=0;i<KeyLen;i++){
        cs += (unsigned char)keyChars[i];
    }
    return cs;
}

void Cache::nameSeqToKey(char* buf, const char* name, IUINT32 index){
    memcpy(buf, name, KeyLen-sizeof(IUINT32));
    memcpy(buf+KeyLen-sizeof(IUINT32), &index, sizeof(IUINT32));
}

Cache::Cache(int nameLen):KeyLen(nameLen+sizeof(IUINT32)){
}

shared_ptr<Block> Cache::addBlock(const char* keyChars){
    shared_ptr<Block> blockPtr(new Block);
    dataMap.setValue(keyChars, KeyLen, blockPtr);
    // cout<<"[Cache::addBlock] dataMap.setValue "<<checksum(keyChars)<<endl;
    
    Node newNode;
    // in map
    newNode.blockIter = dataMap.findIter(keyChars, KeyLen);
    // in lruList
    lruList.push_back(newNode);
    if(lruList.size() > MAX_BLOCK_NUM){
        dropBlock(lruList.begin());
    }
    list<Node>::iterator iter = lruList.end();
    blockPtr->nodeIter = --iter;
    memset(blockPtr->ranges, 0, BLOCK_SEG_NUM*2*sizeof(IUINT32));

    return blockPtr;
}

void Cache::dropBlock(list<Node>::iterator iter){
    // delete block in map
    dataMap.erase(iter->blockIter);
    // delete block in lruList
    lruList.erase(iter);
}
void Cache::updateLRU(shared_ptr<Block> blockPtr){
    // move node in lruList to lruList's end
    lruList.splice(lruList.end(), lruList, blockPtr->nodeIter);
}
//TODO seq over UINT32_MAX
int Cache::insert(const char* name, IUINT32 dataStart, IUINT32 dataEnd, const char* dataBuf){
    lock.lock();
    shared_ptr<Block> blockPtr;
    char tmpKeyChars[100];
    //divide data to blocks
    IUINT32 blockStart = (dataStart/BLOCK_LEN)*BLOCK_LEN;
    for(; blockStart<dataEnd; blockStart+=BLOCK_LEN){
        nameSeqToKey(tmpKeyChars, name, blockStart);
        // cout<<"[Cache::insert] insert cache "<<checksum(tmpKeyChars)<<' '<<blockStart<<endl;
        int ret = dataMap.readValue(tmpKeyChars, KeyLen, &blockPtr);
        IUINT32 writeStart = max(dataStart, blockStart);
        IUINT32 writeEnd = min(blockStart+BLOCK_LEN, dataEnd);
        if(ret == -1){
            blockPtr = addBlock(tmpKeyChars);

            blockPtr->ranges[0] = writeStart;
            blockPtr->ranges[1] = writeEnd;
        }else{
            //check data range union
            IUINT32 newRangeStart = writeStart, newRangeEnd = writeEnd;
            bool isSubset = false;
            while(1){
                bool hasUnion = false;
                for(int i=0;i<BLOCK_SEG_NUM;i++){
                    //empty range
                    if(blockPtr->ranges[i*2]==0 && blockPtr->ranges[i*2+1]==0){
                        continue;
                    }
                    //has union
                    if((newRangeStart < blockPtr->ranges[i*2] && newRangeEnd >= blockPtr->ranges[i*2])
                            || (newRangeEnd > blockPtr->ranges[i*2+1] && newRangeStart <= blockPtr->ranges[i*2+1])){
                        newRangeStart = min(newRangeStart, blockPtr->ranges[i*2]);
                        newRangeEnd = max(newRangeEnd, blockPtr->ranges[i*2+1]);
                        blockPtr->ranges[i*2] = blockPtr->ranges[i*2+1] = 0;
                        hasUnion = true;
                        break;
                    }
                    // new range is a subset of one existing range
                    if(newRangeStart >= blockPtr->ranges[i*2] && newRangeEnd <= blockPtr->ranges[i*2+1]){
                        isSubset = true;
                        break;
                    }
                }
                if(!hasUnion || isSubset)
                    break;
            }
            if(!isSubset){
                bool overflow = true;
                for(int i=0;i<BLOCK_SEG_NUM;i++){
                    if(blockPtr->ranges[i*2]==0 && blockPtr->ranges[i*2+1]==0){
                        blockPtr->ranges[i*2]=newRangeStart;
                        blockPtr->ranges[i*2+1]=newRangeEnd;
                        overflow = false;
                        break;
                    }
                }
                if(overflow){
                    int pos = (blockPtr->lastPos+1) % BLOCK_SEG_NUM;
                    blockPtr->lastPos = pos;
                    blockPtr->ranges[pos*2]=newRangeStart;
                    blockPtr->ranges[pos*2+1]=newRangeEnd;
                    lock.unlock();
                    return 0;
                }
            }
        }

        memcpy(blockPtr->dataPtr+(writeStart-blockStart), dataBuf+(writeStart-dataStart), writeEnd-writeStart);
    }
    lock.unlock();
    return 0;
}


int Cache::read(const char* name, IUINT32 dataStart, IUINT32 dataEnd, char* dataBuf){
    lock.lock();
    shared_ptr<Block> blockPtr;
    char tmpKeyChars[KeyLen];

    //divide data to blocks
    IUINT32 blockStart = (dataStart/BLOCK_LEN)*BLOCK_LEN;
    int readlen = 0, ret;
    IUINT32 readStart, readEnd;
    for(; blockStart<dataEnd; blockStart+=BLOCK_LEN){
        nameSeqToKey(tmpKeyChars, name, blockStart);
        // for(int i=0;i<KeyLen-sizeof(IUINT32);i++){
        //     cout<<name[i]-'\0'<<' ';
        // }
        // cout<<endl;
        LOG(TRACE,"read cache cks=%d start=%d",checksum(tmpKeyChars),blockStart);
        int ret = dataMap.readValue(tmpKeyChars, KeyLen, &blockPtr);
        if(ret == -1){
            LOG(TRACE,"no block. map size=%d",dataMap.size());
            break; // no block
        }

        readStart = max(dataStart, blockStart);
        readEnd = -1;
        for(int i=0;i<BLOCK_SEG_NUM;i++){
            // cout<<"block seg "<<blockPtr->ranges[i*2]<<' '<<blockPtr->ranges[i*2+1]<<endl;
            //empty range
            if(blockPtr->ranges[i*2]==0 && blockPtr->ranges[i*2+1]==0){
                continue;
            }
            //cover readStart
            if(blockPtr->ranges[i*2]<=readStart && blockPtr->ranges[i*2+1]>readStart){
                readEnd = min(blockPtr->ranges[i*2+1], dataEnd);
                break;
            }
        }
        if(readEnd==-1)
            break; // no satisfying range
        memcpy(dataBuf+(readStart-dataStart), blockPtr->dataPtr+(readStart-blockStart), readEnd-readStart);
        readlen += readEnd-readStart;
        updateLRU(blockPtr);
        // if data request is not completed but a empty segment is encoutered
        if(dataEnd>readEnd && readEnd<blockStart+BLOCK_LEN){
            break;
        }
    }
    lock.unlock();
    return readlen;
}