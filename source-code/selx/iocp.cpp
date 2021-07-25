#include <algorithm>
#include <WinSock2.h>
#include <MSWSock.h>
#include "iocp.hpp"

using namespace selx::iocp;

Server::~Server()
{
    for (Waitable osWaitable : this->osPeersWaitables)
    {
        ::closesocket(osWaitable.osSocket);
    }

    ::closesocket(this->osListenerPeerSocket);
    ::closesocket(this->osListenerSocket);
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

    Server::Socket osListenerSocket = ::WSASocket(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED
    );

    if (INVALID_SOCKET == osListenerSocket)
    {
        throw Server::Errors::OpenSocket();
    }

    sockaddr_in osAddress = {};

    osAddress.sin_family = AF_INET;
    osAddress.sin_addr.s_addr = INADDR_ANY;
    osAddress.sin_port = ::htons(port);

    if (SOCKET_ERROR == ::bind(osListenerSocket, (sockaddr*) &osAddress, sizeof(osAddress)))
    {
        throw Server::Errors::BindSocket();
    }

    if (SOCKET_ERROR == ::listen(osListenerSocket, 128))
    {
        throw Server::Errors::ListenSocket();
    }

    u_long osNonBlocking = 1;

    if (SOCKET_ERROR == ::ioctlsocket(osListenerSocket, FIONBIO, &osNonBlocking))
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
        osListenerSocket,
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
        .osSocket = osListenerSocket,
        .osOverlapped = {},
        .osBuffer = {},
    };

    if (NULL == ::CreateIoCompletionPort(
        (HANDLE) osListenerSocket,
        osIocpDescriptor,
        (ULONG_PTR) osListenerWaitable,
        0
    ))
    {
        throw Server::Errors::AttachIocp();
    }

    Server::Socket osListenerPeerSocket = ::WSASocket(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED
    );

    if (INVALID_SOCKET == osListenerPeerSocket)
    {
        throw Server::Errors::OpenSocket();
    }

    DWORD osListenerBytesTransferred = {};

    if (FALSE == osAcceptExFunction(
        osListenerWaitable->osSocket,
        osListenerPeerSocket,
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
        osListenerPeerSocket,
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

        if (this->osListenerSocket == osWaitable->osSocket)
        {
            this->accept();
        }
        else
        {
            this->read(osWaitable, osBytesTransferred);
        }
    }
}

void Server::send(Server::Socket osPeerSocket, char* buffer, std::size_t bufferLength)
{
    if (SOCKET_ERROR == ::send(osPeerSocket, buffer, bufferLength, 0))
    {
        throw Server::Errors::WriteSocket();
    }
}

void Server::kick(Server::Socket osPeerSocket)
{
    auto iterator = std::find_if(
        std::begin(this->osPeersWaitables),
        std::end(this->osPeersWaitables),
        [&osPeerDescriptor](const Waitable& waitable) {
            return osPeerDescriptor == waitable.osSocket;
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
    Server::Socket osListenerPeerSocket,
    void* osIocpDescriptor,
    void* osAcceptExFunction,
    Handlers handlers
)
{
    this->osListenerWaitable = osListenerWaitable;
    this->osListenerSocket = osListenerWaitable->osSocket;
    this->osListenerPeerSocket = osListenerPeerSocket;
    this->osIocpDescriptor = osIocpDescriptor;
    this->osAcceptExFunction = osAcceptExFunction;
    this->osPeersWaitables = {};
    this->handlers = handlers;
}

void Server::accept()
{
    if (SOCKET_ERROR == ::setsockopt(
        this->osListenerPeerSocket,
        SOL_SOCKET,
        SO_UPDATE_ACCEPT_CONTEXT,
        (char*) &this->osListenerSocket,
        sizeof(SOCKET)
    ))
    {
        throw Server::Errors::TweakSocket();
    }

    u_long osNonBlocking = 1;

    if (SOCKET_ERROR == ::ioctlsocket(this->osListenerPeerSocket, FIONBIO, &osNonBlocking))
    {
        throw Server::Errors::UnblockSocket();
    }

    this->osPeersWaitables.push_back(Waitable {
        .osSocket = this->osListenerPeerSocket,
        .osOverlapped = {},
        .osBuffer = {},
    });

    Waitable* osPeerWaitable = &this->osPeersWaitables.back();

    if (NULL == ::CreateIoCompletionPort(
        (HANDLE) this->osListenerPeerSocket,
        osIocpDescriptor,
        (ULONG_PTR) osPeerWaitable,
        0
    ))
    {
        throw Server::Errors::AttachIocp();
    }

    this->handlers.handlePeerConnection(this, this->osListenerPeerSocket);

    WSABUF osBuffer;

    osBuffer.buf = &osPeerWaitable->osBuffer[0];
    osBuffer.len = 1028;

    DWORD osFlags;

    if (SOCKET_ERROR == ::WSARecv(
        osPeerWaitable->osSocket,
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

    this->osListenerPeerSocket = ::WSASocket(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED
    );

    if (INVALID_SOCKET == osListenerPeerSocket)
    {
        throw Server::Errors::OpenSocket();
    }

    DWORD osListenerBytesTransferred = {};

    if (FALSE == ((LPFN_ACCEPTEX)(this->osAcceptExFunction))(
        this->osListenerWaitable->osSocket,
        this->osListenerPeerSocket,
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
        this->kick(osWaitable->osSocket);
    }
    else
    {
        this->handlers.handleDataArrival(
            this,
            osWaitable->osSocket,
            &osWaitable->osBuffer[0],
            osLength
        );

        WSABUF osBuffer;

        osBuffer.buf = &osWaitable->osBuffer[0];
        osBuffer.len = 1028;

        DWORD osFlags;

        if (SOCKET_ERROR == ::WSARecv(
            osWaitable->osSocket,
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
