#include "CommData.h"


#include <stdio.h>
#include <errno.h>
#include <string.h>
#ifdef _WIN32
   #define WIN32_LEAN_AND_MEAN
   #include <winsock2.h>
   #include <ws2tcpip.h>
  /* BSD �� Win API ġȯ: �� ���Ͽ����� ���� */
   #ifndef read
   #define read(fd,buf,len)   recv((fd),(buf),(len),0)
   #endif
   #ifndef write
   #define write(fd,buf,len)  send((fd),(buf),(len),0)
   #endif
   #ifndef ioctl
   #define ioctl(a,b,c)       ioctlsocket((a),(long)(b),(u_long*)(c))
   #endif
   #ifndef close
   #define close(fd)          closesocket((SOCKET)(fd))
   #endif
   #ifndef usleep
   #define usleep(x)          Sleep((x)/1000)
   #endif
   #ifndef sleep
   #define sleep(x)           Sleep((x)*1000)
   #endif
  /* SIGPIPE/longjmp ��ü(�����Ͽ�) */
   #include <setjmp.h>
   #include <signal.h>
   #ifndef SIGPIPE
   #define SIGPIPE 13
   #endif
   #ifndef SIG_IGN
  #define SIG_IGN  ((void (__cdecl *)(int))1)
   #endif
   #define sigjmp_buf          jmp_buf
   #define sigsetjmp(env,sv)   setjmp(env)
   #define siglongjmp(env,val) longjmp(env,val)
   static inline void msec_sleep(int sec, int msec){ Sleep(sec*1000 + msec); }
#else
   #include <unistd.h>
   #include <sys/types.h>
   #include <sys/socket.h>
   #include <sys/ioctl.h>   // ioctl, FIONBIO
   #include <netinet/in.h>
   #include <arpa/inet.h>
   #include <netdb.h>
   #include <setjmp.h>
   #include <signal.h>
   static inline void msec_sleep(int sec, int msec){ usleep(sec*1000000 + msec*1000); }
#endif

#ifndef ASSERT
   #include <assert.h>
   #define ASSERT(x) assert(x)
#endif

TCOMMINFO CommInfoVector[MAX_HANDLE_INDEX];

// Last ERROR Code
int nCommErrCode;
int		  nGlobal_SockHandle;
/*------------------------------------------------------------------------------------*/
/* Description: ���� �ֱٿ� get_elapsed_time()�� ȣ���� ���� ���� ���� ������         */


/*              �ҿ��� �ð� (��)�� ��´�.                                            */
/* return     : �ҿ��� �ð� (��)                                                      */
/*------------------------------------------------------------------------------------*/


// Read nleft size data from Socket, write to *Buf
BOOL ReadnStream(THANDLEINDEX HandleIndex, char *Buf, int nleft, int timeout, int *actl_nread)
{
   int sock_fd;
   register int i;
   register int nread = 0;
   register int rtn;
   int  elapsed = 0;
   int  arg; /* blocking, non-blocking ��ȯ�� */

   if (actl_nread != NULL) *actl_nread = 0;

   sock_fd = CommInfoVector[HandleIndex].sock_fd;

   if (timeout >= 0) {
      arg = 1; ioctl(sock_fd, FIONBIO, &arg);   /* non-blocking mode */
   }

   for(i = 0; i < nleft; ) {

      rtn = read(sock_fd, Buf + i, nleft - nread);

      if (rtn == 0) {  /* EOF */

         arg = 0; ioctl(sock_fd, FIONBIO, &arg);   /* blocking mode */
         if (nread > 0) { /* 1 Byte �̻� ������ ������ */
            if (actl_nread != NULL) *actl_nread = nread;
            return true;
         } else {
            close(CommInfoVector[HandleIndex].sock_fd);
            CommInfoVector[HandleIndex].bEstablished = false;
            nCommErrCode = DISCONNECTED;
            if (CommInfoVector[HandleIndex].bDisplay == true) {
            	printf("COM] EOF received  i=%d  nread=%d \n\n", i, nread);
            }
            return false;
         }


      } else if (rtn < 0) {  /* error */

         if (timeout >= 0) {
            if (elapsed >= timeout) { /* time out */
               arg = 0; ioctl(sock_fd, FIONBIO, &arg);   /* blocking mode */
               if (nread > 0) { /* 1 Byte �̻� ������ ������ */
                  if (actl_nread != NULL) *actl_nread = nread;
                  return true;
               } else {
                  nCommErrCode = TIME_OUT;
                  return false;
               }
            }
            msec_sleep(0, 1);
            //sleep(1);
            elapsed++;
         }

      } else { /* rtn(bytes)�� read�� ��� */

         nread += rtn;
         i += rtn;

         /* blocking�̸� non-blocking���� */
         if (timeout < 0) {
            arg = 1; ioctl(sock_fd, FIONBIO, &arg);   /* non-blocking mode */
            elapsed = 0;
            timeout = 2;
         }
      }

   }

   arg = 0; ioctl(sock_fd, FIONBIO, &arg);   /* blocking mode */

   if (actl_nread != NULL) *actl_nread = nread;
   return true;
}



#ifndef _WIN32
sigjmp_buf jmpbufPIPE;
void SIGPIPE_handler(int sig_no)
{
   siglongjmp(jmpbufPIPE, SIGPIPE);
}
#else
sigjmp_buf jmpbufPIPE;
#endif

