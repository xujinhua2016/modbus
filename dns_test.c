//DNS协议包含了完整的报文结构定义，如请求报文，应答报文等，本质上
//DNS使用UDP进行报文的传输，以实现DNS服务器和DNS客户端间的数据交互

//首先需要设置用户配置文件lwipopt.h ，打开与DNS相关的宏开关。
//#define LWIP_DNS    1
//此时，与DNS功能相关的文件dns.c将会被编译。
/**
*要使用DNS的查询功能，在Raw/Callback API中可以调用函数dns_gethosbyname进行域名
*解析，此时用户需要自行定义一个回调函数，该函数类型如下，当内核成功完成域名解析后，
*用户的回调函数会被内核执行，其中参数ipaddr指向最终成功解析的IP地址。
*typedef void(*dns_found_callback)(const char *name, ip_addr_t *ipaddr void *callback_arg);
*在Sequential API中可以调用函数netconn_gethostname进行域名解析，该函数会一直阻塞，直到DNS
*解析域名成功，当域名无法解析或解析错误时，函数返回ERR_VAL；
*基于DNS的测试程序如下，该程序为一个TCP服务器，它能够接收客户端的链接，并将客户端发送的域名字符
*串解析成对应的IP地址，最后将地址信息返回给客户端。
*/

#define MAX_BUFFER_LEN 256
char recvbuf[MAX_BUFFER_LEN];       //数据接收缓存，允许的最大域名长度
char sendbuf[MAX_BUFFER_LEN];       //数据发送缓冲

void dns_netconn_thread(void *pdata)
{
	struct netconn *conn = NULL, *newconn = NULL;
	err_t ret = ERR_OK;

	conn = netconn_new(NETCONN_TCP);    //新建TCP连接
	netconn_bind(conn, NULL, 8080);     //绑定本地端口
	netconn_listen(conn);               //服务器进入侦听状态

	while(1)
	{
		ret = netconn_accept(conn, &newconn);  //接受新连接
		while(newconn != NULL)                 //新连接有效，则循环接收数据并处理
		{
			struct netbuf *inbuf;
			char *dataptr;
			u16_t size;

			inbuf = netconn_recv(newconn);
			if (inbuf != NULL)				//数据有效
			{
				struct ip_addr dnsaddr;
				netbuf_data(inbuf, &dataptr,&size);    //获取数据信息
				if(size >= MAX_BUFFER_LEN)             //数据长度验证
				{
					netbuf_delete(inbuf);
					continue;
				}

			}

			MEMCPY(recvbuf,dataptr,size);    //将数据拷贝到recvbuf中
			recvbuf[size] = '\0';            //字符串在数组中，最后为'\0'
			netbuf_delete(inbuf);

			//调用函数，请求域名服务器解析出对应的IP地址
			if ((ret = netconn_gethostbyname((char*)(recvbuf), &(dnsaddr))) == ERR_OK)
			{
				u16_t strlen = sprintf(sendbuf,"%s = %s\n",recvbuf,ip_ntoa(&dnsaddr));
				if (strlen > 0)		//解析结果转为字符串，并向客户端返回
				{
					netconn_write(newconn,sendbuf,strlen,NETCONN_TCP);
				}
			}else{
				//若接收到NULL，说明对方已断开连接
				netconn_close(newconn);  //关闭连接
				netconn_delete(newconn); //删除连接结构
				newconn = NULL;          //结束本次循环
			}

		}//while(newconn != NULL)
	}
}

//服务器初始化函数
void dns_netconn_init()
{
	sys_thread_new("dns_netconn_thread", dns_netconn_thread, NULL, DEFAULT_THREAD_STACKSIZE, TCPIP_THREAD_PRIO+1);
}

/**
*函数netconn_gethostbyname的原型如下，其参数name指向一个字符串，该字符串包含了请求解析的域名。
*err_t netconn_gethostbyname(const char *name, struct ip_addr *addr)
*当内核向域名服务器（这里为默认网关192.168.1.1）发送关于该地址的域名解析请求，并在收到DNS服务器
*的正确响应后，解析出的IP地址会被填写到地址addr中。用户可以把addr看作一个返回值，并把它使用在后续的编程中。
*
****/


