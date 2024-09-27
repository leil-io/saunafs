/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <inttypes.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

#ifdef _WIN32
#define TCPEAGAIN WSAEWOULDBLOCK
#define TCPEINPROGRESS WSAEWOULDBLOCK
#define TCPENOTSUP WSAEOPNOTSUPP
#define TCPEINVAL WSAEINVAL
#define TCPETIMEDOUT WSAETIMEDOUT
#else
#define TCPEAGAIN EAGAIN
#define TCPEINPROGRESS EINPROGRESS
#define TCPENOTSUP ENOTSUP
#define TCPEINVAL EINVAL
#define TCPETIMEDOUT ETIMEDOUT
#define TCPNORESPONSE EIO
#endif

#include <vector>

/* ----------------- SOCKET--------------- */

int socketinit();
int socketrelease();

/* ----------------- TCP ----------------- */

int tcpgetlasterror();
void tcpsetlasterror(int err);
int tcpsocket(void);
int tcpresolve(const char *hostname, const char *service, uint32_t *ip, uint16_t *port,
		int passiveflag);
int tcpnonblock(int sock);
int tcpsetacceptfilter(int sock);
int tcpreuseaddr(int sock);
int tcpnodelay(int sock);
int tcpaccfhttp(int sock);
int tcpaccfdata(int sock);
int tcpnumbind(int sock, uint32_t ip, uint16_t port);
int tcpstrbind(int sock, const char *hostname, const char *service);
int tcpnumconnect(int sock, uint32_t ip, uint16_t port);
int tcpnumtoconnect(int sock, uint32_t ip, uint16_t port, uint32_t msecto);
int tcpstrconnect(int sock, const char *hostname, const char *service);
int tcpstrtoconnect(int sock, const char *hostname, const char *service, uint32_t msecto);
int tcpgetstatus(int sock);
int tcpnumlisten(int sock, uint32_t ip, uint16_t port, uint16_t queue);
int tcpstrlisten(int sock, const char *hostname, const char *service, uint16_t queue);
int tcpaccept(int lsock);
int tcpgetpeer(int sock, uint32_t *ip, uint16_t *port);
int tcpgetmyaddr(int sock, uint32_t *ip, uint16_t *port);
int tcpclose(int sock);
int tcptopoll(int sock, int events, int msecto);
int tcppoll(pollfd &pfd, int msecto);
int tcppoll(std::vector<pollfd> &pfd, int msecto);
int32_t tcprecv(int sock, void *buff, uint32_t len, int flags = 0);
int32_t tcpsend(int sock, const void *buff, uint32_t len, int flags = 0);
int32_t tcptoread(int sock, void *buff, uint32_t leng, int msecto);
int32_t tcptowrite(int sock, const void *buff, uint32_t leng, int msecto);
int tcptoaccept(int sock, uint32_t msecto);

/* ----------------- UDP ----------------- */

int udpsocket(void);
int udpresolve(const char *hostname, const char *service, uint32_t *ip, uint16_t *port,
		int passiveflag);
int udpnonblock(int sock);
int udpnumlisten(int sock, uint32_t ip, uint16_t port);
int udpstrlisten(int sock, const char *hostname, const char *service);
int udpwrite(int sock, uint32_t ip, uint16_t port, const void *buff, uint16_t leng);
int udpread(int sock, uint32_t *ip, uint16_t *port, void *buff, uint16_t leng);
int udpclose(int sock);
