//IGMP协议
/**
*前面所涉及的IP数据报发送基本都是基于单播方式进行，即发送方和接收方一一对应。但在实际应用中，某系主机希望把同样
*的数据发送给多个特定的接收方（如视频点播），这就涉及到多播（组播）的概念。习惯上将所有这些特定的数据接收方称为
*一个多播群组，主机可以动态的加入或退出一个多播群组，一个群组拥有一个唯一的D类IP地址，发送该IP地址的数据会被群组
*内的所有成员接收到。
*多播群组中的各成员可能都处于在单个物理网络中，也可能来自于不同的物理网络，在前一种情况下，多播的实现比较简单，
*IP的多播可以借助物理层的多播来实现（如以太网多播）；对于后一种情况，要实现多播功能，网络上必须具有能转发多播分组
*的路由器（多播路由器），由于多播群组中的成员来自不同的物理网络，必须由多播路由器实现多播数据在各个物理层中的转发。
*
*为了实现转发，多播路由器必须知道哪些主机加入了多播群组，此时，路由器和主机之间需要使用网际组管理协议（IGMP)进行
*通信，以实现成员关系的查询和通告。ICMP的工作分为两个流程：当一个主机新加入一个多播组时，它向该组的多播地址发送
*一个IGMP成员关系报告报文，以声明成员关系，本地的多播路由器接收到这个报文后，将向网络中的其他多播路由器传递这个组
*成员信息，以建立必须要的路由；在运行期间，多播路由器会周期性的探测多播组中的各个成员（成员关系查询报文），以便
*知晓各个成员是否还存在，如果多播组中的任一成员响应这种探测，则路由器就保持该群组的活跃性。若经过多次探测后，群组
*中没有任何主机响应，多播路由器就认为当前网络中不在有属于该群组的主机，同时也停止向其他多播路由器通告该群组的信息。
*
*参与IP多播的主机可以在任何位置、任何时间、成员总数不受限制的加入或退出多播组。多播路由器不需要也不可能保存所有主机
*的成员关系，它只能通过IGMP协议查询每个接口连接的网段上是否存在某个多播组的接受者，即组成员，而主机方只需要保存自己
*加入了哪些多播组。
*
*从TCP/IP协议的分层结构上看，IGMP与ICMP类似，属于IP层协议。类似的，IGMP也使用IP协议来传送报文（IP数据报中的协议字段
*为2），因此，我们不能把IGMP看作是一个独立的协议，事实上，它是IP协议不可分割的整体。
*
*lwip中实现了IGMP的功能，应用程序可以调出IGMP接口函数加入到某个多播组中，这样，在IP层接收到的关于该多播地址的数据时，
*数据包将允许向上层递交。注意，由于多播对应的是一对多通信，所以在传输层协议中，只有UDP能使用IGMP提供的多播功能。
**/

/**
*要使用Lwip提供的IGMP功能，首先必须保证网卡具有多播数据接收功能，且网卡已被正确配置，同时网卡多播数据接收功能已使能
*（对ENC38J60网卡来说，其数据接受过滤器ERXFCON的MCEN位需要使能，表示允许网卡驱动接收多播数据包）。在底层初始化时，
*必须配置网络接口结构netif的flags字段，表示的NETIF_FLAG_IGMP位必须设置。
*
*#define  LWIP_IGMP  1
*
*如下程序本质上是一个基于Sequential API的UDP客户端，完成了如下功能：加入到多播组233.0.0.6中，并接收该组中的数据，
*如果收到数据，则将这些数据发送到主机192.168.1.103的端口8080上，主机能将这些数据加以显示。
**/

extern struct netif enc28j60_netif;
void igmp_netconn_thread(void *pdata)
{
	struct netconn *conn;
	struct ip_addr loacl_addr,group_addr,remote_addr;
	err_t err = ERR_OK;
	while(enc28j60_netif.dhcp->state != DHCP_BOUND)   //DHCP是否获得有效地址
		OSTimeDly(10);                                //等待，直到得到有效地址

	//构造三个IP地址
	loacl_addr = enc28j60_netif.ip_addr;              //本地IP地址
	IP4_ADDR(&group_addr,233,0,0,6);                  //多播地址
	IP4_ADDR(&remote_addr,192.168.1.103);             //服务器地址

	conn = netconn_new(NETCONN_UDP);                  //新建UDP类型的链接结构
	netconn_bind(conn,NULL,9090);                     //绑定本地端口9090上

	//将本地地址加入多播组
	netconn_join_leave_group(conn,&group_addr,&loacl_addr,NETCONN_JOIN);

	while(1)
	{
		struct netbuf *inbuf;
		err = netconn_recv(conn,&inbuf);              //端口9090上等待接收数据
		if (err == ERR_OK)                            //数据有效
		{
			//则将数据发往主机remote_addr的8080端口上
			netconn_sendto(conn,inbuf,&remote_addr,8080);
			netbuf_delete(inbuf);                     //删除数据
		}
	}

	netconn_delete(conn);
}

//UDP客户端初始化函数
void igmp_netconn_init()
{
	sys_thread_new("igmp_netconn_thread",igmp_netconn_thread,NULL,DEFAULT_THREAD_STACKSIZE,TCPIP_THREAD_PRIO+1);
}

/**
*在使用Sequential API时，用户可以调用函数netconn_join_leave_group来加入或退出一个多播组，该函数的原型如下：
err_t netconn_join_leave_group(struct netconn *conn,
                               struct ip_addr *multiaddr,
                               struct ip_addr *interface,
                               enum netconn_igmp join_or_leave)
*参数multiaddr指明了多播组的地址，他必须是一个有效的D类地址，这里使用了233.0.0.6；
*参数interface指出了加入到多播组中的本地IP地址，多播路由器会记录下该IP地址，并为主机维护一个多播数据的路由信息；
*参数join_or_leave可以为NETCONN_JOIN或NETCONN_LEAVE，分别代表加入或退出多播组。
*/