/**
*下载外网数据
*DNS为访问外网服务器提供了方便，我们只要知道一个服务器的域名就可以方便的访问。
*让板子诸佛那个到外网服务器下载数据。与世界互联。
*
*
*Socket中也提供了DNS相关的接口，其调用原型如下所示，域名字符串作为该函数的唯一输入参数，该函数将阻塞，
*直至域名解析成功，解析结果保存在结构hostent中
*struct hostent* gethostbyname(const char *name)
*hostent时host entry的缩写，可用于描述一台主机的全部地址信息，包括主机名、别名、地址值等定义如下
struct hostent{
	char *h_name;      //主机名称
	char **h_aliases;  //主机预备名称，可能有多个，最后一个为NULL
	int  h_addrtype;   //地址类型，通常为AF_INET
	int  h_length;     //地址长度，对于IP地址来说，该值为
	char **h_addr_list;//地址列表，可能有多个，最后一个为NULL
#define h_addr h_addr_list[0]  //地址列表中第一个地址
};
*目前在Lwip中h_addr_list只支持一个地址，即解析到的ip地址在h_addr_list[0]中，而h_addr_list[1]始终为NULL。
*
*本历程可以完成与外网服务器www.163.com建立HTTP连接，并在连接建立后向服务器请求固定目录下的数据，当数据
*传输完成后，服务器断开连接；此后客户端重新尝试建立连接并重新下载数据，如此循环。
**/
#define SOCK_TARGET_PORT 80;   //服务器端口
int reclen = 0;                //记录从服务器下载的总数据量
struct hostent* hip = NULL;    //服务器域名地址信息

static u8_t buf[1024];         //数据接收内部缓冲区

//谅解服务器并下载数据
static void sockex_nonblocking_connect(void *arg)
{
	//服务器数据请求
	char *m_requestheader = "GET http://www.163.com HTTP/1.1\r\nHost:www.163.com\r\n \
	                          Accept:*/*\r\nUser-Agent:Mozilla/4.0(compatible;MSIE 7.0; \
	                          Windows NT 6.0;Trident/4.0)\r\nConnection:Close\r\n\r\n";

	int s, ret, err;
	struct sockaddr_in addr;
	int tick = 0;
	int count = 0;

	memset(&addr,0,sizeof(addr));    //初始化服务器地址信息
	addr.sin_len = sizeof(addr);
	addr.sin_family = AF_INET;
	addr.sin_port = PP_HTONS(SOCK_TARGET_PORT);
	addr.sin_addr.s_addr = inet_addr(inet_ntoa(*((struct in_addr *)hip->h_addr_list[0])));

	s = lwip_socket(AF_INET,SOCK_STREAM,0);  //新建socket
	ret = lwip_connect(s,(struct sockaddr *)&addr,sizeof(addr));//连接服务器
	ret = lwip_write(s,m_requestheader,strlen(m_requestheader));//发送请求

	while(1)
	{
		//循环读取服务器返回的数据
		count = lwip_read(s,buf,1024);
		if (count == -1)                    //接收错误，则关闭连接并退出
		{
			break;
		}
		if (count != 0)                     //读取数据成功，则计算总长度
		{
			tick = 0;
			reclen += count;
			//串口输出当前接收长度以及数据总长度
			Printf("new len %d, total Led:%d KB\r\n",count,reclen/1024);
		}else{   //若接收数据失败，则延迟等待
			sys_msleep(300);
			if (tick == 10)  //若10次内未收到任何数据，则关闭连接并退出
			{
				break;
			}
		}
		tick++;
	}

	ret = lwip_close(s);    //断开连接

}


static void sockex_nonblocking(void *arg)
{
	arg = arg;

	while(hip == NULL)
	{
		//解析服务器域名，直至解析成功
		hip = gethostbyname("www.163.com");  //此处调用的是Socket中也提供了DNS相关的接口
		if (hip != NULL)
		{
			printf("IP address :%s\n", inet_nota(*((struct in_addr *)hip->h_addr_list[0])));
		}
	}

	while(1)
	{
		//连接服务器并下载数据
		sockex_nonblocking_connect(NULL);
		sys_msleep(3000);  //下载完成后，等待3s,继续下一次下载

	}
}

//客户端初始化函数
void socket_examples_init(void)
{
	sys_thread_new("sockex_nonblocking_connect",sockex_nonblocking,NULL,0,TCPIP_THREAD_PRIO + 1)
}