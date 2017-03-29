#define  MAX_CLIENT_NUM    5     //最大子任务数（最大并发数量）
#define  CLIENT_STK_SIZE   256   //各子任务堆栈大小

#define  CLIENT_START_PRIO 15    //各子任务其实优先级

//根据操作系统的特性，需要为每个任务自行分配堆栈空间
typedef struct child_stack
{
	OS_STK stk_area[MAX_CLIENT_NUM][CLIENT_STK_SIZE];    //所有子任务堆栈
	unsigned int stack_bitmap;                           //用位图表示堆栈分配情况
	sys_sem_t stack_sem;                                 //堆栈区访问互斥量
}child_stack_t;

child_stack_t child_stack_areas;                         //定义堆栈管理空间

//各个子任务访问共享资源的互斥信号量，这使用静态全局变量
static sys_sem_t mem_sem;

//Modbus TCP服务器熟知端口号
#define  MODBUS_SERVER_DEFAULT_PORT  502

//服务器内部处理错误码
typedef enum
{
	MBS_ERROK,       //无错误
	MBS_BADREQUEST,  //请求不完整
	MBS_BADPROCTOL,  //协议验证失败
	MBS_ERRFUNC,     //功能码错误
	MBS_ERRSEND,     //返回数据失败
}eMBServerErrorCode;

//管理子任务堆栈的几个函数，用位图来标识某个堆栈区域是否被使用
//堆栈任务分配时，查找为0的最低bit位并将其对应的堆栈区域分配给任务使用
//堆栈回收时，在位图中清除堆栈区域对应的bit位

//堆栈管理空间初始化
err_t ModbusStackInit(void)
{
	child_stack_areas.stack_bitmap = 0;                     //初始化时，堆栈管理空间结构体内的位图清零
	return sys_sem_new(&child_stack_areas.stack_sem, 1);    //初始化互斥信号量，为1
}

//查找可用的堆栈区域，返回堆栈区索引（找位图Bit为0的位）
unsigned int ModbusStackFind(void)
{
	unsigned int i = 0;                                     //用于保存最低为0的bit位索引
	sys_sem_wait(&child_stack_areas.stack_sem);             //获取互斥信号量

   //查找为0的最低bit
	while((child_stack_areas.stack_bitmap >> i) & 0x01)
	{
		if ((i < MAX_CLIENT_NUM) && (i < 32)) i++;
		else break;
	}

	sys_sem_signal(&child_stack_areas.stack_sem);

	return i;
}

//堆栈分配，在为位图中将堆栈区域对应的bit位置1
void ModubsStackGet(unsigned int index)
{
	sys_sem_wait(&child_stack_areas.stack_sem);
	child_stack_areas.stack_bitmap |= (0x01 << index);
	sys_sem_signal(&child_stack_areas.stack_sem);
}

//服务器主任务
void ModbusMainServer(void *p_arg)
{
	struct netconn *conn = NULL;
	struct netconn *newconn = NULL;
	err_t ret = ERR_OK;

	ret = sys_sem_new($mem_sem, 1);    //初始化访问共享资源的互斥信号量
	ret = ModbusStackInit();           //初始化子任务堆栈管理

	conn = netconn_new(NETCONN_TCP);   //初始化TCP服务器
	ret = netconn_bind(conn, NULL, MODBUS_SERVER_DEFAULT_PORT);
	ret = netconn_listen(conn);

	while(1)
	{
		ret = netconn_accept(conn, &newconn);    //服务器阻塞，接受新连接
		if (ret == ERR_OK)
		{
			//连接成功建立，则为其分配子任务堆栈空间
			unsigned int i = ModbusStackFind();
			if (i < MAX_CLIENT_NUM)
			{
				//堆栈空间有效，创建子任务
				INT8U Err = OSTaskCreate(ModbusClientServer,
										 (void *)newconn,
										 (OS_STK *)&child_stack_areas.stk_area[i][CLIENT_STK_SIZE - 1],
										 (INT8U)(CLIENT_START_PRIO + i));
				if (Err == OS_ERR_NONE)
				{
					//若子任务创建成功，则标识堆栈已使用
					ModubsStackGet(i);
					continue;
				}
			}

			//堆栈空间分配失败，或子任务创建失败，则关闭新连接
			//由于资源限制（对其空间分配失败），无法响应该连接
			netconn_close(newconn);
			netconn_delete(newconn);
			newconn = NULL;
		}//if
	}//while

}


//服务器子任务，负责处理单个连接上的请求，并向客户端返回响应
static void ModbusClientServer(void* p_arg)
{
	//获得堆栈区域索引，便于后续释放
	unsigned int task_index = (OSPrioCur - CLIENT_START_PRIO);
	struct netconn *newconn = (struct netconn *)p_arg;    //获得连接结构

	while(newconn)
	{
		struct netbuf *inbuf = NULL;
		inbuf = newconn_recv(newconn);    //阻塞接受客户端的请求

		if (inbuf != NULL)
		{
			//接收到请求，则首先获得资源访问的互斥信号量，再处理请求
			sys_sem_wait(&mem_sem);
			eMBServerErrorCode err = ModbusRquestHadle(newconn, inbuf);
			sys_sem_signal(&mem_sem);     //请求处理完毕，释放互斥量

			netbuf_delete(inbuf);         //删除客户端数据包
			if(err == MBS_ERROK)          //服务器不主动断开连接，继续处理后续请求
				continue;
		}
		else            //收到客户端的NULL数据包，表明客户端断开连接
		{
			netconn_close(newconn);       //服务器也自动断开本地连接
			netconn_delete(newconn);
			netconn = NULL;
		}


	}//while

	//子任务退出，释放堆栈区域，并删除任务自身
	ModbusStacKFree(task_index);
	OSTaskDel(OS_PRIO_SELF);

}


/*
*子任务负责接收客户端的请求，并调用函数ModbusRquestHadle来处理请求，
*注意当服务器上存在多个连接时，这些子任务可能将对同一系统资源进行访问和修改，
*为了实现对这些资源的互斥访问，这里引入了互斥信号量mem_sem。子任务在调用函数ModbusRquestHadle
*处理Modbus/TCP请求之前，应该先获得该互斥信号量。
*/



