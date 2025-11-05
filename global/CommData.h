#ifndef COMM_DATA_H
#define COMM_DATA_H

#include <string.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif
#include <signal.h>
#include <setjmp.h>
#ifndef _WIN32
#include <netdb.h>
#endif
#ifndef _WIN32
#include <netinet/in.h>
#endif
#ifndef _WIN32
#include <netinet/tcp.h>
#endif
#ifndef _WIN32
#include <sys/ioctl.h>
#endif
#ifndef _WIN32
#include <arpa/inet.h>
#endif

#include "global.h"
#include "shm_type.h"
#include "msg_type.h"

// Network over TCP/IP  - Basic Function
#define  MAX_HOST_NAME_LENGTH    (64)
#define  MAX_HANDLE_INDEX		(64)
#define  CONNECT	1
#define  ACCEPT		2

#define   SOH   0x01
#define   STX   0x02
#define   ETX   0x03
#define   EOT   0x04
#define   ENQ   0x05
#define   ACK   0x06
#define   NAK   0x15

#define   WAIT     (-1)
#define   NO_WAIT  (0)

/* Comm Handle Type */
typedef int THANDLEINDEX;

/* Comm Open d Type */
typedef struct _CommInfo_  {
	   BOOL     bOpen;        /* TRUE:Open()ǻ SOCKETFALSE:Close()ǻ */
	   int      nType;        /* Open Type (CONNECT, ACCEPT)                     */
	   char     szHostName[MAX_HOST_NAME_LENGTH]; /* Remote Host Name            */
	   short    nPort;        /* Port No                                         */
	   BOOL     bEstablished; /* Connect Accept              */
	   int      svr_sock_fd;  /* ACCEPT type listen socket fd                */
	   int      sock_fd;      /* socket fd                                       */
	   int      nSendBuf;     /* Buffer (Byte)                              */
	   int      nRecvBuf;     /* Buffer (Byte)                              */
	   BOOL     bDisplay;     /* Vervose Mode                                    */
} TCOMMINFO;

/* Error Code */
enum {
	INVALID_OPEN_TYPE,
	INVALID_COMM_HANDLE,
	COMM_HANDLE_CLOSED,
	COMM_INFO_TABLE_FULL,
	CONNECT_FAIL,
	ACCEPT_FAIL,
	SETSENDBUF_FAIL,
	SETRECVBUF_FAIL,
	READ_FAIL,
	WRITE_FAIL,
	DISCONNECTED,
	TIME_OUT,
	READ_INVALID_CODE,
	CRC_NOT_MATCHING,
	SEQUENCE_ERROR,
	SIZE_ERROR,
	STX_ERROR,
	ETX_ERROR,
	OPCODE_ERROR,
	NREAD_LESS_THAN_NLEFT
};

int      get_elapsed_time();

BOOL     ReadnStream(THANDLEINDEX HandleIndex, char *Buf, int nleft, int timeout, int *actl_nread);
BOOL     RecvBuf(THANDLEINDEX HandleIndex, char *szRecvBuf, int nRecvBufLen, int *nActlRecvLen, int nWaitSec);
int      GetRecvBuf(THANDLEINDEX HandleIndex);
BOOL     SetRecvBuf(THANDLEINDEX HandleIndex, int nBufLen);
BOOL     RecvBufClear(THANDLEINDEX HandleIndex);

BOOL 	AcceptCheck( THANDLEINDEX DHandleIndex,  THANDLEINDEX HandleIndex, int nTimeOut);
BOOL     WritenStream(THANDLEINDEX HandleIndex, char *p, int nleft);
BOOL     SendBuf(THANDLEINDEX HandleIndex, char *szSendBuf, int nSendBufLen);
int      GetSendBuf(THANDLEINDEX HandleIndex);
BOOL     SetSendBuf(THANDLEINDEX HandleIndex, int nBufLen);
BOOL 	sendBuffer(char *buf,int nLength);

int      GetEmptyCommSlot();


THANDLEINDEX CommOpen(int nType, char *szHostName, short nPort);
THANDLEINDEX CommInit(int nType, char *szHostName, short nPort, BOOL bDisplay);
THANDLEINDEX CommServerOpen(short nPort);
THANDLEINDEX CommClientOpen(char *szHostName, short nPort);

BOOL     CommConnect(THANDLEINDEX HandleIndex, int nTimeOut);
BOOL     CommEstablish(THANDLEINDEX HandleIndex, int nTimeOut);
BOOL     IsEstablished(THANDLEINDEX HandleIndex);

BOOL     CommAccept(THANDLEINDEX HandleIndex, int nTimeOut);
BOOL     HandleIndexCheck(THANDLEINDEX HandleIndex);

int      GetLastCommError();
void     SetLastCommError(int nErrCode);
void     PrintLastCommError();

BOOL     CommDisconnect(THANDLEINDEX HandleIndex);
BOOL     CommClose(THANDLEINDEX HandleIndex);

// UDP Function
int fnCreateUDP (short nPortID);
int fnClose (int s);
int fnSendDgram (int s, unsigned long lIpAddr, char cBroadcast,short nPortID, char * lpData, int nDataLength);
int fnRecvDgram (int s, char * lpData, int nDataLength, unsigned long * plIp);
bool  fnSetBuffer (int s, int nSendBuf, int nRecvBuf);
bool fnSetBlock (int s, bool bBlock);

#endif /* #ifdef COMM_DATA */