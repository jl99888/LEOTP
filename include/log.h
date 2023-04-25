#ifndef __LOG_H__
#define __LOG_H__

#include <stdio.h>
#include <string.h>

#define TRACE 0
#define DEBUG 2
#define INFO 4
#define WARN 6
#define SILENT 8
#define ERROR 10

#define LOG_LEVEL DEBUG

#define LOG(level, format, ...) \
    if(level>=LOG_LEVEL){ \
        char fileStr[100]{__FILE__}; \
        const char *ptrL = strrchr(fileStr,'/'); \
        if(ptrL==NULL) { \
            ptrL = fileStr; \
        }else{ \
            ptrL++; \
        } \
        char prefix[50]; \
        memset(prefix,' ',50); \
        int FuncNameLen=8,ProgramNameLen=10,LineLen=4; \
        int TotalLen=FuncNameLen+ProgramNameLen+LineLen+2; \
        snprintf(prefix, 50, "%*.*s@%*.*s %*d", \
                FuncNameLen,FuncNameLen,__func__, \
                ProgramNameLen,ProgramNameLen,ptrL, \
                LineLen,__LINE__); \
        prefix[TotalLen] = '|'; \
        prefix[TotalLen+1] = ' '; \
        prefix[TotalLen+2] = '\0'; \
        printf("%s" format "\n",prefix,##__VA_ARGS__); \
    }

#endif