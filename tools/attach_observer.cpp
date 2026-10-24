// Loads this project's diagnostic DLL into the explicitly selected game only.
// Does not write any disk file or game executable; no save operations.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include "dominus/runtime_api.h"
#include <string>
#include <iostream>
#include <stdexcept>
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(std::string(text)+" Win32="+std::to_string(GetLastError()));}
struct H{HANDLE h=nullptr;~H(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
std::uintptr_t module(DWORD pid,const std::wstring& path,bool full){
    H snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid)};require(snapshot.h!=INVALID_HANDLE_VALUE,"Module inventory");MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Module32FirstW(snapshot.h,&entry))do{if(!_wcsicmp(full?entry.szExePath:entry.szModule,path.c_str()))return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);}while(Module32NextW(snapshot.h,&entry));return 0;
}
DWORD call(HANDLE process,std::uintptr_t address,void* argument){H thread{CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(address),argument,0,nullptr)};require(thread.h!=nullptr,"Start owned diagnostic entry");require(WaitForSingleObject(thread.h,20000)==WAIT_OBJECT_0,"Diagnostic entry timeout");DWORD result=0;require(GetExitCodeThread(thread.h,&result),"Diagnostic result");return result;}
int wmain(int argc,wchar_t** argv){try{
    require(argc==3||argc==4,"Usage: attach_observer.exe gamePID absolute-built-dll [status]");DWORD pid=std::stoul(argv[1]);std::wstring path=argv[2];require(path.size()>4&&path[1]==L':',"Absolute DLL path");const bool status=argc==4&&std::wstring(argv[3])==L"status";require(argc==3||status,"Unknown command");
    H process{OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid)};require(process.h!=nullptr,"Open authorized game");wchar_t image[32768]{};DWORD count=32768;require(QueryFullProcessImageNameW(process.h,0,image,&count),"Target identity");
    require(!_wcsicmp(image,L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Castlevania Dominus Collection\\game.exe"),"Refuse other target");
    auto local=LoadLibraryExW(path.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);require(local!=nullptr,"Read owned DLL exports");auto entry=GetProcAddress(local,status?"DominusGetStatus":"DominusAttachObserver");require(entry!=nullptr,"Owned DLL entry missing");auto offset=reinterpret_cast<std::uintptr_t>(entry)-reinterpret_cast<std::uintptr_t>(local);FreeLibrary(local);
    auto remote=module(pid,path,true);
    if(!remote){require(!status,"Status requires already loaded observer");
        auto load=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");HMODULE owner=nullptr;require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(load),&owner),"System loader owner");wchar_t ownerPath[MAX_PATH]{};GetModuleFileNameW(owner,ownerPath,MAX_PATH);auto ownerName=std::wstring(ownerPath);ownerName=ownerName.substr(ownerName.find_last_of(L"\\/")+1);auto targetOwner=module(pid,ownerName,false);require(targetOwner!=0,"Remote system loader owner");
        auto address=targetOwner+reinterpret_cast<std::uintptr_t>(load)-reinterpret_cast<std::uintptr_t>(owner);const auto bytes=(path.size()+1)*sizeof(wchar_t);auto buffer=VirtualAllocEx(process.h,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);require(buffer!=nullptr,"Diagnostic path allocation");SIZE_T written=0;require(WriteProcessMemory(process.h,buffer,path.c_str(),bytes,&written)&&written==bytes,"Write only diagnostic path");call(process.h,address,buffer);VirtualFreeEx(process.h,buffer,0,MEM_RELEASE);remote=module(pid,path,true);require(remote!=0,"Owned DLL loaded");
    }
    if(status){DominusStatus value{};value.size=sizeof(value);auto buffer=VirtualAllocEx(process.h,nullptr,sizeof(value),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);require(buffer!=nullptr,"Status allocation");SIZE_T transferred=0;require(WriteProcessMemory(process.h,buffer,&value,sizeof(value),&transferred)&&transferred==sizeof(value),"Status request");auto result=call(process.h,remote+offset,buffer);require(ReadProcessMemory(process.h,buffer,&value,sizeof(value),&transferred)&&transferred==sizeof(value),"Read owned status");VirtualFreeEx(process.h,buffer,0,MEM_RELEASE);std::cout<<"status result="<<result<<" supported="<<value.supported_build<<" observer="<<value.observer_enabled<<" presents="<<value.presents<<" compositions="<<value.compositions<<" fallbacks="<<value.fallbacks<<" resizes="<<value.resizes<<"\n";return result==1?0:2;}
    auto result=call(process.h,remote+offset,nullptr);std::cout<<"DominusAttachObserver result="<<result<<"\n";return result==1?0:2;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
