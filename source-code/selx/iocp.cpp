#include <algorithm>
#include <WinSock2.h>
#include <MSWSock.h>
#include "iocp.hpp"

using namespace selx::iocp;

Server::~Server()
{
    for (Waitable osWaitable : this->osPeersWaitables)
    {
        ::closesocket(osWaitable.osDescriptor);
    }

    ::closesocket(this->osListenerPeerDescriptor);
    ::closesocket(this->osListenerDescriptor);
    ::CloseHandle(this->osIocpDescriptor);
}

Server Server::listen(std::uint16_t port, Server::Handlers handlers)
{
    WORD osVersion = MAKEWORD(2, 2);
    WSADATA osData = {};

    if (0 != ::WSAStartup(osVersion, &osData))
    {
        throw Server::Errors::OpenAPI();
    }

    unsigned int osListenerDescriptor = ::WSASocket(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED
    );

    if (INVALID_SOCKET == osListenerDescriptor)
    {
        throw Server::Errors::OpenSocket();
    }

    sockaddr_in osAddress = {};

    osAddress.sin_family = AF_INET;
    osAddress.sin_addr.s_addr = INADDR_ANY;
    osAddress.sin_port = ::htons(port);

    if (SOCKET_ERROR == ::bind(osListenerDescriptor, (sockaddr*) &osAddress, sizeof(osAddress)))
    {
        throw Server::Errors::BindSocket();
    }

    if (SOCKET_ERROR == ::listen(osListenerDescriptor, 128))
    {
        throw Server::Errors::ListenSocket();
    }

    u_long osNonBlocking = 1;

    if (SOCKET_ERROR == ::ioctlsocket(osListenerDescriptor, FIONBIO, &osNonBlocking))
    {
        throw Server::Errors::UnblockSocket();
    }

    void* osIocpDescriptor = ::CreateIoCompletionPort(
        INVALID_HANDLE_VALUE, NULL, NULL, 0
    );

    if (NULL == osIocpDescriptor)
    {
        throw Server::Errors::OpenIocp();
    }

    LPFN_ACCEPTEX osAcceptExFunction = NULL;
    GUID osAcceptExGUID = WSAID_ACCEPTEX;
    DWORD osAcceptExBytesTransferred = {};

    // Load the AcceptEx function into memory using `WSAIoctl`.
    if (SOCKET_ERROR == ::WSAIoctl(
        osListenerDescriptor,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &osAcceptExGUID,
        sizeof(osAcceptExGUID),
        &osAcceptExFunction,
        sizeof(osAcceptExFunction),
        &osAcceptExBytesTransferred,
        NULL,
        NULL
    ))
    {
        throw Server::Errors::LoadIocp();
    }

    Waitable* osListenerWaitable = new Waitable {
        .osDescriptor = osListenerDescriptor,
        .osOverlapped = {},
        .osBuffer = {},
    };

    if (NULL == ::CreateIoCompletionPort(
        (HANDLE) osListenerDescriptor,
        osIocpDescriptor,
        (ULONG_PTR) osListenerWaitable,
        0
    ))
    {
        throw Server::Errors::AttachIocp();
    }

    unsigned int osListenerPeerDescriptor = ::WSASocket(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED
    );

    if (INVALID_SOCKET == osListenerPeerDescriptor)
    {
        throw Server::Errors::OpenSocket();
    }

    DWORD osListenerBytesTransferred = {};

    if (FALSE == osAcceptExFunction(
        osListenerWaitable->osDescriptor,
        osListenerPeerDescriptor,
        (void*) &osListenerWaitable->osBuffer[0],
        0,
        sizeof(sockaddr_in) + 16,
        sizeof(sockaddr_in) + 16,
        &osListenerBytesTransferred,
        &osListenerWaitable->osOverlapped
    ))
    {
        if (WSA_IO_PENDING != ::WSAGetLastError())
        {
            throw Server::Errors::AcceptSocket();
        }
    }

    return Server(
        osListenerWaitable,
        osListenerPeerDescriptor,
        osIocpDescriptor,
        (void*) osAcceptExFunction,
        handlers
    );
}

void Server::poll()
{
    DWORD osBytesTransferred = {};
    ULONG_PTR completionKey = {};
    OVERLAPPED* overlapped = {};

    if (FALSE == ::GetQueuedCompletionStatus(
        (HANDLE) this->osIocpDescriptor,
        &osBytesTransferred,
        &completionKey,
        &overlapped,
        0
    ))
    {
        if (WAIT_TIMEOUT != ::GetLastError())
        {
            throw Server::Errors::WaitIocp();
        }
    }
    else
    {
        Waitable* osWaitable = (Waitable*) completionKey;

        if (this->osListenerDescriptor == osWaitable->osDescriptor)
        {
            this->accept();
        }
        else
        {
            this->read(osWaitable, osBytesTransferred);
        }
    }
}

