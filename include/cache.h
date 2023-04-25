#ifndef __CACHE_H__
#define __CACHE_H__

#include "ByteMap.h"
#include "generality.h"
#include "log.h"

#include <list>
#include <mutex>

using namespace std;

// #include <queue>

#define BLOCK_LEN 4096
#define BLOCK_SEG_NUM 100
#define MAX_BLOCK_NUM 20000 // 40MB

struct Block;
struct Node{
    ByteMap<shared_ptr<Block>>::iterator blockIter;
};
struct Block {
    char dataPtr[BLOCK_LEN];
    IUINT32 ranges[BLOCK_SEG_NUM*2];
    IUINT32 lastPos = -1;
    list<Node>::iterator nodeIter;
};
// struct BlockInfo {
//     ByteMap<Block>::iterator iter;
//     IUINT32 lastTime;
// };
// struct cmp{
//     bool operator()(BlockInfo a, BlockInfo b){
//         return a.lastTime > b.lastTime; //min heap
//     }
// };

class Cache
{
public: // need lock
    ByteMap<shared_ptr<Block>> dataMap;
    void nameSeqToKey(char* buf, const char* name, IUINT32 index);

    
    Cache(int nameLen);
    int insert(const char* name, IUINT32 dataStart, IUINT32 dataEnd, const char* dataBuf);
    int read(const char* name, IUINT32 dataStart, IUINT32 dataEnd, char* dataBuf);
private: // doesn't need lock
    int KeyLen;
    // priority_queue<BlockInfo, vector<BlockInfo>, cmp> queueLRU;
    list<Node> lruList;
    // key = name + blockStart
    // KeyLen = name + sizeof(IUNIT32)
    mutex lock;
    shared_ptr<Block> addBlock(const char* key);
    void dropBlock(list<Node>::iterator iter);
    void updateLRU(shared_ptr<Block> blockPtr);
    int checksum(const char* keyChars);
};


#endif
