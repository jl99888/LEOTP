#ifndef __BYTEMAP_H__
#define __BYTEMAP_H__

#include <unordered_map>
#include <cstring>
#include <iostream>
#include <memory>
using namespace std;
class ByteArray{
public:
    int len;
    shared_ptr<char> bytePtr;
    ByteArray(int _len, const char* _bytePtr):len(_len) {
        bytePtr = shared_ptr<char>(new char[len]);
        memcpy(bytePtr.get(), _bytePtr, len);
    }
    // ~ByteArray(){
    //     cout<<(void*)bytePtr.get()<<' '<<bytePtr.use_count()<<endl;
    //     // delete bytePtr;
    // }
};
struct hashBytes
{
    size_t operator()(const ByteArray &ba) const
    {
        // std::cout<<"hash "<<ba.bytePtr<<std::endl;
        return std::_Hash_bytes(ba.bytePtr.get(), ba.len, 0);
    }
};
struct eqBytes
{
    bool operator()(const ByteArray &ba1, const ByteArray &ba2) const
    {
        if(ba1.len==ba2.len && memcmp(ba1.bytePtr.get(), ba2.bytePtr.get(), ba1.len)==0)
            return true;
        return false;
    }
};
template <class T>
class ByteMap{
public:
    typedef typename std::unordered_map<const ByteArray, T, hashBytes, eqBytes>::iterator iterator;
    void setValue(const char* keyChars, int len, const T &value);
    iterator findIter(const char* keyChars, int len);
    int readValue(const char* keyChars, int len, T *valuePtr);
    iterator erase(iterator iter);
    int size();
    std::unordered_map<const ByteArray, T, hashBytes, eqBytes> umap;
};


template <class T>
void ByteMap<T>::setValue(const char* keyChars, int len, const T &value){
    ByteArray key(len, keyChars);
    umap.insert(std::make_pair(key, value));
}

template <class T>
typename ByteMap<T>::iterator ByteMap<T>::findIter(const char* keyChars, int len){
    ByteArray key(len, keyChars);
    iterator iter = umap.find(key);
    return iter;
}
template <class T>
int ByteMap<T>::readValue(const char* keyChars, int len, T *valuePtr){
    iterator iter = findIter(keyChars, len);
    if(iter == umap.end()){
        return -1;
    }else{
        //NOTE operator=() needs to be defined?
        *valuePtr = iter->second;
        return 0;
    }
}
template <class T>
typename ByteMap<T>::iterator ByteMap<T>::erase(typename ByteMap<T>::iterator iter){
    return umap.erase(iter);
}
template <class T>
int ByteMap<T>::size(){
    return umap.size();
}
#endif