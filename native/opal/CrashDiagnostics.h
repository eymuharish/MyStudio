#pragma once
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
namespace studio::opal {
inline char crashDirectory[MAX_PATH]{};
inline std::atomic<bool> crashWritten{false};
inline LONG CALLBACK TraceException(EXCEPTION_POINTERS*exception){
 if(exception->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION||crashWritten.exchange(true))return EXCEPTION_CONTINUE_SEARCH;
 char path[MAX_PATH];sprintf_s(path,"%s\\opal-%lu-crash.txt",crashDirectory,GetCurrentProcessId());HANDLE file=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return EXCEPTION_CONTINUE_SEARCH;
 auto write=[&](const char*value){DWORD count;WriteFile(file,value,DWORD(strlen(value)),&count,nullptr);};char line[2048];sprintf_s(line,"Exception %08lx address=%p thread=%lu\r\n",exception->ExceptionRecord->ExceptionCode,exception->ExceptionRecord->ExceptionAddress,GetCurrentThreadId());write(line);
 CONTEXT context=*exception->ContextRecord;STACKFRAME64 frame{};frame.AddrPC={context.Rip,AddrModeFlat};frame.AddrFrame={context.Rbp,AddrModeFlat};frame.AddrStack={context.Rsp,AddrModeFlat};
 for(int index=0;index<40&&frame.AddrPC.Offset;++index){char buffer[sizeof(SYMBOL_INFO)+1024]{};auto*symbol=reinterpret_cast<SYMBOL_INFO*>(buffer);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=1023;DWORD64 displacement=0;HMODULE module=nullptr;char modulePath[MAX_PATH]{};GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<const char*>(frame.AddrPC.Offset),&module);if(module)GetModuleFileNameA(module,modulePath,MAX_PATH);bool resolved=SymFromAddr(GetCurrentProcess(),frame.AddrPC.Offset,&displacement,symbol);sprintf_s(line,"%02d %016llx %s + %llx %s\r\n",index,frame.AddrPC.Offset,resolved?symbol->Name:"unknown",resolved?displacement:frame.AddrPC.Offset-reinterpret_cast<uint64_t>(module),modulePath);write(line);if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,GetCurrentProcess(),GetCurrentThread(),&frame,&context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;}
 CloseHandle(file);return EXCEPTION_CONTINUE_SEARCH;
}
inline void InstallCrashTrace(){auto*path=std::getenv("ESSASTUDIO_DIAGNOSTIC_CRASH_DIR");if(!path||!*path)return;strcpy_s(crashDirectory,path);SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_UNDNAME);SymInitialize(GetCurrentProcess(),std::getenv("ESSASTUDIO_DIAGNOSTIC_SYMBOL_PATH"),TRUE);AddVectoredExceptionHandler(1,TraceException);}
}
