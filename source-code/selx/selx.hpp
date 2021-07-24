#if defined(_WIN32) || defined(_WIN64)
    #include "iocp.hpp"

    namespace selx {
        using namespace selx::iocp;
    }
#elif defined(unix) || defined (__unix) || defined(__unix__)
    #include "epoll.hpp"

    namespace selx {
        using namespace selx::epoll;
    }
#endif
