#ifndef SELX_IOCP_HPP
#define SELX_IOCP_HPP

#include <cstdint>
#include <functional>
#include <list>
#include <winsock2.h>

namespace selx::iocp {

    class Server {

        public:

            using Socket = unsigned int;

            struct Handlers {
                std::function<void(Server*, Socket)>                      	handlePeerConnection;
                std::function<void(Server*, Socket)>                      	handlePeerDisconnection;
                std::function<void(Server*, Socket, char*, std::size_t)>	handleDataArrival;
            };

            class Errors {

                public:

                    class OpenAPI : std::exception {};
                    class CloseAPI : std::exception {};

                    class OpenSocket : std::exception {};
                    class BindSocket : std::exception {};
                    class ListenSocket : std::exception {};
                    class UnblockSocket : std::exception {};
                    class TweakSocket : std::exception {};
                    class AcceptSocket : std::exception {};
                    class ReadSocket : std::exception {};
                    class WriteSocket : std::exception {};
                    class CloseSocket : std::exception {};

                    class OpenIocp : std::exception {};
                    class LoadIocp : std::exception {};
                    class AttachIocp : std::exception {};
                    class WaitIocp : std::exception {};

                    Errors() = delete;
                    ~Errors() = delete;

            };

            Server() = delete;
            Server(const Server& other) = delete;
            Server(Server&& other) = default;

            Server& operator=(const Server& other) = delete;
            Server& operator=(Server&& other) = default;

            ~Server();

            Server static listen(std::uint16_t port, Handlers handlers);
            void poll();
            void send(Socket osPeerSocket, char* buffer, std::size_t bufferLength);
            void kick(Socket osPeerSocket);

        private:

            struct Waitable {
                Socket        		osSocket;
                OVERLAPPED          osOverlapped;
                char                osBuffer[1028];
            };

            Waitable*               osListenerWaitable;
            Socket            		osListenerSocket;
            Socket            		osListenerPeerSocket;
            void*                   osIocpDescriptor;
            void*                   osAcceptExFunction;
            std::list<Waitable>     osPeersWaitables;
            Handlers                handlers;

            Server(
                Waitable* osListenerWaitable,
                Socket osListenerPeerSocket,
                void* osIocpDescriptor,
                void* osAcceptExFunction,
                Handlers handlers
            );

            void accept();
            void read(Waitable* osWaitable, std::uint32_t osLength);

    };

}

#endif // SELX_IOCP_HPP
