/*
*函数ModbusRquestHadle对请求的具体处理流程如下，其基本思想就是按照Modbus/TCP
*请求帧格式解析数据包，获得其中的操作码字段，同时回调FreeModbus应用层的功能码
*处理函数，并根据函数执行结果想客户端返回响应帧。
*/


//FreeModbus中对应的功能码处理回调函数（处理Modbus PDU）
#include "mbconfig.h"

extern xMBFunctionHandler xFuncHandlers[MB_FUNC_HANDLERS_MAX];


//Modbus/TCP最大帧长度
#define  MB_MAX_BUF_SIZE  (256+7)

//服务器内部Modbus/TCP处理缓冲区
static unsigned char TCPSendReceiveBuf[MB_MAX_BUF_SIZE];

//MBAP帧头各字段的偏移值
#define  LWIP_TCP_TID    0       //事务标识符
#define  LWIP_TCP_PID    2       //协议标识符
#define  LWIP_TCP_LEN    4       //长度
#define  LWIP_TCP_UID    6       //设备标识符
#define  LWIP_TCP_FUNC   7       //功能码

#define  MODBUSTCP_PROTOCOL_ID    0     //协议标识符， 0 = Modbus协议

/**
*处理Modbus/TCP请求并向客户端返回处理结果
*conn:对应客户端的连接结果
*inbuf:来自客户端的Modbus/TCP请求
*返回值：正确处理则返回MBS_ERROK，否则返回响应错误值
*/
eMBServerErrorCode ModbusRquestHadle(struct netconn *conn, struct netbuf *inbuf)
{
	unsigned char *dataptr = NULL;
	u16_t datasize = 0;
	unsigned int i;
	eMBGATEErrorCode processflag = MBS_ERROK;

	eMBException eException;        //功能码回调函数执行结果
	unsigned char  *ucMBFrame;      //Modbus PDU起始地址

	//MBAP帧头各个字段值
	unsigned int usPID;             //PID
	unsigned short usLength;        //LEN
	unsigned char usUID;            //UID
	unsigned char ucFunctionCode;   //FUNC

	err_t sendstat;             //向客户端发送响应

	//获得请求中的数据地址和长度
	netbuf_data(inbuf, &datasize);

	do
	{
		//滤除长度不合法的请求
		if (datasize > MB_MAX_BUF_SIZE || datasize < LWIP_TCP_FUNC)
		{
			processflag = MBS_BADREQUEST;
			break;
		}

		//将整个Modbus/TCP请求拷贝到内部缓冲，方便对数据的处理
		memcpy(TCPSendReceiveBuf, dataptr, datasize);

		//获得请求中MBAP各字段，
		usPID = (TCPSendReceiveBuf[LWIP_TCP_PID] << 8U) + TCPSendReceiveBuf[LWIP_TCP_PID+1];     //2个字节
		usLength = (TCPSendReceiveBuf[LWIP_TCP_LEN] << 8U) + TCPSendReceiveBuf[LWIP_TCP_LEN+1];  //2个字节
		usUID = TCPSendReceiveBuf[LWIP_TCP_UID];             //1个字节
		ucFunctionCode = TCPSendReceiveBuf[LWIP_TCP_FUNC];   //1个字节

		//获得Modb PDU起始地址
		ucMBFrame = &TCPSendReceiveBuf[LWIP_TCP_FUNC];

		//对PID和LEN进行验证，LWIP_TCP_UID为6，6个字节，2字节事务标识符+2字节协议标识符+2字节长度
		if (usPID != MODBUSTCP_PROTOCOL_ID || (usLength + LWIP_TCP_UID) != datasize)
		{
			processflag = MBS_BADPROCTOL;
			break;
		}

		usLength--;     //得到Modbus PDU的长度

		eException = MB_EX_ILLEGAL_FUNCTION;		//功能码回调函数执行结果
		for (i = 0; i < MB_FUNC_HANDLERS_MAX; i++)
		{
			//根据功能码查找对应的功能码处理回调函数
			if (xFuncHandlers[i].ucFunctionCode == 0 || xFuncHandlers[i].pxHandler == NULL)
			{
				processflag = MBS_ERRFUNC;
				break;
			}
			else if(xFuncHandlers[i].ucFunctionCode == ucFunctionCode)
			{
				//调用功能码回调函数，处理Modbus PDU
				//pxHander处理结束后，会生成Modb PDU响应，响应结果存储在ucMBFrame中，
				//并且usLength返回了响应的长度
				eException = xFuncHandlers[i].pxHandler(ucMBFrame,&usLength);
				break;
			}
		}

		//如果功能码未注册，则设置错误标志
		if (i == MB_FUNC_HANDLERS_MAX)
		{
			processflag = MBS_ERRFUNC;
		}
		if (processflag == MBS_ERRFUNC)
		{
			break;
		}

		//此时，Modbus PDU处理完毕，需要向客户端返回处理结果
		if (usUID == MB_ADDRESS_BROADCAST)
		{
			//对于广播地址，不返回任何结果
			break;
		}

		//注：当服务器响应客户机时，它可以使用PDU的功能码字段来表示正常响应（无差错执行）
		//还是异常响应（出现某种异常或错误）。对于正常响应而言，服务器响应包中的功能码字段只是简单的复制
		//客户端请求包中的功能码；而对于异常响应来说，服务器会将客户端请求中的功能码最高有效位置为1后返回，
		//同时响应包鞋到了异常吗，用户指明差错的类型。
		//01 :不支持该功能码  02：越界 03：寄存器数量超出范围 04：读写错误

		//若处理Modbus PDU出现错误，则构造异常响应帧
		if (eException != MB_EX_NONE)
		{                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     
			//将Modbus PDU中的功能码最高位置1
			usLength = 0;
			ucMBFrame[usLength++] = (UCHAR)(ucFunctionCode | MB_FUNC_ERROR);

			//数据区域中携带相关错误码
			ucMBFrame[usLength++] = eException;
		}

		//注意：ucMBFrame是指向Modbus PDU首地址，其指向的内容被包含在TCPSendReceiveBuf[]，
		//故发送TCPSendReceiveBuf时，ucMBFrame所指向的内容也被发送出去

		//调整Modbus PDU的LEN字段
		TCPSendReceiveBuf[LWIP_TCP_LEN] = (usLength + 1) >> 8U;
		TCPSendReceiveBuf[LWIP_TCP_LEN + 1] = (usLength + 1) & 0xFF;

		//向客户端返回数据，数据拷贝方式发送，
		sendstat = netconn_write(conn, TCPSendReceiveBuf, usLength + LWIP_TCP_FUNC, NETCONN_COPY);

		if(sendstat != ERR_OK)		//记录发送结果
		{
			processflag = MBS_ERRSEND;
		}

	}while(0);

	return processflag;

}


//注：功能码处理函数xFuncHandlers是在移植FreeModbus中完成的，它为每个对应的功能码定义了一个回调函数，
//例如控制线圈状态、控制阀门状态等。ModbusRquestHadle本质工作就是根据功能码查找相关的回调函数来处理
//Modbus PDU







