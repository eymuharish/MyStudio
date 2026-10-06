// Debug only a child worker created by this tool; never attach to other apps.
// Preserve its stdin/stdout contract and exit status. Capture stack addresses
// and symbols without creating a full-memory dump or changing machine policy.
#include <Windows.h>
#include <dbghelp.h>
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <chrono>
std::wstring Quote(const wchar_t* input) {
    std::wstring result=L"\""; unsigned slashes=0;
    for(const wchar_t* p=input;*p;++p) {
        if(*p==L'\\'){++slashes;continue;}
        result.append(*p==L'"'?slashes*2+1:slashes,L'\\'); slashes=0;result+=*p;
    }
    result.append(slashes*2,L'\\'); return result+L'"';
}
void Stack(HANDLE process,DWORD threadId) {
    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,threadId);
    if(!thread)return; CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
    if(!GetThreadContext(thread,&context)){CloseHandle(thread);return;}
    STACKFRAME64 frame{}; frame.AddrPC.Offset=context.Rip;frame.AddrPC.Mode=AddrModeFlat;
    frame.AddrFrame.Offset=context.Rbp;frame.AddrFrame.Mode=AddrModeFlat;
    frame.AddrStack.Offset=context.Rsp;frame.AddrStack.Mode=AddrModeFlat;
    for(unsigned n=0;n<50&&frame.AddrPC.Offset;++n) {
        alignas(SYMBOL_INFO) unsigned char storage[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
        auto* symbol=reinterpret_cast<SYMBOL_INFO*>(storage);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=MAX_SYM_NAME;
        DWORD64 offset=0; IMAGEHLP_MODULE64 module{}; module.SizeOfStruct=sizeof(module);
        std::cerr<<"DEBUG_STACK "<<n<<" 0x"<<std::hex<<frame.AddrPC.Offset<<std::dec;
        if(SymGetModuleInfo64(process,frame.AddrPC.Offset,&module))std::cerr<<" "<<module.ModuleName<<"+0x"<<std::hex<<frame.AddrPC.Offset-module.BaseOfImage<<std::dec;
        if(SymFromAddr(process,frame.AddrPC.Offset,&offset,symbol))std::cerr<<" "<<symbol->Name<<"+0x"<<std::hex<<offset<<std::dec;
        DWORD displacement=0;IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof(line);
        if(SymGetLineFromAddr64(process,frame.AddrPC.Offset,&displacement,&line))std::cerr<<" "<<line.FileName<<":"<<line.LineNumber;
        std::cerr<<'\n';
        if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,thread,&frame,&context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;
    }
    CloseHandle(thread);
}
int wmain(int argc,wchar_t** argv) {
    int workerIndex=1; unsigned stackDelay=0;
    if(argc>3&&std::wstring(argv[1])==L"--stacks-after-ms") {
        stackDelay=wcstoul(argv[2],nullptr,10);workerIndex=3;
        if(stackDelay<1000||stackDelay>90000){std::cerr<<"Stack timer must be 1000–90000ms\n";return 2;}
    }
    if(argc<=workerIndex){std::cerr<<"Usage: opal_debug_probe.exe [--stacks-after-ms N] worker.exe [worker args]\n";return 2;}
    std::wstring command;for(int i=workerIndex;i<argc;++i){if(i>workerIndex)command+=L' ';command+=Quote(argv[i]);}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    if(!CreateProcessW(argv[workerIndex],command.data(),nullptr,nullptr,TRUE,DEBUG_ONLY_THIS_PROCESS|CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info)){std::cerr<<"CreateProcess failed "<<GetLastError()<<'\n';return 2;}
    SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS);
    SymInitialize(info.hProcess,nullptr,FALSE);
    DWORD exitCode=2;bool running=true,stackRequested=false,stackCaptured=false;
    std::set<DWORD> threads{info.dwThreadId};auto started=std::chrono::steady_clock::now();
    while(running) {
        if(stackDelay&&!stackRequested&&std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()>=stackDelay) {
            stackRequested=DebugBreakProcess(info.hProcess)!=FALSE;
            if(!stackRequested){std::cerr<<"DEBUG_SNAPSHOT_FAILED "<<GetLastError()<<'\n';stackDelay=0;}
        }
        DEBUG_EVENT event{};if(!WaitForDebugEvent(&event,1000))continue;DWORD continuation=DBG_CONTINUE;
        switch(event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            SymLoadModuleExW(info.hProcess,event.u.CreateProcessInfo.hFile,nullptr,nullptr,reinterpret_cast<DWORD64>(event.u.CreateProcessInfo.lpBaseOfImage),0,nullptr,0);
            if(event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);break;
        case LOAD_DLL_DEBUG_EVENT:
            SymLoadModuleExW(info.hProcess,event.u.LoadDll.hFile,nullptr,nullptr,reinterpret_cast<DWORD64>(event.u.LoadDll.lpBaseOfDll),0,nullptr,0);
            if(event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);break;
        case UNLOAD_DLL_DEBUG_EVENT:SymUnloadModule64(info.hProcess,reinterpret_cast<DWORD64>(event.u.UnloadDll.lpBaseOfDll));break;
        case CREATE_THREAD_DEBUG_EVENT:
            threads.insert(event.dwThreadId);if(event.u.CreateThread.hThread)CloseHandle(event.u.CreateThread.hThread);break;
        case EXIT_THREAD_DEBUG_EVENT:threads.erase(event.dwThreadId);break;
        case EXCEPTION_DEBUG_EVENT: {
            const auto& exception=event.u.Exception;auto code=exception.ExceptionRecord.ExceptionCode;
            if(code==EXCEPTION_BREAKPOINT&&stackRequested&&!stackCaptured) {
                stackCaptured=true;std::cerr<<"DEBUG_SNAPSHOT ownedWorker="<<info.dwProcessId<<" threads="<<threads.size()<<'\n';
                for(auto thread:threads){std::cerr<<"DEBUG_THREAD "<<thread<<'\n';Stack(info.hProcess,thread);}
                std::cerr<<"DEBUG_SNAPSHOT_END\n";
            } else if(code!=EXCEPTION_BREAKPOINT) {
                continuation=DBG_EXCEPTION_NOT_HANDLED;
                if(!exception.dwFirstChance||code==0xc0000409) {
                    std::cerr<<"DEBUG_EXCEPTION code=0x"<<std::hex<<code<<std::dec<<" firstChance="<<exception.dwFirstChance<<" thread="<<event.dwThreadId;
                    if(exception.ExceptionRecord.NumberParameters)std::cerr<<" reason="<<exception.ExceptionRecord.ExceptionInformation[0];std::cerr<<'\n';
                    Stack(info.hProcess,event.dwThreadId);
                }
            }
            break; }
        case EXIT_PROCESS_DEBUG_EVENT:exitCode=event.u.ExitProcess.dwExitCode;running=false;break;
        default:break;
        }
        ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation);
    }
    SymCleanup(info.hProcess);CloseHandle(info.hThread);CloseHandle(info.hProcess);return static_cast<int>(exitCode);
}
