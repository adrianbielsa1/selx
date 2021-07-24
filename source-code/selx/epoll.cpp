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
    for (int osPeerDescriptor : this->osPeersDescriptors)
    {
        ::epoll_ctl(this->osEpollDescriptor, EPOLL_CTL_DEL, osPeerDescriptor, NULL);
        ::close(osPeerDescriptor);
    }

    ::epoll_ctl(this->osEpollDescriptor, EPOLL_CTL_DEL, this->osListenerDescriptor, NULL);

    ::close(this->osEpollDescriptor);
    ::close(this->osListenerDescriptor);
}

Server Server::listen(std::uint16_t port, Server::Handlers handlers)
{
    int osListenerDescriptor = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    
    if (-1 == osListenerDescriptor)
    {
        throw Server::Errors::OpenSocket();
    }

    sockaddr_in osAddress = {};

    osAddress.sin_family = AF_INET;
    osAddress.sin_addr.s_addr = INADDR_ANY;
    osAddress.sin_port = ::htons(port);

    if (-1 == ::bind(osListenerDescriptor, (sockaddr*) &osAddress, sizeof(osAddress)))
    {
        throw Server::Errors::BindSocket();
    }

    if (-1 == ::listen(osListenerDescriptor, 128))
    {
        throw Server::Errors::ListenSocket();
    }

    // TODO: Retrieve old flags?
    if (-1 == ::fcntl(osListenerDescriptor, F_SETFL, O_NONBLOCK))
    {
        throw Server::Errors::UnblockSocket();
    }

    int osEpollDescriptor = ::epoll_create1(0);

    if (-1 == osEpollDescriptor)
    {
        throw Server::Errors::OpenEpoll();
    }

    epoll_event osEpollEvent = {};

    osEpollEvent.data.fd = osListenerDescriptor;
    osEpollEvent.events = EPOLLIN | EPOLLERR;

    if (-1 == ::epoll_ctl(
        osEpollDescriptor, EPOLL_CTL_ADD, osListenerDescriptor, &osEpollEvent
    ))
    {
        throw Server::Errors::AttachEpoll();
    }

    return Server(osListenerDescriptor, osEpollDescriptor, handlers);
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
        if (osEpollEvents[i].data.fd == this->osListenerDescriptor)
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

void Server::send(int osPeerDescriptor, char* buffer, std::size_t bufferLength)
{
    if (-1 == ::send(osPeerDescriptor, (void*) buffer, bufferLength, 0))
    {
        throw Server::Errors::WriteSocket();
    }
}

void Server::kick(int osPeerDescriptor)
{
    // NOTE: This can be optimized with a map of descriptors-to-indexes,
    // reducing this search's time complexity to O(log(n)). Since it is
    // not a priority, I choose the simplest solution.
    auto iterator = std::find(
        std::begin(this->osPeersDescriptors), 
        std::end(this->osPeersDescriptors), 
        osPeerDescriptor
    );

    if (std::end(this->osPeersDescriptors) != iterator)
    {
        // Swap-and-remove, to avoid moving down all elements above the
        // iterator.
        std::iter_swap(iterator, std::end(this->osPeersDescriptors) - 1);
        this->osPeersDescriptors.pop_back();
    }

    if (-1 == ::epoll_ctl(this->osEpollDescriptor, EPOLL_CTL_DEL, osPeerDescriptor, NULL))
    {
        throw Server::Errors::DetachEpoll();
    }

    if (-1 == ::close(osPeerDescriptor))
    {
        throw Server::Errors::CloseSocket();
    }
    else
    {
        this->handlers.handlePeerDisconnection(this, osPeerDescriptor);
    }
}

Server::Server(int osListenerDescriptor, int osEpollDescriptor, Server::Handlers handlers)
{
    this->osListenerDescriptor = osListenerDescriptor;
    this->osEpollDescriptor = osEpollDescriptor;
    this->osPeersDescriptors = {};
    this->handlers = handlers;
}

void Server::accept()
{
    sockaddr osAddress = {};
    socklen_t osAddressLength = {};
    int osPeerDescriptor = ::accept(this->osListenerDescriptor, &osAddress, &osAddressLength);

    if (-1 == osPeerDescriptor)
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
        if (-1 == ::fcntl(osPeerDescriptor, F_SETFL, O_NONBLOCK))
        {
            throw Server::Errors::UnblockSocket();
        }

        epoll_event osEpollEvent = {};

        osEpollEvent.data.fd = osPeerDescriptor;
        osEpollEvent.events = EPOLLIN | EPOLLERR;

        if (-1 == ::epoll_ctl(
            this->osEpollDescriptor, EPOLL_CTL_ADD, osPeerDescriptor, &osEpollEvent
        ))
        {
            throw Server::Errors::AttachEpoll();
        }

        this->osPeersDescriptors.push_back(osPeerDescriptor);
        this->handlers.handlePeerConnection(this, osPeerDescriptor);
    }
}

void Server::read(int osPeerDescriptor)
{
    char buffer[1028];
    ssize_t bufferLength = ::read(osPeerDescriptor, &buffer[0], 1028);

    if (-1 == bufferLength)
    {
        throw Server::Errors::ReadSocket();
    }
    else if (0 == bufferLength)
    {
        this->kick(osPeerDescriptor);
    }
    else
    {
        // NOTE: Casting from signed-to-unsigned is well-defined. Since `bufferLength` is greater
        // than 0 from here on, casting it should not change the actual value (e.g. 10i8 == 10u8).
        this->handlers.handleDataArrival(this, osPeerDescriptor, &buffer[0], (std::size_t) bufferLength);
    }
}
