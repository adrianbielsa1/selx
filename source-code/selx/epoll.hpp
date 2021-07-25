#ifndef SELX_EPOLL_HPP
#define SELX_EPOLL_HPP

#include <cstdint>
#include <functional>
#include <vector>

namespace selx::epoll {

    class Server {

        public:

            using Socket = int;

            struct Handlers {
                std::function<void(Server*, Socket)>                       	handlePeerConnection;
                std::function<void(Server*, Socket)>                       	handlePeerDisconnection;
                std::function<void(Server*, Socket, char*, std::size_t)>	handleDataArrival;
            };

            class Errors {
                
                public:

                    class OpenSocket : std::exception {};
                    class BindSocket : std::exception {};
                    class ListenSocket : std::exception {};
                    class UnblockSocket : std::exception {};
                    class AcceptSocket : std::exception {};
                    class ReadSocket : std::exception {};
                    class WriteSocket : std::exception {};
                    class CloseSocket : std::exception {};

                    class OpenEpoll : std::exception {};
                    class AttachEpoll : std::exception {};
                    class DetachEpoll : std::exception {};
                    class WaitEpoll : std::exception {};

                    class BrokenListener : std::exception {};
                    class BrokenPeer : std::exception {};

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

            Socket             	osListenerSocket;
            int                 osEpollDescriptor;
            std::vector<Socket>	osPeersSockets;
            Handlers            handlers;

            Server(
                Socket osListenerSocket,
                int osEpollDescriptor,
                Handlers handlers
            );

            void accept();
            void read(Socket osPeerSocket);

    };

}

#endif // SELX_EPOLL_SERVER_HPP
