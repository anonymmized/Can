#include "BackgroundSession.cpp"

void BackgroundSession::checkSession() {
    try {
        struct stat info{};
        if (fstat(sessionFd, &info) == -1) {
            throw std::system_error(errno, generic_category(), "Cannot inspect session file");
        }
        if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 || (info.st_mode & 077) != 0) {
            throw std::runtime_error("Unsafe session file");
        }
    } catch (...) {
        close(sessionFd);
        sessionFd = -1;
        throw;
    }
}
