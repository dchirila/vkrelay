// Windows implementation of vkr::process::Process.
//
// Single CreateProcessW call site. Builds a CommandLineToArgvW-compatible
// command line, a sorted UTF-16 environment block (parent env + overrides),
// an optional stdout capture pipe, and an optional Job Object so the whole
// child tree can be killed atomically.
#include "common/process/process.hpp"

#include "common/argv/command_line.hpp"
#include "common/process/line_queue.hpp"

#include <windows.h>

#include <algorithm>
#include <map>
#include <thread>

namespace vkr::process {
namespace {

std::wstring upper(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towupper(c)); });
    return s;
}

// Parses the current process environment into name -> (original-name, value),
// keyed case-insensitively, then applies overrides and emits a sorted,
// double-NUL-terminated UTF-16 block for CREATE_UNICODE_ENVIRONMENT.
std::vector<wchar_t>
build_environment_block(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> env;

    LPWCH strings = ::GetEnvironmentStringsW();
    if (strings != nullptr) {
        const wchar_t* p = strings;
        while (*p != L'\0') {
            std::wstring entry(p);
            p += entry.size() + 1;
            // Names may legitimately start with '=' (drive cwd entries), so
            // search for the separator from index 1.
            const std::size_t eq = entry.find(L'=', 1);
            if (eq == std::wstring::npos) {
                continue;
            }
            std::wstring name = entry.substr(0, eq);
            std::wstring value = entry.substr(eq + 1);
            env[upper(name)] = {name, value};
        }
        ::FreeEnvironmentStringsW(strings);
    }

    for (const auto& kv : overrides) {
        std::wstring name = argv::utf8_to_wide(kv.first);
        std::wstring value = argv::utf8_to_wide(kv.second);
        env[upper(name)] = {name, value};
    }

    std::vector<wchar_t> block;
    for (const auto& kv : env) {
        const std::wstring& name = kv.second.first;
        const std::wstring& value = kv.second.second;
        block.insert(block.end(), name.begin(), name.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0'); // terminating empty string
    return block;
}

std::string last_error(const char* what) {
    const DWORD code = ::GetLastError();
    return std::string(what) + " failed (GetLastError=" + std::to_string(code) + ")";
}

} // namespace

struct Process::Impl {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    DWORD pid = 0;
    bool new_group = false;
    bool capture = false;
    HANDLE read_end = nullptr;
    std::thread reader;
    LineQueue queue;

    bool running() const {
        return process != nullptr && ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }

    ~Impl() {
        // Terminate first so the child releases its stdout write handle and the
        // reader thread observes EOF and can be joined.
        if (running()) {
            if (job != nullptr) {
                ::TerminateJobObject(job, 1);
            } else {
                ::TerminateProcess(process, 1);
            }
        }
        if (reader.joinable()) {
            reader.join();
        }
        if (read_end != nullptr) {
            ::CloseHandle(read_end);
        }
        if (process != nullptr) {
            ::CloseHandle(process);
        }
        if (job != nullptr) {
            ::CloseHandle(job);
        }
    }
};

Process::Process() = default;
Process::~Process() = default;
Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;

Process Process::spawn(const SpawnRequest& request) {
    if (request.argv.empty()) {
        throw SpawnError("spawn requires a non-empty argv");
    }

    auto impl = std::make_unique<Impl>();
    impl->new_group = request.new_group;
    impl->capture = request.capture_stdout;

    HANDLE child_write = nullptr;
    if (request.capture_stdout) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (::CreatePipe(&impl->read_end, &child_write, &sa, 0) == 0) {
            throw SpawnError(last_error("CreatePipe"));
        }
        // The read end must not leak into the child.
        ::SetHandleInformation(impl->read_end, HANDLE_FLAG_INHERIT, 0);
    }

    if (request.new_group) {
        impl->job = ::CreateJobObjectW(nullptr, nullptr);
        if (impl->job == nullptr) {
            if (child_write != nullptr) {
                ::CloseHandle(child_write);
            }
            throw SpawnError(last_error("CreateJobObject"));
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits,
                                  sizeof(limits));
    }

    std::wstring command_line = argv::utf8_to_wide(argv::build_command_line(request.argv));
    std::vector<wchar_t> mutable_cmd(command_line.begin(), command_line.end());
    mutable_cmd.push_back(L'\0');

    std::vector<wchar_t> env_block = build_environment_block(request.env_overrides);

    std::wstring cwd_wide;
    const wchar_t* cwd_ptr = nullptr;
    if (!request.cwd.empty()) {
        cwd_wide = argv::utf8_to_wide(request.cwd);
        cwd_ptr = cwd_wide.c_str();
    }

    // Decide which handles the child inherits. We pass them through an explicit
    // STARTUPINFOEX handle list so no unrelated inheritable handle can leak in,
    // and we duplicate std handles as inheritable copies rather than flipping
    // the originals' inherit flag (which would persist and leak into later
    // spawns). The duplicates are closed right after CreateProcessW.
    const bool use_std_handles = request.capture_stdout || request.inherit_streams;
    std::vector<HANDLE> dup_handles;
    std::vector<HANDLE> inherit_list;

    auto cleanup_spawn_handles = [&]() {
        for (HANDLE h : dup_handles) {
            ::CloseHandle(h);
        }
        if (child_write != nullptr) {
            ::CloseHandle(child_write);
        }
    };

    // Duplicates a std handle as an inheritable copy. Handles placed in a
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST must be inheritable, so a DuplicateHandle
    // failure is fatal (returns nullptr + sets dup_failed) rather than silently
    // passing the original non-inheritable handle.
    bool dup_failed = false;
    auto make_inheritable_dup = [&](HANDLE src) -> HANDLE {
        if (src == nullptr || src == INVALID_HANDLE_VALUE) {
            return src; // no such stream; not added to the inherit list
        }
        HANDLE dup = nullptr;
        if (::DuplicateHandle(::GetCurrentProcess(), src, ::GetCurrentProcess(), &dup, 0, TRUE,
                              DUPLICATE_SAME_ACCESS) != 0) {
            dup_handles.push_back(dup);
            return dup;
        }
        dup_failed = true;
        return nullptr;
    };

    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);
    if (use_std_handles) {
        siex.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        siex.StartupInfo.hStdInput = make_inheritable_dup(::GetStdHandle(STD_INPUT_HANDLE));
        siex.StartupInfo.hStdOutput = request.capture_stdout
                                          ? child_write
                                          : make_inheritable_dup(::GetStdHandle(STD_OUTPUT_HANDLE));
        siex.StartupInfo.hStdError = make_inheritable_dup(::GetStdHandle(STD_ERROR_HANDLE));
        if (dup_failed) {
            cleanup_spawn_handles();
            throw SpawnError(last_error("DuplicateHandle for child stdio"));
        }
        for (HANDLE h : {siex.StartupInfo.hStdInput, siex.StartupInfo.hStdOutput,
                         siex.StartupInfo.hStdError}) {
            if (h != nullptr && h != INVALID_HANDLE_VALUE &&
                std::find(inherit_list.begin(), inherit_list.end(), h) == inherit_list.end()) {
                inherit_list.push_back(h);
            }
        }
    }

    std::vector<char> attr_buffer;
    LPPROC_THREAD_ATTRIBUTE_LIST attr_list = nullptr;
    if (!inherit_list.empty()) {
        SIZE_T attr_size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        attr_buffer.resize(attr_size);
        auto* candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
        if (::InitializeProcThreadAttributeList(candidate, 1, 0, &attr_size) == 0) {
            cleanup_spawn_handles();
            throw SpawnError(last_error("InitializeProcThreadAttributeList"));
        }
        // The list is initialized from here on; it must be deleted on every exit
        // path, including the UpdateProcThreadAttribute failure branch.
        if (::UpdateProcThreadAttribute(candidate, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                        inherit_list.data(), inherit_list.size() * sizeof(HANDLE),
                                        nullptr, nullptr) == 0) {
            ::DeleteProcThreadAttributeList(candidate);
            cleanup_spawn_handles();
            throw SpawnError(last_error("UpdateProcThreadAttribute"));
        }
        attr_list = candidate;
        siex.lpAttributeList = attr_list;
    }

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (attr_list != nullptr) {
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    if (request.new_group) {
        flags |= CREATE_SUSPENDED;
    }
    if (request.capture_stdout) {
        flags |= CREATE_NO_WINDOW;
    }

    // Only inherit handles when we have installed an explicit handle list. If
    // inherit_streams was requested but no std handle was available, attr_list
    // is null and bInheritHandles stays FALSE so unrelated inheritable handles
    // cannot leak into the child.
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr,
                                     /*bInheritHandles=*/attr_list != nullptr ? TRUE : FALSE, flags,
                                     env_block.data(), cwd_ptr, &siex.StartupInfo, &pi);

    // The duplicated std handles and the attribute list are no longer needed in
    // this process (the child holds its own inherited copies).
    cleanup_spawn_handles();
    if (attr_list != nullptr) {
        ::DeleteProcThreadAttributeList(attr_list);
    }
    if (ok == 0) {
        throw SpawnError(last_error("CreateProcessW"));
    }

    impl->process = pi.hProcess;
    impl->pid = pi.dwProcessId;

    if (request.new_group) {
        if (::AssignProcessToJobObject(impl->job, pi.hProcess) == 0) {
            // The child is still suspended and is NOT a member of the job, so
            // the destructor's TerminateJobObject would not reach it. Kill it
            // directly (TerminateProcess works on a suspended process) before
            // throwing, so a worker can never be orphaned by this failure.
            const std::string err = last_error("AssignProcessToJobObject");
            ::TerminateProcess(pi.hProcess, 1);
            ::WaitForSingleObject(pi.hProcess, 2000);
            ::CloseHandle(pi.hThread);
            throw SpawnError(err);
        }
        ::ResumeThread(pi.hThread);
    }
    ::CloseHandle(pi.hThread);

    if (request.capture_stdout) {
        HANDLE read_end = impl->read_end;
        LineQueue* queue = &impl->queue;
        impl->reader = std::thread([read_end, queue]() {
            char buf[4096];
            for (;;) {
                DWORD read_bytes = 0;
                const BOOL read_ok = ::ReadFile(read_end, buf, sizeof(buf), &read_bytes, nullptr);
                if (read_ok == 0 || read_bytes == 0) {
                    break;
                }
                queue->push_bytes(buf, read_bytes);
            }
            queue->mark_eof();
        });
    }

    Process result;
    result.impl_ = std::move(impl);
    return result;
}