/*------------------------------------------------------------------------------------*/
/* Description: socket stream pipe�� data�� nleft(bytes)��ŭ ����                     */
/* return     : true�̸� ����, false�̸� ����                                         */
/*------------------------------------------------------------------------------------*/
BOOL WritenStream(THANDLEINDEX HandleIndex, char *p, int nleft)
{
   int sock_fd = 0;
   register int i;
   register int nwritten = 0;
   register int rtn;
   int sigrtn;
   int  arg; /* blocking, non-blocking ��ȯ�� */


   /* Socket Stream�� Close�Ǿ��� �� SIGPIPE�� �����Ѵ� */
#ifndef _WIN32
   signal(SIGPIPE, SIGPIPE_handler);

   #endif
/* SIGPIPE_handler���� longjmp�ϵ��� �Ѵ�. */
   sigrtn = sigsetjmp(jmpbufPIPE, 1);
   if (sigrtn == SIGPIPE) {
      close(CommInfoVector[HandleIndex].sock_fd);
      CommInfoVector[HandleIndex].bEstablished = false;
      nCommErrCode = DISCONNECTED;
      if (CommInfoVector[HandleIndex].bDisplay == true) {
      	printf("COM] SIGPIPE occured  \n\n");
      }
      arg = 0; ioctl(sock_fd, FIONBIO, &arg);   /* blocking mode */
#ifndef _WIN32
      signal(SIGPIPE, SIG_IGN);
      #endif
return false;
   }

   sock_fd = CommInfoVector[HandleIndex].sock_fd;

   arg = 1; ioctl(sock_fd, FIONBIO, &arg);   /* non-blocking mode */
   arg = 0; ioctl(sock_fd, FIONBIO, &arg);   /* blocking mode */

   for(i = 0; i < nleft; ) {

      rtn = write(sock_fd, p + i, nleft - nwritten);

      if (rtn <= 0) {  /* error */
         if (CommInfoVector[HandleIndex].bDisplay == true) {
         		printf("COM] WritenStream() error  rtn=%d \n\n", rtn);
         }
         arg = 0; ioctl(sock_fd, FIONBIO, &arg);   /* blocking mode */
#ifndef _WIN32
         signal(SIGPIPE, SIG_IGN);
         #endif
nCommErrCode = WRITE_FAIL;
         return false;

      } else {
         nwritten += rtn;
         i += rtn;
      }
   }

   arg = 0; ioctl(sock_fd, FIONBIO, &arg);   /* blocking mode */
#ifndef _WIN32
   signal(SIGPIPE, SIG_IGN);
   #endif
return true;
}



/*------------------------------------------------------------------------------------*/
/* Description: Comm Open ���� Table���� Empty Slot�� ��ȯ                        */
/* return     : -1�̸� error                                                          */
/*------------------------------------------------------------------------------------*/
int GetEmptyCommSlot()
{
   int i;
   for(i = 0; i < MAX_HANDLE_INDEX; i++) {
      if (CommInfoVector[i].bOpen == false) return i;
   }

   return -1;
}



