#include <algorithm>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "epoll.hpp"

using namespace selx::epoll;

Server::~Server()
{
    for (Server::Socket osPeerSocket : this->osPeersSockets)
    {
        ::epoll_ctl(this->osEpollDescriptor, EPOLL_CTL_DEL, osPeerSocket, NULL);
        ::close(osPeerSocket);
    }

    ::epoll_ctl(this->osEpollDescriptor, EPOLL_CTL_DEL, this->osListenerSocket, NULL);

    ::close(this->osEpollDescriptor);
    ::close(this->osListenerSocket);
}

Server Server::listen(std::uint16_t port, Server::Handlers handlers)
{
    Server::Socket osListenerSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    
    if (-1 == osListenerSocket)
    {
        throw Server::Errors::OpenSocket();
    }

    sockaddr_in osAddress = {};

    osAddress.sin_family = AF_INET;
    osAddress.sin_addr.s_addr = INADDR_ANY;
    osAddress.sin_port = ::htons(port);

    if (-1 == ::bind(osListenerSocket, (sockaddr*) &osAddress, sizeof(osAddress)))
    {
        throw Server::Errors::BindSocket();
    }

    if (-1 == ::listen(osListenerSocket, 128))
    {
        throw Server::Errors::ListenSocket();
    }

    // TODO: Retrieve old flags?
    if (-1 == ::fcntl(osListenerSocket, F_SETFL, O_NONBLOCK))
    {
        throw Server::Errors::UnblockSocket();
    }

    int osEpollDescriptor = ::epoll_create1(0);

    if (-1 == osEpollDescriptor)
    {
        throw Server::Errors::OpenEpoll();
    }

    epoll_event osEpollEvent = {};

    osEpollEvent.data.fd = osListenerSocket;
    osEpollEvent.events = EPOLLIN | EPOLLERR;

    if (-1 == ::epoll_ctl(
        osEpollDescriptor, EPOLL_CTL_ADD, osListenerSocket, &osEpollEvent
    ))
    {
        throw Server::Errors::AttachEpoll();
    }

    return Server(osListenerSocket, osEpollDescriptor, handlers);
}

void Server::poll()
{
    std::array<epoll_event, 128> osEpollEvents;
    int osEpollEventsCount = ::epoll_wait(this->osEpollDescriptor, &osEpollEvents[0], 128, 0);

    if (-1 == osEpollEventsCount)
    {
        throw Server::Errors::WaitEpoll();
    }

    for (int i = 0; i < osEpollEventsCount; i++)
    {
        if (osEpollEvents[i].data.fd == this->osListenerSocket)
        {
            if (osEpollEvents[i].events & EPOLLERR)
            {
                throw Server::Errors::BrokenListener();
            }

            if (osEpollEvents[i].events & EPOLLIN)
            {
                this->accept();
            }
        } 
        else
        {
            if (osEpollEvents[i].events & EPOLLERR)
            {
                throw Server::Errors::BrokenPeer();
            }
            else
            {
                this->read(osEpollEvents[i].data.fd);
            }
        }
    }
}

void Server::send(Server::Socket osPeerSocket, char* buffer, std::size_t bufferLength)
{
    if (-1 == ::send(osPeerSocket, (void*) buffer, bufferLength, 0))
    {
        throw Server::Errors::WriteSocket();
    }
}

void Server::kick(Server::Socket osPeerSocket)
{
    // NOTE: This can be optimized with a map of descriptors-to-indexes,
    // reducing this search's time complexity to O(log(n)). Since it is
    // not a priority, I choose the simplest solution.
    auto iterator = std::find(
        std::begin(this->osPeersSockets), 
        std::end(this->osPeersSockets), 
        osPeerSocket
    );

    if (std::end(this->osPeersSockets) != iterator)
    {
        // Swap-and-remove, to avoid moving down all elements above the
        // iterator.
        std::iter_swap(iterator, std::end(this->osPeersSockets) - 1);
        this->osPeersSockets.pop_back();
    }

    if (-1 == ::epoll_ctl(this->osEpollDescriptor, EPOLL_CTL_DEL, osPeerSocket, NULL))
    {
        throw Server::Errors::DetachEpoll();
    }

    if (-1 == ::close(osPeerSocket))
    {
        throw Server::Errors::CloseSocket();
    }
    else
    {
        this->handlers.handlePeerDisconnection(this, osPeerSocket);
    }
}

Server::Server(
	Server::Socket osListenerSocket,
	int osEpollDescriptor,
	Server::Handlers handlers
)
{
    this->osListenerSocket = osListenerSocket;
    this->osEpollDescriptor = osEpollDescriptor;
    this->osPeersSockets = {};
    this->handlers = handlers;
}

void Server::accept()
{
    sockaddr osAddress = {};
    socklen_t osAddressLength = {};
    Server::Socket osPeerSocket = ::accept(this->osListenerSocket, &osAddress, &osAddressLength);

    if (-1 == osPeerSocket)
    {
        if ((EAGAIN == errno) || (EWOULDBLOCK == errno))
        {
            // NOTE: This should not ever happen because this method is called
            // after the listener socket is signaled by the edge polling
            // `wait` call.
            throw Server::Errors::AcceptSocket();
        }
        else
        {
            throw Server::Errors::AcceptSocket();
        }
    }
    else
    {
        // TODO: Retrieve old flags?
        if (-1 == ::fcntl(osPeerSocket, F_SETFL, O_NONBLOCK))
        {
            throw Server::Errors::UnblockSocket();
        }

        epoll_event osEpollEvent = {};

        osEpollEvent.data.fd = osPeerSocket;
        osEpollEvent.events = EPOLLIN | EPOLLERR;

        if (-1 == ::epoll_ctl(
            this->osEpollDescriptor, EPOLL_CTL_ADD, osPeerSocket, &osEpollEvent
        ))
        {
            throw Server::Errors::AttachEpoll();
        }

        this->osPeersSockets.push_back(osPeerSocket);
        this->handlers.handlePeerConnection(this, osPeerSocket);
    }
}

void Server::read(Server::Socket osPeerSocket)
{
    char buffer[1028];
    ssize_t bufferLength = ::read(osPeerSocket, &buffer[0], 1028);

    if (-1 == bufferLength)
    {
        throw Server::Errors::ReadSocket();
    }
    else if (0 == bufferLength)
    {
        this->kick(osPeerSocket);
    }
    else
    {
        // NOTE: Casting from signed-to-unsigned is well-defined. Since `bufferLength` is greater
        // than 0 from here on, casting it should not change the actual value (e.g. 10i8 == 10u8).
        this->handlers.handleDataArrival(
			this, osPeerSocket,
			&buffer[0],
			(std::size_t) bufferLength
		);
    }
}
