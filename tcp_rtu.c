

//网关服务器内部错误码定义
typedef enum
{
	MBGATE_ERROK = 0,        //无操作
	MBGATE_BADREQUEST,       //请求不合法
	MBGATE_BADPROCTOL,       //协议字段校验失败
	MBGATE_ERRSENDRTU,       //发送RTU帧失败
	MBGATE_ERRRECVRTU,       //接收RTU帧失败
	MBGATE_BADCRC            //RTU帧校验失败
}eMBGATEErrorCode;


//用于串口访问的互斥信号量
static sys_sem_t usart_sem;

//RTU帧中地址域最大取值
#define  MODBUSTCP_ADDRESS_MAX   (247)

//查询RS485总线上是否接收到RTU响应
#define  TRY_TIME_INTERVAL  (50)   //查询周期
#define  TRY_MAX_NUM        (20)   //最大查询次数

//Modbus/TCP帧的最大长度
#define  MB_USART_BUF_SIZE  (256+7)

//网关服务器处理Modbus/TCP帧内部缓冲区
static unsigned char TCPSendReceiveBuf[MB_USART_BUF_SIZE];

//FreeModbus内部处理ModbusRTU帧的缓冲区，在mbrtu.c中定义
extern unsigned char ucRTUBuf[];

/**
*将Modbus/TCP请求转换为Modbus/RTU请求并发送到串行链路上，同时等待Modbus/RTU响应返回，
*并将其转化为Modbus/TCP响应并返回给客户端。
*conn:对应客户端的连接结构；inbuf:来自客户端的Modbus/TCP请求
*返回值：正确处理则返回MBGATE_ERROK,否则返回响应错误值
**/
eMBGATEErrorCode ModbusRquestHadle(struct netconn *conn, struct netbuf *inbuf)
{
	unsigned char *dataptr = NULL;
	u16_t  datasize = 0;
	eMBGATEErrorCode processflag = MBGATE_ERROK;
	eMBErrorCode  err=MB_ENOERR;

	//Modbus/TCP请求MBAP帧头各个字段值
	unsigned int usPID    = 0;
	unsigned int usLength = 0;
	unsigned char usUID   = 0;
	unsigned char usFUN   = 0;

	//用于串口状态查询的变量
	eMBEventType eEvent;
	unsigned char trytimes = 0;

	//接收到的Modbus/RTU帧
	unsigned char *PDUStartAddr = NULL;		//RTU PDU起始地址
	unsigned char RTURcvAddress;            //RTU ADU地址域
	unsigned short PDULength;               //RTU PDU长度

	netbuf_data(inbuf,&dataptr,&datasize);

	do
	{
		//校验Modbus/TCP请求帧数据长度
		if (datasize > MB_USART_BUF_SIZE || datasize < LWIP_TCP_FUNC)
		{
			processflag = MBGATE_BADREQUEST;
			break;
		}

		//拷贝至内部缓冲区中以便对数据进行操作
		memcpy(TCPSendReceiveBuf,dataptr,datasize);

		//1.将Modbus/TCP帧转换为Modbus/RTU帧
		usPID = (TCPSendReceiveBuf[LWIP_TCP_PID] << 8U) + TCPSendReceiveBuf[LWIP_TCP_PID+1];
		usLength = (TCPSendReceiveBuf[LWIP_TCP_LEN] << 8U) + TCPSendReceiveBuf[LWIP_TCP_LEN+1];
		usUID = TCPSendReceiveBuf[LWIP_TCP_UID];
		usFUN = TCPSendReceiveBuf[LWIP_TCP_FUNC];

		//校验MBAP首部各个字段
		if (usPID != MODBUSTCP_PROTOCOL_ID || (usLength + LWIP_TCP_UID) != datasize 
			|| usUID > MODBUSTCP_ADDRESS_MAX)
		{
				processflag = MBGATE_BADPROCTOL;
				break;
		}

		//将Modbus/RTU拷贝到ucRTUBuf中
		memcpy(ucRTUBuf,$TCPSendReceiveBuf[LWIP_TCP_UID],usLength);

		//2.发送Modbus/RTU帧，该函数将自动添加CRC
		err = eMBRTUSend(usUID,&ucRTUBuf[1],usLength-1);
		if (err != MB_ENOERR)
		{
			processflag = MBGATE_ERRSENDRTU;
			break;
		}

		//3.发送成功后，等到Modbus/RTU响应
		if (usUID == 0)	 //若是广播请求，则无需等待响应
		{
			break;
		}

		//循环检测串口数据是否就绪
		do{
			OSTimeDlyHMSM(0, 0, 0, TRY_TIME_INTERVAL);

			if(xMBPortEventGet(&eEvent) == TRUE)
				break;
			trytimes++;
		}while(trytimes < TRY_MAX_NUM);    //尝试多次接收串口的数据

		if(trytimes == TRY_MAX_NUM || eEvent != EV_FRAME_RECEIVED)
		{
			//在多次尝试后扔未接收到响应，则接收失败
			processflag = MBGATE_ERRRECVRTU;
			break;
		}


		//接收RTU响应成功，则读取数据，其中PDUStartAddr表示PDU的起始地址，
		//而RTURcvAddress则表示RTU ADU地址域
		//eMBRTUReceive
		err = eMBRTUReceive(&RTURcvAddress,&PDUStartAddr,&PDULength);

		if(err != MB_ENOERR)
		{
			processflag = MBGATE_BADCRC;
			break;
		}

		//4.将Modbus/RTU转化为Modbus/TCP帧
		TCPSendReceiveBuf[LWIP_TCP_LEN] = (PDULength + 1) >> 8U;	//加1为单元标识符字节（地址域)1个字节
		TCPSendReceiveBuf[LWIP_TCP_LEN + 1] = (PDULength + 1) & 0xFF;
		TCPSendReceiveBuf[LWIP_TCP_UID] = RTURcvAddress;			//为单元标识符赋值，对应RTU的地址域
		memcpy(&TCPSendReceiveBuf[LWIP_TCP_FUNC],PDUStartAddr,PDULength);  //LWIP_TCP_FUNC为7，从功能码开始拷贝

		//5.发送Modbus/TCP响应给客户端，拷贝方式发送
		netconn_write(conn, TCPSendReceiveBuf,PDULength + LWIP_TCP_FUNC, NETCONN_COPY);


	}while(0);


	//返回处理结果
	return processflag;

}

/**子任务在调用ModbusRquestHadle处理Modbus/TCP请求之前，必须先获取信号量usart_sem,从而保证对RS485接口的独占访问
*上面几个重要函数是在移植FreeModbus中完成的：一是RTU帧发送函数eMBRTUSend,该函数会将MBAP的单元标识字段usUID封装到
*串行链路Modbus/RTU数据帧的地址域中，添加CRC校验字段构造完整的RTU数据帧发送出去；在等待读取串行链路服务器的响应时，
*使用的是xMBPortEventGet函数来检测串口状态机，若FreeModbus成功接收到了响应，eMBRTUReceive函数将被调用来读取响应帧。
*/