/*------------------------------------------------------------------------------------*/
/* Description: SOCKET ��� ȯ�� Open                                                    */
/* nType      : Open Type, CONNECT/ACCEPT �� �ϳ�.                                    */
/* szHostName : CONNECT�ÿ� �����û���� Server�� Host Name.                          */
/*              ACCEPT�ÿ��� ���õ�.                                                  */
/* nPort      : Port No.                                                              */
/* return     : THANDLEINDEX type�� Handle, -1�̸� Error.                                 */
/*------------------------------------------------------------------------------------*/
THANDLEINDEX CommOpen(int nType, char *szHostName, short nPort) // For Server !!!
{
   THANDLEINDEX HandleIndex;

#ifdef _WIN32
    /* 1) Winsock은 소켓 사용 전에 반드시 초기화 */
   static int _wsinit = 0;
   if(!_wsinit){ WSADATA w; WSAStartup(MAKEWORD(2,2), &w); _wsinit = 1; }
#endif

    /* 2) 타입 유효성 체크 */
   if (!(nType == CONNECT || nType == ACCEPT)) {
      nCommErrCode = INVALID_OPEN_TYPE ;
      return -1;
   }

    /* 3) 빈 슬롯 찾기 */
   HandleIndex = GetEmptyCommSlot();
   if (HandleIndex == -1) {
      nCommErrCode = COMM_INFO_TABLE_FULL;
      return -1;
   }

    /* 4) 테이블 초기화 */
   CommInfoVector[HandleIndex].bOpen        = true;
   CommInfoVector[HandleIndex].nType        = nType;
   if (szHostName != NULL)
      sprintf(CommInfoVector[HandleIndex].szHostName, "%s", szHostName);
   CommInfoVector[HandleIndex].nPort        = nPort;
   CommInfoVector[HandleIndex].bEstablished = false;
   CommInfoVector[HandleIndex].sock_fd      = -1;
   CommInfoVector[HandleIndex].bDisplay     = true;  /* ← 디버그 로그 켬 */

   if (nType == ACCEPT)
   {
      int    recv_sock_fd;
      struct sockaddr_in recv_addr;
#ifdef _WIN32
      int opt = 1;
#endif
      if((recv_sock_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            CommInfoVector[HandleIndex].bOpen = false;
            return -1;
      }
#ifdef _WIN32
        /* Windows에서 포트 재바인드 편의 */
      setsockopt(recv_sock_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
#endif
        memset((char *)&recv_addr, 0x00, sizeof(recv_addr));
      recv_addr.sin_family      = AF_INET;
      recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
      recv_addr.sin_port        = htons(CommInfoVector[HandleIndex].nPort);
        if( bind(recv_sock_fd, (struct sockaddr *)&recv_addr, sizeof(recv_addr)) < 0 ) {
            CommInfoVector[HandleIndex].bOpen = false;
            return -1;
      } else {
         CommInfoVector[HandleIndex].svr_sock_fd = recv_sock_fd; // Server Accept only
      }
      if(listen(recv_sock_fd, SOMAXCONN) == -1) {
         CommInfoVector[HandleIndex].bOpen = false;
         return -1;
      }
   } else if (nType == CONNECT)
   {
      if(!CommConnect(HandleIndex, 5)) {
         CommInfoVector[HandleIndex].bOpen = false;
         return -1;
      }
   }

#ifndef _WIN32
   signal(SIGPIPE, SIG_IGN);
#endif
   return HandleIndex;
}

/*------------------------------------------------------------------------------------*/
/* Description: SOCKET ��� ȯ�� Open*/
/* nType: Open Type, CONNECT/ACCEPT �� �ϳ�.*/
/* szHostName : CONNECT�ÿ� �����û���� Server�� Host Name.*/
/*              ACCEPT�ÿ��� ���õ�.                                                  */
/* nPort      : Port No.                                                              */
/* return     : THANDLEINDEX type�� Handle, -1�̸� Error.                                 */
/*------------------------------------------------------------------------------------*/
THANDLEINDEX CommInit(int nType, char *szHostName, short nPort, BOOL bDisplay)
{
   THANDLEINDEX HandleIndex;
   HandleIndex = CommOpen(nType, szHostName, nPort);
   if (HandleIndex != -1) CommInfoVector[HandleIndex].bDisplay = bDisplay;
   // signal(SIGPIPE, SIG_IGN); // SIGPIPE 발생 시 프로그램 종료 방지
   return HandleIndex;
}



/*------------------------------------------------------------------------------------*/
/* Description: SOCKET ��� ȯ���� Server�� Open                                         */
/* nPort      : Port No.                                                              */
/* return     : THANDLEINDEX type�� Handle, -1�̸� Error.                                 */
/*------------------------------------------------------------------------------------*/
THANDLEINDEX CommServerOpen(short nPort)
{
   return CommOpen(ACCEPT, NULL, nPort);
}



/*------------------------------------------------------------------------------------*/
/* Description: SOCKET ��� ȯ���� Client�� Open                                         */
/* szHostName : CONNECT�ÿ� �����û���� Server�� Host Name.                          */
/* nPort      : Port No.                                                              */
/* return     : THANDLEINDEX type�� Handle, -1�̸� Error.                                 */
/*------------------------------------------------------------------------------------*/
THANDLEINDEX CommClientOpen(char *szHostName, short nPort)
{
   return CommOpen(CONNECT, szHostName, nPort);
}



/*------------------------------------------------------------------------------------*/
/* Description: Server�� �����û�Ͽ� SOCKET ��� ȯ�� Open                              */
/* HandleIndex   : THANDLEINDEX type�� Handle                                                */
/* nTimeOut   : connect() �����Ҷ������� �ִ� ��� �ð� (��)                          */
/* return     : true�̸� ����, false�̸� ����                                         */
/*------------------------------------------------------------------------------------*/
BOOL CommConnect(THANDLEINDEX HandleIndex, int nTimeOut)
{
   if (HandleIndexCheck(HandleIndex) == false) return false;

    /* --- 주소 해석: IPv4/IPv6 모두 시도 --- */
   char portstr[16];
   snprintf(portstr, sizeof(portstr), "%d", (int)CommInfoVector[HandleIndex].nPort);

   struct addrinfo hints, *res = NULL, *rp = NULL;
   memset(&hints, 0, sizeof(hints));
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_family   = AF_UNSPEC;      /* v4/v6 둘 다 허용 */
   hints.ai_protocol = IPPROTO_TCP;
#ifdef AI_ADDRCONFIG
   hints.ai_flags    = AI_ADDRCONFIG;  /* 로컬에 설정된 주소 패밀리만 */
#endif

   int gerr = getaddrinfo(CommInfoVector[HandleIndex].szHostName, portstr, &hints, &res);
   if (gerr != 0) {
   #ifdef _WIN32
      printf("COM] getaddrinfo() fail: %d (host=%s, port=%s)\n", gerr,
            CommInfoVector[HandleIndex].szHostName, portstr);
   #else
      printf("COM] getaddrinfo() fail: %s (host=%s, port=%s)\n",
            gai_strerror(gerr), CommInfoVector[HandleIndex].szHostName, portstr);
   #endif
      nCommErrCode = CONNECT_FAIL;
      return false;
   }

   if (CommInfoVector[HandleIndex].bDisplay == true) {
      printf("COM] before connect() host=%s port=%s timeout=%d(sec)\n",
            CommInfoVector[HandleIndex].szHostName, portstr, nTimeOut);
   }

    /* nTimeOut: 초 단위. WAIT면 무한 재시도 */
   int elapsed_sec = 0;
   for (rp = res; rp != NULL; rp = rp->ai_next) {

      while ( (nTimeOut == WAIT) || (elapsed_sec < nTimeOut) ) {

         int s = (int)socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
         if (s < 0) {
               /* 소켓 생성 실패 → 1초 후 재시도 */
               if (CommInfoVector[HandleIndex].bDisplay)
                  printf("COM] socket() fail (family=%d)\n", rp->ai_family);
               msec_sleep(1, 0); elapsed_sec++;
               continue;
         }

         int r = connect(s, rp->ai_addr, (int)rp->ai_addrlen);
         if (r == 0) {
               CommInfoVector[HandleIndex].bEstablished = true;
               CommInfoVector[HandleIndex].sock_fd      = s;
               if (CommInfoVector[HandleIndex].bDisplay == true) {
                  printf("COM] connect() OK (family=%d)\n", rp->ai_family);
               }
               freeaddrinfo(res);
               return true;
         } else {
               /* 실패 원인 출력 후 소켓 닫고 1초 대기 */
         #ifdef _WIN32
               int werr = WSAGetLastError();
               if (CommInfoVector[HandleIndex].bDisplay == true)
                  printf("COM] connect() fail: WSAError=%d (family=%d)\n", werr, rp->ai_family);
         #else
               if (CommInfoVector[HandleIndex].bDisplay == true)
                  printf("COM] connect() fail: errno=%d (%s) (family=%d)\n",
                        errno, strerror(errno), rp->ai_family);
         #endif
               close(s);
               msec_sleep(1, 0);  /* 1초 대기 */
               elapsed_sec++;

               if (nTimeOut != WAIT && elapsed_sec >= nTimeOut)
                  break;
         }
      }

      if (nTimeOut != WAIT && elapsed_sec >= nTimeOut)
         break;
   }

   freeaddrinfo(res);
   nCommErrCode = TIME_OUT;
   return false;
}

BOOL AcceptCheck(THANDLEINDEX DummyHandleIndex, THANDLEINDEX ORGHandleIndex, int nTimeOut)
{

	int    recv_new_sock_fd, recv_sock_fd;
	struct timeval tm_val;
	fd_set ready;
	struct sockaddr_in cli_addr;
	int    cli_addr_size;
	int    rtn;

	/* Handle Check */
	// if (HandleIndexCheck(DummyHandleIndex) == false) return false;

	recv_sock_fd = CommInfoVector[ORGHandleIndex].svr_sock_fd;

	if (nTimeOut < 0) { /* forever */
		cli_addr_size = sizeof(cli_addr);
		recv_new_sock_fd = accept(recv_sock_fd, (struct sockaddr *)&cli_addr, &cli_addr_size);
		if (recv_new_sock_fd < 0) {
			msec_sleep(0, 2);
			nCommErrCode = ACCEPT_FAIL;
			return false;
		}
	}
	else {
		FD_ZERO(&ready);
		FD_SET(recv_sock_fd, &ready);
		tm_val.tv_sec  = (long)nTimeOut ;
		tm_val.tv_usec = (long)0        ;
		rtn = select(recv_sock_fd+1,
				(fd_set *)&ready, (fd_set *)0, (fd_set *)0,
				(nTimeOut < 0) ? NULL : (struct timeval *)&tm_val);
		if (rtn > 0 && FD_ISSET(recv_sock_fd, &ready)) { /* ���� */
			cli_addr_size = sizeof(cli_addr);
			recv_new_sock_fd = accept(recv_sock_fd, (struct sockaddr *)&cli_addr, &cli_addr_size);
		// if ( (recv_new_sock_fd < 0) || (recv_new_sock_fd - recv_sock_fd) ) {
			if  (recv_new_sock_fd < 0) {
				nCommErrCode = TIME_OUT;
				return false;
			}
		}
		else {
			nCommErrCode = TIME_OUT;
			return false;
		}
	}

	if (CommInfoVector[ORGHandleIndex].sock_fd > 0) {
   		close(recv_new_sock_fd);
   		return false; // JIN0713 !!! - Accept only one client socket
	}

	CommInfoVector[DummyHandleIndex].bEstablished = true;
   	CommInfoVector[DummyHandleIndex].sock_fd      = recv_new_sock_fd;
   	if (CommInfoVector[DummyHandleIndex].bDisplay == true) {
   		printf("COM] accept() Handle(%d), Socket(%d), SvrSocket(%d) OK \n",DummyHandleIndex, recv_new_sock_fd, recv_sock_fd);
   	}
   	return true;
}

/*------------------------------------------------------------------------------------*/
/* Description: Client�κ��� �����û�޾� SOCKET ��� ȯ�� Open                          */
/* HandleIndex   : THANDLEINDEX type�� Handle                                                */
/* nTimeOut   : accept() �����Ҷ������� �ִ� ��� �ð�  (��)                          */
/* return     : true�̸� ����, false�̸� ����                                         */
/*------------------------------------------------------------------------------------*/
BOOL CommAccept(THANDLEINDEX HandleIndex, int nTimeOut)
{
	int    recv_sock_fd;          /* listening socket fd                               */
	int    recv_new_sock_fd;      /* accept()���� socket fd                            */
	struct timeval tm_val;        /* recv_sock_fd�� connect()��û�� �ִ��� check�ϱ� ���� ���� */
	fd_set ready;
	struct sockaddr_in cli_addr;  /* accept()�� Client IP addrsss�� �����ϱ� ���� ���� */
	int    cli_addr_size;         /* sizeof(cli_addr)�� �����ϴ� ����                  */
/*   int    arg;    */               /* blocking, non-blocking ��ȯ��                     */
	int    rtn;

   /* Handle Check */
	if (HandleIndexCheck(HandleIndex) == false) return false;

	recv_sock_fd = CommInfoVector[HandleIndex].svr_sock_fd;

	if (CommInfoVector[HandleIndex].bDisplay == true)
		printf("COM] before accept()  nPort=%-d  nTimeOut = %d \n", CommInfoVector[HandleIndex].nPort, nTimeOut);

   /* Client�κ��� connect() ��û�� ������ accept()�� �����Ѵ� */
   if (nTimeOut < 0) { /* forever */
      cli_addr_size = sizeof(cli_addr);
      recv_new_sock_fd = accept(recv_sock_fd, (struct sockaddr *)&cli_addr, &cli_addr_size);
      if (recv_new_sock_fd < 0) {
         msec_sleep(0, 2);
         nCommErrCode = ACCEPT_FAIL;
         return false;
      }

   } else {
      FD_ZERO(&ready);
      FD_SET(recv_sock_fd, &ready);
      tm_val.tv_sec  = (long)nTimeOut ;
      tm_val.tv_usec = (long)0        ;
      rtn = select(recv_sock_fd+1,
                  (fd_set *)&ready, (fd_set *)0, (fd_set *)0,
                  (nTimeOut < 0) ? NULL : (struct timeval *)&tm_val);
      if (rtn > 0 && FD_ISSET(recv_sock_fd, &ready)) { /* ���� */
         cli_addr_size = sizeof(cli_addr);
         recv_new_sock_fd = accept(recv_sock_fd, (struct sockaddr *)&cli_addr, &cli_addr_size);
	   if ( recv_new_sock_fd < 0 )
		   printf("COM] Socket Connect Error\n");
	   else {
	  	//printf(" Welcom to 211.240.33.103");
	   }


	   nGlobal_SockHandle = recv_new_sock_fd;
        // if ( (recv_new_sock_fd < 0) || (recv_new_sock_fd - recv_sock_fd) ) {
         if  (recv_new_sock_fd < 0) {
            nCommErrCode = TIME_OUT;
            return false;
         }
      } else {
         nCommErrCode = TIME_OUT;
         return false;
      }
   }

   /* �����̸� Tsd Commpen ���� Table�� ���� */
   CommInfoVector[HandleIndex].bEstablished = true;
   CommInfoVector[HandleIndex].sock_fd      = recv_new_sock_fd;
   //if (CommInfoVector[HandleIndex].bDisplay == true) {
   		printf("COM] accept() Handle(%d), Socket(%d), SvrSocket(%d) OK \n", HandleIndex, recv_new_sock_fd, recv_sock_fd);
   // }
   return true;
}



/*------------------------------------------------------------------------------------*/
/* Description: Server/Client���� connect()/accept()�� �����Ѵ�                       */
/* HandleIndex   : THANDLEINDEX type�� Handle                                                */
/* nTimeOut   : Time Out */
/* return     : true�̸� ����, false�̸� ����                                         */
/*------------------------------------------------------------------------------------*/
BOOL CommEstablish(THANDLEINDEX HandleIndex, int nTimeOut)
{
   int bRtn;

   /* Handle Check */
   if (HandleIndexCheck(HandleIndex) == false) return false;

   if (CommInfoVector[HandleIndex].nType == CONNECT) {
      bRtn = CommConnect(HandleIndex, nTimeOut);
      if (bRtn == false) {
         nCommErrCode = CONNECT_FAIL;
         return false;
      }

   } else if (CommInfoVector[HandleIndex].nType == ACCEPT) {
      bRtn = CommAccept(HandleIndex, nTimeOut);
      if (bRtn == false) {
         nCommErrCode = ACCEPT_FAIL;
         return false;
      }

   } else {
      return false;
   }

   // �۽� Buffer�� 64KByte�� �ø���.
   bRtn = SetSendBuf(HandleIndex, 1024 * 128);
   if (bRtn == false) {
      nCommErrCode = SETSENDBUF_FAIL;
      return false;
   }

   // ���� Buffer�� 64KByte�� �ø���.
   bRtn = SetRecvBuf(HandleIndex, 1024 * 128);
   if (bRtn == false) {
      nCommErrCode = SETRECVBUF_FAIL;
      return false;
   }

   return true;
}



/*------------------------------------------------------------------------------------*/
/* Description: TCP/IP ���� ���� Check                                                */
/* HandleIndex   : THANDLEINDEX type�� Handle                                                */
/* return     : true�̸� ����� ����, false�̸� ������ ����                           */
/*------------------------------------------------------------------------------------*/
BOOL IsEstablished(THANDLEINDEX HandleIndex)
{
   /* Handle Check */
   if (HandleIndexCheck(HandleIndex) == false) return false;

   return CommInfoVector[HandleIndex].bEstablished;
}



/*------------------------------------------------------------------------------------*/
/* Description: ������ �۽� Buffer ���                                               */
/* HandleIndex   : THANDLEINDEX type�� Handle                                                */
/* return     : �۽� Buffer�� ũ��                                                    */
/*------------------------------------------------------------------------------------*/
int GetSendBuf(THANDLEINDEX HandleIndex)
{
	ASSERT( HandleIndex != -1);

	int    sendbuff;
	int    size;
	int    rtn;

	size = sizeof(int);

	rtn = getsockopt(CommInfoVector[HandleIndex].sock_fd, SOL_SOCKET, SO_SNDBUF,
	     (void *)&sendbuff, &size);
	if (rtn < 0)
	{
		printf("COM] [SOCKET] GetSendBuf() - %s\n", strerror(errno));
		return -1;
	}

	return sendbuff;
}


/*------------------------------------------------------------------------------------*/
/* Description: ������ �۽� Buffer ����                                               */
/* HandleIndex   : THANDLEINDEX type�� Handle                                                */
/* nBufLen    : �۽� Buffer�� ũ��                                                    */
/* return     : true�̸� ����, false�̸� ����                                         */
/*------------------------------------------------------------------------------------*/
BOOL SetSendBuf(THANDLEINDEX HandleIndex, int nBufLen)
{
   int sendbuff;
   int rtn;

   sendbuff = nBufLen; /* 128 KB */

   rtn = setsockopt(CommInfoVector[HandleIndex].sock_fd, SOL_SOCKET, SO_SNDBUF,
         (char *)&sendbuff, sizeof(sendbuff));
   if (rtn < 0) return false;

   CommInfoVector[HandleIndex].nSendBuf = sendbuff;
   return true;
}



/*------------------------------------------------------------------------------------*/
/* Description: ������ ���� Buffer ���                                               */
/* HandleIndex   : THANDLEINDEX type�� Handle                                                */
/* return     : ���� Buffer�� ũ��                                                    */
/*------------------------------------------------------------------------------------*/
int GetRecvBuf(THANDLEINDEX HandleIndex)
{
	ASSERT( HandleIndex != -1);

	int    recvbuff;
	int    size;
	int    rtn;

	size = sizeof(int);

	rtn = getsockopt(CommInfoVector[HandleIndex].sock_fd, SOL_SOCKET, SO_RCVBUF,
	     (void *)&recvbuff, &size);
	if (rtn < 0)
	{
		printf("COM] [SOCKET] GetRecvBuf() - %s\n", strerror(errno));
		return -1;
	}

	return recvbuff;
}



/*------------------------------------------------------------------------------------*/
/* Description: ������ ���� Buffer ����                                               */
/* HandleIndex   : THANDLEINDEX type�� Handle                                                */
/* nBufLen    : ���� Buffer�� ũ��                                                    */
/* return     : true�̸� ����, false�̸� ����                                         */
/*------------------------------------------------------------------------------------*/
BOOL SetRecvBuf(THANDLEINDEX HandleIndex, int nBufLen)
{
   int recvbuff;
   int rtn;

   recvbuff = nBufLen; /* 128 KB */

   rtn = setsockopt(CommInfoVector[HandleIndex].sock_fd, SOL_SOCKET, SO_RCVBUF,
         (char *)&recvbuff, sizeof(recvbuff));
   if (rtn < 0) return false;

   CommInfoVector[HandleIndex].nRecvBuf = recvbuff;
   return true;
}



/*------------------------------------------------------------------------------------*/
/* Description : Data �۽�                                                            */
/* HandleIndex    : THANDLEINDEX type�� Handle.                                              */
/* szSendBuf   : �۽��� Data�� ���� Pointer                                           */
/* nSendBufLen : �۽��� Data�� ũ�� (Bytes)                                           */
/* return      : true�̸� ����, false�̸� ����                                        */
/*------------------------------------------------------------------------------------*/
BOOL SendBuf(THANDLEINDEX HandleIndex, char *szSendBuf, int nSendBufLen)
{
   int bRtn;
   int	i;

   /* Handle Check */
   if (HandleIndexCheck(HandleIndex) == false) return false;

   /* Comm�� Server/Client�� ����Ǿ� �ִ��� Check */
   if (CommInfoVector[HandleIndex].bEstablished == false)
      if (CommEstablish(HandleIndex, 10) == false) return false;

/*	printf("MSG SEND:");
	for(i=0; i<nSendBufLen; i++)
		printf( "[%02X]",szSendBuf[i]&0xFF);
	printf("\n");
*/

   bRtn = WritenStream(HandleIndex, szSendBuf, nSendBufLen);
   if (bRtn == false) return false;  /* WritenStream() error */

   return true;
}

/*--------------------------------------------------------------------------------------*/
/* Description : nRecvBufLen(bytes)�� Data�� �����Ҷ����� ���                          */
/* HandleIndex    : THANDLEINDEX type�� Handle.                                                */
/* szRecvBuf   : ������ Data�� ����� ���� Pointer (OUT)                                */
/* nRecvBufLen : szRecvBuf�� ũ�� (Bytes)                                               */
/* nActlRecvLen: ���� ������ Data�� ũ�� (Bytes)                                        */
/* nWaitSec    : �����̸� ������ ���. (��)                                             */
/*             : �ִ� ���ð� ����Ŀ��� ������ Data�� nRecvBufLen���� �۾Ƶ� return.  */
/*               nRecvBufLen(byte)��ŭ ���������� ��ٷ� return.                        */
/* return      : true�̸� ����, false�̸� ����                                          */
/*--------------------------------------------------------------------------------------*/
BOOL RecvBuf(THANDLEINDEX HandleIndex, char *szRecvBuf, int nRecvBufLen, int *nActlRecvLen, int nWaitSec)
{
   int bRtn;

   /* Handle Check */
   if (HandleIndexCheck(HandleIndex) == false) return false;

   /* Comm�� Server/Client�� ����Ǿ� �ִ��� Check */
   if (CommInfoVector[HandleIndex].bEstablished == false)
      if (CommEstablish(HandleIndex, nWaitSec) == false) return false;

   /* �ҿ� �ð� Check */
   if (nWaitSec > 0) {
      if (nWaitSec <= 0) {
         nCommErrCode = TIME_OUT;
         return false;
      }
   }

   /* Data ���� */
   bRtn = ReadnStream(HandleIndex, szRecvBuf, nRecvBufLen, nWaitSec, nActlRecvLen);

   if (bRtn == false) return false;

   return true;
}



/*------------------------------------------------------------------------------------*/
/* Description: Socket ���� Buffer Clear                                              */
/* HandleIndex   : THANDLEINDEX type�� Handle.                                               */
/*------------------------------------------------------------------------------------*/
BOOL RecvBufClear(THANDLEINDEX HandleIndex)
{
   int  sock_fd;
   int  arg; /* blocking, non-blocking ��ȯ�� */
   char Temp[2];

   /* Handle Check */
   if (HandleIndexCheck(HandleIndex) == false) return false;

   /* Comm�� Server/Client�� ����Ǿ� �ִ��� Check */
   if (CommInfoVector[HandleIndex].bEstablished == false) {
      nCommErrCode = DISCONNECTED;
      return false;
   }

   sock_fd = CommInfoVector[HandleIndex].sock_fd;

   arg = 1; ioctl(CommInfoVector[HandleIndex].sock_fd, FIONBIO, &arg);   /* non-blocking mode */
   while(read(sock_fd, Temp, 1) >= 1) {}
   arg = 0; ioctl(CommInfoVector[HandleIndex].sock_fd, FIONBIO, &arg);   /* blocking mode */

   return false;
}



/*------------------------------------------------------------------------------------*/
/* Description: SOCKET ��� ���� ����                                                    */
/* HandleIndex   : THANDLEINDEX type�� Handle.                                               */
/* return     : true�̸� ����, false�̸� ����                                         */
/*------------------------------------------------------------------------------------*/
BOOL CommDisconnect(THANDLEINDEX HandleIndex)
{
	printf("COM] Comm Disconnected \n");
   /* Handle Check */
   if (HandleIndexCheck(HandleIndex) == false) return false;

   /* Comm�� Server/Client�� ����Ǿ� �ִ��� Check */
   if (CommInfoVector[HandleIndex].bEstablished == false) {
      nCommErrCode = DISCONNECTED;
      return false;
   }

   close(CommInfoVector[HandleIndex].sock_fd);
   CommInfoVector[HandleIndex].bEstablished = false;
   return true;
}



/*------------------------------------------------------------------------------------*/
/* Description: SOCKET ��� ȯ�� Close                                                   */
/* HandleIndex   : THANDLEINDEX type�� Handle.                                               */
/* return     : true�̸� ����, false�̸� ����                                         */
/*------------------------------------------------------------------------------------*/
BOOL CommClose(THANDLEINDEX HandleIndex)
{
   /* Handle Check */
   if (HandleIndexCheck(HandleIndex) == false) return false;

   CommDisconnect(HandleIndex); // Client Socket Close !!!

   if (CommInfoVector[HandleIndex].nType == ACCEPT) close(CommInfoVector[HandleIndex].svr_sock_fd); // Server type ... Close
   CommInfoVector[HandleIndex].bOpen = false;

   return true;
}



/*------------------------------------------------------------------------------------*/
/* Description: SOCKET ��� ȯ���� Handle Check                                          */
/* HandleIndex   : THANDLEINDEX type�� Handle.                                               */
/* return     : true�̸� ����, false�̸� ����                                         */
/*------------------------------------------------------------------------------------*/
BOOL HandleIndexCheck(THANDLEINDEX HandleIndex)
{
   /* Comm Handle�� ��ȿ���� Check */
   if (!(0 <= HandleIndex && HandleIndex <= MAX_HANDLE_INDEX)) {
      nCommErrCode = INVALID_COMM_HANDLE;
      return false;
   }

   /* Open�Ǿ� �ִ� Handle���� Check */
   if (CommInfoVector[HandleIndex].bOpen == false) {
      nCommErrCode = COMM_HANDLE_CLOSED;
      return false;
   }

   return true;
}




/*------------------------------------------------------------------------------------*/
/* Description: ���� �ֱ��� Error ������ Return                                       */
/*------------------------------------------------------------------------------------*/
int GetLastCommError()
{
   return nCommErrCode;
}



/*------------------------------------------------------------------------------------*/
/* Description: ���� �ֱ��� Error ���� ����                                           */
/*------------------------------------------------------------------------------------*/
void SetLastCommError(int nErrCode)
{
   nCommErrCode = nErrCode;
}


/*------------------------------------------------------------------------------------*/
/* Description: ���� �ֱ��� Error ������ ǥ�� ��¿� ǥ��                             */
/*------------------------------------------------------------------------------------*/
void PrintLastCommError()
{

   switch(nCommErrCode)
   {
      case INVALID_OPEN_TYPE:
         printf("COM] Invalid Open Type\n");
         break;

      case INVALID_COMM_HANDLE:
         printf("COM] Invalid ��� Handle\n");
         break;

      case COMM_HANDLE_CLOSED:
         printf("COM] ��� Handle is Closed \n");
         break;

      case COMM_INFO_TABLE_FULL:
         printf("COM] ��� ���� Table Full\n");
         break;

      case CONNECT_FAIL:
         printf("COM] CONNECT Fail\n");
         break;

      case ACCEPT_FAIL:
         printf("COM] ACCEPT Fail\n");
         break;

      case SETSENDBUF_FAIL:
         printf("COM] SetSendBuf() Fail\n");
         break;

      case SETRECVBUF_FAIL:
         printf("COM] SetRecvBuf() Fail\n");
         break;

      case READ_FAIL:
         printf("COM] read() Fail\n");
         break;

      case WRITE_FAIL:
         printf("COM] write() Fail\n");
         break;

      case DISCONNECTED:
         printf("COM] ������ �������� \n");
         break;

      case TIME_OUT:
         printf("COM] Time Out\n");
         break;

      case READ_INVALID_CODE:
         printf("COM] Received Invalid Code\n");
         break;

      case CRC_NOT_MATCHING:
         printf("COM] CRC not matching\n");
         break;

      case SEQUENCE_ERROR:
         printf("COM] Sequence Error\n");
         break;

      case SIZE_ERROR:
         printf("COM] SIZE not matching\n");
         break;

      case STX_ERROR:
         printf("COM] STX error\n");
         break;

      case ETX_ERROR:
         printf("COM] ETX error\n");
         break;

      case OPCODE_ERROR:
         printf("COM] OP CODE error\n");
         break;

      default:
         printf("COM] error code (%d) not found\n",nCommErrCode);
         break;
   }
}

//*****************************************************************************
int fnCreateUDP (short nPortID)	//UDP socket open
//*****************************************************************************
{
	int			s;
	struct		sockaddr_in		sin;
//	bool		bReuseAddr = TRUE;

	s = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == -1)
	{
		return -1;
	}

	if (nPortID != 0)
	{
		sin.sin_family			= AF_INET;
		sin.sin_addr.s_addr		= INADDR_ANY;
		sin.sin_port			= htons (nPortID);
		memset (&sin.sin_zero[0], 0, sizeof (sin.sin_zero));

//		setsockopt (s, SOL_SOCKET, SO_REUSEADDR, (const char *) &bReuseAddr, sizeof(bReuseAddr));
		if (bind(s, (const struct sockaddr  *) &sin, sizeof(struct sockaddr)) != 0)
		{
			if (0 == close (s))
			return -1;
		}
	}

	return s;
}
//*****************************************************************************
int fnClose (int s)
//*****************************************************************************
{
	if (0 != close (s))
		return s;
	return -1;
}
//*****************************************************************************
int fnSendDgram (int s, unsigned long  lIpAddr, char cBroadcast,short nPortID, char * lpData, int nDataLength)
//*****************************************************************************
{
	struct	sockaddr_in		sin;
	int				nSend;
	bool			bBroadcast, bDontroute;

	//if (lIpAddr == 0)
	//	lIpAddr = LOCALIP;

	sin.sin_family		= AF_INET;
	sin.sin_addr.s_addr	= htonl (lIpAddr);
	sin.sin_port		= htons (nPortID);
	memset (&sin.sin_zero[0], 0, sizeof (sin.sin_zero));

//	if (sin.sin_addr.S_un.S_un_b.s_b4 == 255) //Broadcast?  QNX?????????

	/*if(cBroadcast == BROADCAST)
	{
		bBroadcast = TRUE;

		if (setsockopt (s, SOL_SOCKET, SO_BROADCAST, (char *) &bBroadcast, sizeof (bool)) != 0)
		{
			return -1;
		}
	}*/

	bDontroute = FALSE;
	if (setsockopt (s, SOL_SOCKET, SO_DONTROUTE, (char  *) &bDontroute, sizeof (bool)) != 0)
	{
		return -1;
	}

//	nSend = sendto(s, lpData, nDataLength, MSG_DONTROUTE, (LPSOCKADDR)&sin, sizeof(SOCKADDR) );
	nSend = sendto(s, lpData, nDataLength, 0, (struct sockaddr *)&sin, sizeof(struct sockaddr) );
	if (nSend < 0)
	{
		return -1;
	}

//	if (sin.sin_addr.S_un.S_un_b.s_b4 == 255)	   //Broadcast? QNX?????????
	/*if(cBroadcast == BROADCAST)
	{
		bBroadcast = FALSE;
		if (setsockopt (s, SOL_SOCKET, SO_BROADCAST, (char  *) &bBroadcast, sizeof (bool)) != 0)
		{
			return -1;
		}
	}*/

	return nSend;
}
//*****************************************************************************
int fnRecvDgram (int s, char * lpData, int nDataLength, unsigned long * plIp)
//*****************************************************************************
{
	struct sockaddr_in		sin;
	static	int		nNameLen = sizeof(struct sockaddr);
	int				nRecv;

	nRecv = recvfrom (s, lpData, nDataLength, 0, ( struct sockaddr *) &sin, &nNameLen);

	if (plIp)
		*plIp = ntohl(sin.sin_addr.s_addr);
/*
	//98.6.18 ���� <= -> <
	if (nRecv < 0)
	{
		if (WSAGetLastError () == WSAEWOULDBLOCK)
			return 0;
		else
			return -1;
	}
	*/
	return nRecv;
}

//*****************************************************************************
bool  fnSetBuffer (int s, int nSendBuf, int nRecvBuf)
//*****************************************************************************
{
	if (0 != setsockopt (s, SOL_SOCKET, SO_SNDBUF, (char *)&nSendBuf, sizeof(int)))
		return FALSE;

	if (0 != setsockopt (s, SOL_SOCKET, SO_RCVBUF, (char *)&nRecvBuf, sizeof(int)))
		return FALSE;

	return TRUE;
}
//*****************************************************************************
bool fnSetBlock (int s, bool bBlock)
//*****************************************************************************
{
	unsigned long	ulBytesToRead;

	if (bBlock)
		ulBytesToRead = 0;
	else
		ulBytesToRead = 1;

	if (0 != ioctl (s, FIONBIO, &ulBytesToRead))
	{
		return FALSE;
	}
	return TRUE;
}
