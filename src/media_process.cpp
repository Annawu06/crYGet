#include "media_process.hpp"
#include "diagnostics.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace cryget {
namespace {
bool executable(const std::filesystem::path& path) {
    std::error_code error;
    if(!std::filesystem::is_regular_file(path,error))return false;
#ifdef _WIN32
    return true;
#else
    return access(path.c_str(),X_OK)==0;
#endif
}
#ifdef _WIN32
std::wstring environment(const wchar_t* name) {
    DWORD size=GetEnvironmentVariableW(name,nullptr,0);if(!size)return {};
    std::wstring out(size,L'\0');DWORD written=GetEnvironmentVariableW(name,out.data(),size);
    if(!written||written>=size)return {};out.resize(written);return out;
}
std::wstring quote(const std::wstring& arg) {
    std::wstring out=L"\"";size_t slashes=0;
    for(wchar_t c:arg){
        if(c==L'\\'){++slashes;continue;}
        if(c==L'"')out.append(slashes*2+1,L'\\');else out.append(slashes,L'\\');
        slashes=0;out+=c;
    }
    out.append(slashes*2,L'\\');return out+L"\"";
}
#endif
}
std::filesystem::path find_ffmpeg() {
#ifdef _WIN32
    auto override_path=environment(L"CRYGET_FFMPEG");
    if(!override_path.empty())return executable(override_path)?std::filesystem::absolute(override_path):std::filesystem::path{};
    std::wstring module(32768,L'\0');DWORD n=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    if(n&&n<module.size()) {module.resize(n);auto beside=std::filesystem::path(module).parent_path()/L"ffmpeg.exe";if(executable(beside))return beside;}
    const auto paths=environment(L"PATH");const wchar_t separator=L';';const auto name=L"ffmpeg.exe";
#else
    if(const char* override_path=std::getenv("CRYGET_FFMPEG");override_path&&*override_path)
        return executable(override_path)?std::filesystem::absolute(override_path):std::filesystem::path{};
    const char* raw=std::getenv("PATH");const std::string paths=raw?raw:"";const char separator=':';const auto name="ffmpeg";
#endif
    size_t start=0;
    while(start<paths.size()) {
        auto end=paths.find(separator,start);if(end==decltype(paths)::npos)end=paths.size();
        if(end>start) {
            auto candidate=std::filesystem::path(paths.substr(start,end-start))/name;
            if(executable(candidate))return std::filesystem::absolute(candidate);
        }
        start=end+1;
    }
    return {};
}

void merge_media(const std::filesystem::path& video,const std::filesystem::path& audio,
                 const std::filesystem::path& output,const std::atomic<bool>& canceled) {
    if(canceled.load())throw std::runtime_error("Download canceled");
    auto program=find_ffmpeg();if(program.empty())throw std::runtime_error("FFmpeg is needed to combine this video's audio and picture");
    auto diagnostic=output.parent_path()/"ffmpeg.log";
    log_event("merge.start","ffmpeg="+program.u8string()+" output="+output.u8string());
    int exit_code=-1;
#ifdef _WIN32
    std::vector<std::wstring> args={program.native(),L"-nostdin",L"-hide_banner",L"-loglevel",L"error",L"-n",L"-i",video.native(),L"-i",audio.native(),L"-map",L"0:v:0",L"-map",L"1:a:0",L"-c",L"copy",L"-movflags",L"+faststart",L"-f",L"mp4",output.native()};
    std::wstring command;for(const auto& arg:args){if(!command.empty())command+=L' ';command+=quote(arg);}
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    HANDLE log_handle=CreateFileW(diagnostic.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    if(log_handle==INVALID_HANDLE_VALUE||input==INVALID_HANDLE_VALUE){if(log_handle!=INVALID_HANDLE_VALUE)CloseHandle(log_handle);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);throw std::runtime_error("Cannot create FFmpeg diagnostic files");}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdInput=input;startup.hStdOutput=log_handle;startup.hStdError=log_handle;
    PROCESS_INFORMATION process{};
    const BOOL created=CreateProcessW(program.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process);
    const DWORD spawn_error=GetLastError();CloseHandle(log_handle);CloseHandle(input);
    if(!created){log_event("merge.spawn_error","win32="+std::to_string(spawn_error));throw std::runtime_error("Cannot start FFmpeg (Windows error "+std::to_string(spawn_error)+")");}
    CloseHandle(process.hThread);
    while(WaitForSingleObject(process.hProcess,50)==WAIT_TIMEOUT)if(canceled.load())TerminateProcess(process.hProcess,1);
    DWORD status=1;GetExitCodeProcess(process.hProcess,&status);exit_code=static_cast<int>(status);CloseHandle(process.hProcess);
#else
    std::vector<std::string> args={program.string(),"-nostdin","-hide_banner","-loglevel","error","-n","-i",video.string(),"-i",audio.string(),"-map","0:v:0","-map","1:a:0","-c","copy","-movflags","+faststart","-f","mp4",output.string()};
    std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int rc=posix_spawn_file_actions_init(&actions);
    if(rc)throw std::runtime_error("Cannot prepare FFmpeg process: "+std::string(std::strerror(rc)));
    rc=posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    if(!rc)rc=posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,diagnostic.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,STDERR_FILENO,STDOUT_FILENO);
    pid_t pid=-1;
    if(!rc)rc=posix_spawn(&pid,program.c_str(),&actions,nullptr,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions);
    if(rc){log_event("merge.spawn_error","errno="+std::to_string(rc)+" "+std::strerror(rc));throw std::runtime_error("Cannot start FFmpeg: "+std::string(std::strerror(rc))+"; check the executable and its system libraries");}
    bool terminated=false;auto terminate_at=std::chrono::steady_clock::time_point{};
    for(;;) {
        int status=0;const pid_t result=waitpid(pid,&status,WNOHANG);
        if(result==pid){exit_code=WIFEXITED(status)?WEXITSTATUS(status):128+(WIFSIGNALED(status)?WTERMSIG(status):0);break;}
        if(result<0&&errno!=EINTR)throw std::runtime_error("Cannot wait for FFmpeg");
        if(canceled.load()) {
            if(!terminated){kill(pid,SIGTERM);terminated=true;terminate_at=std::chrono::steady_clock::now();}
            else if(std::chrono::steady_clock::now()-terminate_at>std::chrono::milliseconds(500))kill(pid,SIGKILL);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
    std::ifstream log(diagnostic,std::ios::binary);std::string detail(8192,'\0');log.read(detail.data(),detail.size());detail.resize(static_cast<size_t>(log.gcount()));
    if(!detail.empty())log_event("merge.ffmpeg",detail);
    log_event("merge.exit","code="+std::to_string(exit_code));
    if(canceled.load())throw std::runtime_error("Download canceled");
    if(exit_code!=0)throw std::runtime_error("FFmpeg could not combine the video and audio; see the diagnostic log");
}
} // namespace cryget