bool Process::valid() const {
    return impl_ != nullptr && impl_->process != nullptr;
}
long long Process::pid() const {
    return impl_ ? static_cast<long long>(impl_->pid) : 0;
}

bool Process::read_stdout_line(int timeout_ms, std::string& line) {
    if (!impl_ || !impl_->capture) {
        return false;
    }
    return impl_->queue.pop_line(timeout_ms, line);
}

std::string Process::read_stdout_to_end() {
    if (!impl_ || !impl_->capture) {
        return std::string();
    }
    return impl_->queue.drain_to_end();
}

bool Process::wait(int timeout_ms, int& exit_code) {
    if (!impl_ || impl_->process == nullptr) {
        return false;
    }
    const DWORD timeout = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    if (::WaitForSingleObject(impl_->process, timeout) != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    ::GetExitCodeProcess(impl_->process, &code);
    exit_code = static_cast<int>(code);
    return true;
}

void Process::terminate() {
    if (!impl_) {
        return;
    }
    if (impl_->job != nullptr) {
        ::TerminateJobObject(impl_->job, 1);
    } else if (impl_->process != nullptr) {
        ::TerminateProcess(impl_->process, 1);
    }
}

void Process::terminate_graceful(int /*grace_ms*/) {
    // Windows has no clean SIGTERM equivalent for an arbitrary child, so there is NO graceful
    // phase: this DEGRADES to the hard Job-Object / process terminate (documented in process.hpp).
    // The POSIX build does the real SIGTERM -> grace -> SIGKILL escalation; the app-run spawn that
    // needs it is Linux-only.
    terminate();
}

} // namespace vkr::process
