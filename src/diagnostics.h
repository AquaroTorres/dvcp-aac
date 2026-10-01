#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// Opt-in diagnostics; normal installations never write to a developer path.
inline void aacLogV(const char* format, va_list args) {
    const char* path=std::getenv("RESOLVE_AAC_LOG");
    if(!path || !*path)return;
    FILE* file=std::fopen(path,"a");
    if(!file)return;
    std::fprintf(file,"pid=%d ",int(getpid()));
    std::vfprintf(file,format,args);
    std::fputc('\n',file);
    std::fclose(file);
}
inline void aacLog(const char* format,...) {
    va_list args;va_start(args,format);aacLogV(format,args);va_end(args);
}
