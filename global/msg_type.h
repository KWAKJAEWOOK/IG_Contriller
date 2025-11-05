// 쓰려나
#ifndef _MSG_TYPE_
#define _MSG_TYPE_

#include "global.h"

enum {
	MSG_SET_OPERATION_TYPE = 101,
	MSG_SET_RTC,
	MSG_SET_SCENARIO,
	MSG_REQ_PERIOD_CAR_INFO,
	MSG_VIDEO_DETECT_INFO,
	MSG_SEND_EVENT_TO_CENTER,
	MSG_DETECT_STATUS_INFO,
	MSG_CONTROL_ONOFF,
	MSG_CONTROL_RESET,
	MSG_SET_TIMETABLE,
	MSG_SEND_SUB_SIGNPOLE_INFO,
	MSG_SEND_UNDERCONSTRUCTION_INFO_TO_CENTER,
	MSG_SEND_TRAFFIC_LIGHT_INFO,

    MS_EXIT

};

typedef struct {
    int     msg_code;        	// Message Code
    int     msg_len;

    BYTE	dMessage[1024];
} vmsg_t;

typedef struct {
	long int 	mtype;             /* type of received/sent message */
	vmsg_t 		vMessage;
} trx_msg_t;

#define VMSG_SIZE sizeof(vmsg_t)

enum {
	SVMSG_KEY_IG_SERVER_MANAGER_Q = 2001,
	SVMSG_KEY_LED_MANAGER_Q,
	SVMSG_KEY_M30_MANAGER_Q,
	SVMSG_KEY_CONNECTION_MANAGER_Q,
	TOTAL_KEY_COUNT
};

#define SVMSG_TYPE_V		101
#define SVMSG_TYPE			100

extern int 	IG_SERVER_Q;
extern int 	LED_Q;
extern int	M30_Q;
extern int	CONNECTION_Q;

BOOL msg_all_create();
BOOL msg_all_delete();
BOOL msg_all_open();

int  msg_create(int msq_key);
BOOL msg_delete(int msg_qid);
BOOL msg_erase(int msg_qid);
void msg_dump(int msg_qid);
void msg_clear(int	msg_qid);
void msg_all_dump(void);

#endif /* #ifndef _MSG_TYPE_ */