void Server::send(unsigned int osPeerDescriptor, char* buffer, std::size_t bufferLength)
{
    if (SOCKET_ERROR == ::send(osPeerDescriptor, buffer, bufferLength, 0))
    {
        throw Server::Errors::WriteSocket();
    }
}

void Server::kick(unsigned int osPeerDescriptor)
{
    auto iterator = std::find_if(
        std::begin(this->osPeersWaitables),
        std::end(this->osPeersWaitables),
        [&osPeerDescriptor](const Waitable& waitable) {
            return osPeerDescriptor == waitable.osDescriptor;
        }
    );

    if (std::end(this->osPeersWaitables) != iterator)
    {
        this->osPeersWaitables.erase(iterator);
    }

    if (SOCKET_ERROR == ::closesocket(osPeerDescriptor))
    {
        throw Server::Errors::CloseSocket();
    }

    this->handlers.handlePeerDisconnection(this, osPeerDescriptor);
}

Server::Server(
    Waitable* osListenerWaitable,
    unsigned int osListenerPeerDescriptor,
    void* osIocpDescriptor,
    void* osAcceptExFunction,
    Handlers handlers
)
{
    this->osListenerWaitable = osListenerWaitable;
    this->osListenerDescriptor = osListenerWaitable->osDescriptor;
    this->osListenerPeerDescriptor = osListenerPeerDescriptor;
    this->osIocpDescriptor = osIocpDescriptor;
    this->osAcceptExFunction = osAcceptExFunction;
    this->osPeersWaitables = {};
    this->handlers = handlers;
}

void Server::accept()
{
    if (SOCKET_ERROR == ::setsockopt(
        this->osListenerPeerDescriptor,
        SOL_SOCKET,
        SO_UPDATE_ACCEPT_CONTEXT,
        (char*) &this->osListenerDescriptor,
        sizeof(SOCKET)
    ))
    {
        throw Server::Errors::TweakSocket();
    }

    u_long osNonBlocking = 1;

    if (SOCKET_ERROR == ::ioctlsocket(this->osListenerPeerDescriptor, FIONBIO, &osNonBlocking))
    {
        throw Server::Errors::UnblockSocket();
    }

    this->osPeersWaitables.push_back(Waitable {
        .osDescriptor = this->osListenerPeerDescriptor,
        .osOverlapped = {},
        .osBuffer = {},
    });

    Waitable* osPeerWaitable = &this->osPeersWaitables.back();

    if (NULL == ::CreateIoCompletionPort(
        (HANDLE) this->osListenerPeerDescriptor,
        osIocpDescriptor,
        (ULONG_PTR) osPeerWaitable,
        0
    ))
    {
        throw Server::Errors::AttachIocp();
    }

    this->handlers.handlePeerConnection(this, this->osListenerPeerDescriptor);

    WSABUF osBuffer;

    osBuffer.buf = &osPeerWaitable->osBuffer[0];
    osBuffer.len = 1028;

    DWORD osFlags;

    if (SOCKET_ERROR == ::WSARecv(
        osPeerWaitable->osDescriptor,
        &osBuffer,
        1,
        NULL,
        &osFlags,
        &osPeerWaitable->osOverlapped,
        NULL
    ))
    {
        if (WSA_IO_PENDING != ::WSAGetLastError())
        {
            throw Server::Errors::ReadSocket();
        }
    }

    this->osListenerPeerDescriptor = ::WSASocket(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED
    );

    if (INVALID_SOCKET == osListenerPeerDescriptor)
    {
        throw Server::Errors::OpenSocket();
    }

    DWORD osListenerBytesTransferred = {};

    if (FALSE == ((LPFN_ACCEPTEX)(this->osAcceptExFunction))(
        this->osListenerWaitable->osDescriptor,
        this->osListenerPeerDescriptor,
        (void*) &this->osListenerWaitable->osBuffer[0],
        0,
        sizeof(sockaddr_in) + 16,
        sizeof(sockaddr_in) + 16,
        &osListenerBytesTransferred,
        &this->osListenerWaitable->osOverlapped
    ))
    {
        if (WSA_IO_PENDING != ::WSAGetLastError())
        {
            throw Server::Errors::AcceptSocket();
        }
    }
}

void Server::read(Waitable* osWaitable, std::uint32_t osLength)
{
    if (0 == osLength)
    {
        this->kick(osWaitable->osDescriptor);
    }
    else
    {
        this->handlers.handleDataArrival(
            this,
            osWaitable->osDescriptor,
            &osWaitable->osBuffer[0],
            osLength
        );

        WSABUF osBuffer;

        osBuffer.buf = &osWaitable->osBuffer[0];
        osBuffer.len = 1028;

        DWORD osFlags;

        if (SOCKET_ERROR == ::WSARecv(
            osWaitable->osDescriptor,
            &osBuffer,
            1,
            NULL,
            &osFlags,
            &osWaitable->osOverlapped,
            NULL
        ))
        {
            if (WSA_IO_PENDING != ::WSAGetLastError())
            {
                throw Server::Errors::ReadSocket();
            }
        }
    }
}
