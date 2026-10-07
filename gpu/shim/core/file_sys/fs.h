// bbcoop: the slice of shadPS4's descriptor table that Net uses (sockets, epolls, resolvers).
// bbport's files live in the C runtime (runtime_file.c); these ids are only ever passed back to
// sceNet* calls, and start at 0x1000 so they never read like a runtime file descriptor.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "common/types.h"

namespace Libraries::Net {
struct Socket;
struct Epoll;
struct Resolver;
} // namespace Libraries::Net

namespace Core::FileSys {

enum class FileType { Regular, Directory, Device, Socket, Epoll, Resolver, Equeue };

struct File {
    std::atomic_bool is_opened{};
    std::atomic<FileType> type{FileType::Regular};
    std::string m_guest_name;
    std::mutex m_mutex;
    std::shared_ptr<Libraries::Net::Socket> socket;
    std::shared_ptr<Libraries::Net::Epoll> epoll;
    std::shared_ptr<Libraries::Net::Resolver> resolver;
};

class HandleTable {
public:
    static constexpr int Base = 0x1000;

    int CreateHandle() {
        std::scoped_lock lock{m_mutex};
        for (size_t i = 0; i < m_files.size(); ++i) {
            if (!m_files[i]) {
                m_files[i] = std::make_unique<File>();
                return Base + static_cast<int>(i);
            }
        }
        m_files.push_back(std::make_unique<File>());
        return Base + static_cast<int>(m_files.size() - 1);
    }
    void DeleteHandle(int d) {
        std::scoped_lock lock{m_mutex};
        if (Valid(d)) m_files[d - Base].reset();
    }
    File* GetFile(int d) {
        std::scoped_lock lock{m_mutex};
        return Valid(d) ? m_files[d - Base].get() : nullptr;
    }
    File* GetSocket(int d) { return Typed(d, FileType::Socket); }
    File* GetEpoll(int d) { return Typed(d, FileType::Epoll); }
    File* GetResolver(int d) { return Typed(d, FileType::Resolver); }

private:
    bool Valid(int d) const { return d >= Base && size_t(d - Base) < m_files.size() && m_files[d - Base]; }
    File* Typed(int d, FileType type) {
        File* file = GetFile(d);
        return file && file->type == type ? file : nullptr;
    }
    std::vector<std::unique_ptr<File>> m_files;
    std::mutex m_mutex;
};

} // namespace Core::FileSys
