#include "qyhtcpclient.h"
#include <iostream>

#ifdef WIN32
#define _WIN32_WINNT _WIN32_WINNT_WIN8
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <Ws2tcpip.h>
#else
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <errno.h>
#endif

#ifdef WIN32
#pragma comment(lib,"ws2_32.lib")
#endif

QyhTcpClient::QyhTcpClient(std::string ip,int _port, QyhClientReadCallback _readcallback,QyhClientConnectCallback _connectcallback,QyhClientDisconnectCallback _disconnectcallback):
    need_reconnect(true),
    socketFd(0),
    quit(false),
    serverip(ip),
    port(_port),
    readcallback(_readcallback),
    connectcallback(_connectcallback),
    disconnectcallback(_disconnectcallback)
{
    t = std::thread([&](){

        char recvBuf[QYH_TCP_CLIENT_RECV_BUF_LEN];
        int recvLen;
        struct timeval timeInterval;
        fd_set sockFdSet;
        while(!quit)
        {
            if(!need_reconnect){
                std::chrono::milliseconds dura(1000);
                std::this_thread::sleep_for(dura);
                continue;
            }
            //创建连接
            if(!initConnect()){
                //如果创建失败，过会重新创建(5s)
                std::chrono::milliseconds dura(5000);
                std::this_thread::sleep_for(dura);
                continue;
            }

            connectcallback(myIp,myPort);

            //接收
            while(!quit && socketFd>0)
            {
                //再检查读取内容
                timeInterval.tv_sec = 0;
                timeInterval.tv_usec = 20000;
                FD_ZERO(&sockFdSet);
                FD_SET(socketFd, &sockFdSet);
                int ret = ret = select(socketFd+1, &sockFdSet, NULL, NULL, &timeInterval);
                if (ret <= 0)
                {
                    //无可读内容
                    std::chrono::milliseconds dura(20);
                    std::this_thread::sleep_for(dura);
                    continue;
                }else{
                    if (FD_ISSET(socketFd, &sockFdSet))
                    {
                        //有可读内容
                        recvLen = recv(socketFd, (char *)recvBuf, QYH_TCP_CLIENT_RECV_BUF_LEN-1, 0);
                        if (recvLen <= 0)
                        {
                            if (errno == EINTR || errno == EAGAIN)
                            {
                            }
                            else
                            {
#ifdef WIN32
                                closesocket(socketFd);
#else
                                close(socketFd);
#endif
                                socketFd = 0;

                                disconnectcallback();

                                continue;
                            }
                        }
                        else
                        {
                            recvBuf[recvLen]='\0';
                            readcallback(recvBuf,recvLen);
                        }
                    }
                }
            }
        }
    });
}

QyhTcpClient::~QyhTcpClient()
{
    quit = true;
#ifdef WIN32
    closesocket(socketFd);
#else
    close(socketFd);
#endif
    socketFd = 0;
    if(t.joinable())t.join();
}

void QyhTcpClient::resetConnect(std::string _ip, int _port)
{
    //断开链接
    disconnect();
    serverip = _ip;
    port = (_port);
    need_reconnect = true;
}

bool  QyhTcpClient::sendToServer(const char *data,int len)
{
    if(data==nullptr || len<=0 || socketFd <=0)return false;
    return len == send(socketFd,data,len,0);
}

void QyhTcpClient::disconnect()
{
    need_reconnect = false;
#ifdef WIN32
    closesocket(socketFd);
#else
    close(socketFd);
#endif
    socketFd = 0;
}

bool QyhTcpClient::initConnect()
{
#ifdef WIN32
    //加载winsocket的库
    int 		Error;
    WORD 	VersionRequested;
    WSADATA 	WsaData;

    VersionRequested = MAKEWORD(2,2);

    Error = WSAStartup(VersionRequested,&WsaData); //start WinSock2
    if(Error!=0)
    {
        printf("WSAStartup fail!\n");
        return false;
    }
    else
    {
        if(LOBYTE(WsaData.wVersion)!=2||HIBYTE(WsaData.wHighVersion)!=2)
        {
            printf("WSAStartup version fail\n");
            WSACleanup();
            return false;
        }
    }
#endif

    int ret;

    if(socketFd>0){
#ifdef WIN32
        closesocket(socketFd);
#else
        close(socketFd);
#endif
        socketFd = 0;
    }

    socketFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

#ifdef WIN32
    if (INVALID_SOCKET == socketFd)
#else
    if(socketFd < 0)
#endif
    {
        printf("Create TCP socket Err. [Line: %d] [errno: %d]\n", __LINE__, errno);
        return false;
    }

    struct sockaddr_in stServer;
    stServer.sin_family = AF_INET;
    stServer.sin_port = htons(port);
    stServer.sin_addr.s_addr=inet_addr(serverip.c_str());
#ifdef WIN32
    ret = ::connect(socketFd, (LPSOCKADDR)&stServer, sizeof(stServer));
#else
    ret = connect(socketFd, (struct sockaddr *)&stServer, sizeof(stServer));
#endif

#ifdef WIN32
    if (SOCKET_ERROR == ret)
#else
    if(ret < 0)
#endif
    {
#ifdef WIN32
        closesocket(socketFd);
#else
        close(socketFd);
#endif
        socketFd = 0;
        return false;
    }

    struct sockaddr local_addr;
    socklen_t len = sizeof(sockaddr);
    if (getsockname(socketFd, &local_addr, &len) == 0)
    {
        local_addr.sa_data;//头两位是PORT 后面4位是IP地址
        int high = local_addr.sa_data[0];
        int low = local_addr.sa_data[1];
        myPort = ((high<<8)|(low & 0xFF))& 0xFFFF;

        struct sockaddr_in* sin = (struct sockaddr_in*)(&local_addr);
        char addr_buffer[INET_ADDRSTRLEN];
        void *tmp = &(sin->sin_addr);
        if (inet_ntop(AF_INET, tmp, addr_buffer, INET_ADDRSTRLEN) != NULL)
            myIp = std::string(addr_buffer);
    }
    std::cout<<"client local ip:"<<myIp<<":"<<myPort;

    return true;
}
