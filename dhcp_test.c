/**
*在DHCP功能的支持下，IP地址的分配不仅是自动的（不需要用户干预），更可以是动态的。所谓动态，即DHCP服务
*器为客户分配的IP地址是临时的，服务器只是在一段时间内将给IP地址租用（lease）给客户机，服务器会为每个地址
*维护一个租期，在地址租用期间，服务器不会将该地址租给其他客户，但是在租期结束后，客户必须续租或者停止使用该地址。
*
*DHCP服务器一般有两个数据库，一个数据库维护静态分配的物理地址和IP地址绑定，它记录了特定客户端主机将被分配的特定IP地址，
*客户机可以永久使用这些IP地址；第二个数据库维护了一个IP地址池，其中记录了可以动态分配的IP地址。当客户端主机请求临时
*地址分配时，如果客户端的物理地址在静态数据库中有对应的IP地址项，则服务器将该IP地址呗分配给客户端，如果没有对应项，则
*服务器在地址池中为客户端分配一个地址。
*
*DHCP协议有着自身的报文组织格式，本质上，DHCP使用UDP进行报文的传输。这里有个问题，在主机DHCP获得地址之前，主机并没有
*有效的IP地址，那么主机又是如何使用UDP来发送DHCO报文呢：这一点似乎很矛盾。
*答案在于IP层有一个特殊的IP地址：受限广播IP地址（全1），当该地址作为目的地址时，子网内的所有主机都能收到该IP数据报。
*注意，IP层的广播实际上是基于链路层的广播来实现的，所以这里有个前提，即链路层具有广播功能，在以太网中，目的地址为全1
*的MAC地址可作为链路上的广播地址。只要数据包能够正确到达主机IP层，且主机上的应用程序已绑定在了某个UDP端口上，则应用
*程序就能收到该端口上的广播IP包，这正是DHCP初期设备能够获得IP地址的基本原理。
*
*Lwip中实现了DHCP客户端的功能，当该客户端启动时，它会自动向DHCP服务器（默认网关，192.168.1.1）发送地址分配请求，若得到
*正的响应，它会将有效地址设置到网络接口结构相关字段中。DHCP客户端一经启动后便一直存在于系统内核中，完成地址请求、地址
*选择、地址绑定、地址更新、重新绑定等操作。
*
*DHCP协议采用UDP作为传输协议，DHCP客户端于服务器双方通信的过程中，客户端使用固定端口号68，而服务器使用固定端口号67。
*通常，客户端需要获得IP，需要经过如下过程：
*
*(1)DHCP客户端以广播的方式发出DHCP DISCOVER报文。广播地址所能到达的所有DHCP服务器都能接收到该Discover报文。
*(2)所有的DHCP服务器都会对Discover报文给出响应，向DHCP客户端发送一个DHCP Offer报文。DHCP Offer报文中"Your(Client) IP
*Address"字段就是DHCP服务器能够提供给客户端使用的IP地址，同时DHCP服务器会将自己的IP地址放在报文的“option”字段中以便
*客户端区分不同的DHCP服务器。DHCP服务器在发送Offer报文后，在内部会记录一个已分配IP地址的记录。
*(3)DHCP客户端可能收到来自多个服务器的Offer报文，但是它只能对其中一个做响应，通常DHCP客户端处理最先收到的Offer报文
*并响应。
*(4)DHCP客户端从Offer报文中提取到可使用的IP地址和服务器IP地址后，会发出一个广播的DHCP Request报文，在选项字段中会
*加入选中的DHCP服务器的IP地址和自己需要的IP地址。
*(5)DHCP服务器收到DHCP Request报文后，判断选项字段中的IP地址是否与自己的地址相同。如果不相同，DHCP Server不做任何处理，
*只清除响应IP地址分配记录；如果相同，DHCP服务器就会想客户端响应一个DHCP ACK报文，并在报文选项字段中增加IP地址的
*使用租期信息。
*(6)DHCP客户端接收到DHCP ACK报文后，检查服务器分配的IP地址是否能够使用（通过发送一个ARP请求到网络中，如果无主机回应
*该请求，则表示该地址可用）。如果可以使用，则客户端成功获得IP地址并根据IP地址使用租期自动启动续延过程；如果DHCP客户端
*发现分配的IP地址已经被使用，则需要向DHCP服务器发出一个DHCP Decline报文，通知DHCP服务器禁用这个IP地址，此后，DHCP客户端
*重新开始上述地址申请流程。
*(7)客户端在使用租期超过50%时，会以单播形式向DHCP服务器发送DHCP Request报文来续租IP地址。如果客户端成功收到服务器返回
*的DHCP ACK报文，则DHCP Client继续使用这个IP地址。
*(8)客户端在使用租期超过87.5%时，会以广播形式向DHCP服务器发送DHCP Request报文来续租IP地址。如果客户端成功收到服务器返回
*的DHCP ACK报文，则按相应的时间延长IP地址租期；如果没有收到服务器返回的DHCP ACK报文，则客户端继续使用这个IP地址，知道IP
*地址使用租期到期。当租期到期后，DHCP客户端会向DHCP服务器发送一个DHCP Release报文来释放这个IP地址，并开始新的IP地址申请过程。
*
*最后需要指出的是，DHCP服务器发送的DHCP Offer报文中指定的IP地址不一定为最终分配给客户端的地址，通常情况下，DHCP服务器会
*保留该地址知道客户端发出REQUEST请求。在整个协商过程中，如果DHCP客户端发送的REQUEST报文中的地址不正确，如客户端已经迁移到
*新的子网或者租约已经过期，DHCP服务器会发送DHCP NAK报文给DHCP客户端，让客户端重新发起地址请求过程。
**/

/**
*在源代码dhcp.c和dhcp.h文件中，实现了DHCP客户端的所有功能。基于UDP协议来实现，从客户端的初始化代码看出，注册给UDP控制块
*的数据接收回调函数为dhcp_recv，内核将在该函数中完成所有DHCP客户端逻辑的处理
**/
#define  DHCP_CLIENT_PORT  68
#define  DHCP_SERVER_PORT  67

err_t dhcp_start(struct netif *netif)
{
	dhcp->pcb = udp_new();
	udp_bin(dhcp->pch,IP_ADDR_ANY,DHCP_CLIENT_PORT);
	udp_connect(dhcp->pcb,IP_ADDR_ANY,DHCP_SERVER_PORT);
	udp_recv(udp->pcb,dhcp_recv,netif);
	result = dhcp_discover(netif);              //广播Discover报文，开始DHCP协商流程
	......
}

//client and server之间的协商报文种类
#define  DHCP_DISCOVER  1
#define  DHCP_OFFER     2  
#define  DHCP_REQUEST   3
#define  DHCP_DECLINE   4
#define  DHCP_ACK       5
#define  DHCP_NAK       6
#define  DHCP_RELEASE   7

//DHCP协商过程中，客户端的状态逐渐转移。当从服务器收到不同的响应包时，或者在内核中有DHCP定时时间超时后，
//客户端状态都可能发送迁移，同时，客户端也可能向服务器发送某些数据包。

#define  DHCP_OFF            0       //初始状态
#define  DHCP_SELECTING      6       //已广播Discover报文
#define  DHCP_REQUESTING     1       //已发送Request报文
#define  DHCP_CHECKING       8       //检测分配的IP地址是否可用
#define  DHCP_BOUND          10      //IP地址可用，绑定到该地址
#define  DHCP_RENEWING       5       //租期的50%已到，重发Request报文
#define  DHCP_REBINDING      4       //租期的87.5%已到，重发Request报文
#define  DHCP_BACKING_OFF    12      //收到NAK


//lwipdemo.c文件做了如下更改
#include  "dhcp.h"            //包含DHCP相关的头文件
#include  "includes.h"        //操作系统相关的头文件
struct netif enc28j60_netif;  //网口接口结构

void lwip_init_task(void)
{
	struct ip_addr ipaddr, netmask, gw;
	tcpip_init(NULL,NULL);    //初始化协议栈，建立内核进程

	IP4_ADDR(&gw,0,0,0,0);    //将三个地址初始化为0
	IP4_ADDR(&ipaddr,0，0，0，0);
	IP4_ADDR(&netmask,0，0，0，0);

	//初始化网络接口，注册回调函数
	netif_add(&enc28j60_netif,&ipaddr,&netmask,&gw,NULL,ethernetif_init,tcpip_init);
	netif_set_default(&enc28j60_netif);  //设置缺省网络接口
	netif_set_up(&enc28j60_netif);       //网络接口使能

	dhcp_start(&enc28j60_netif);         //启动DHCP客户端
}

/**
*函数dhcp_start将为系统启动一个DHCP客户端，该客户端基于UDP的Raw API编程接口来实现，函数会在内核中为客户端
*申请一个UDP控制块，并将控制块和DHCP服务器(默认网关，192.168.1.1)进行绑定。当然，用户也可以调用函数dhcp_stop
*来终止这个DHCP客户端，此时网络接口的IP地址不再有效，用户必须手动为网络接口指定IP地址。
**/

/**
*基于DHCP的测试程序如下所示，给程序本质为一个基于Sequential API的TCP客户端，当从DHCP服务器处获得有效IP地址后，
*该客户端程序将于主机192.168.1.103上绑定在端口8080的服务器程序进行连接，连接成功后，客户端循环向服务器发送字符
*串信息，字符串其中包含了客户端的IP地址信息。
*
*******/

#define  MAX_BUFFER_LEN  256
char sendbuf[MAX_BUFFER_LEN];             //数据发送缓冲区

extern struct netif enc28j60_netif;       //外部定义的网络接口结构
//TCP客户端任务，连接服务器并循环发送字符串信息
static void dhcp_netconn_thread(void *arg)
{
	struct netconn *conn;
	struct ip_addr serveraddr;
	u32_t err,wr_err;
	int strlen = 0;

	while(enc28j60_netif.dhcp->state != DHCP_BOUND)    //DHCP是否获得有效IP地址
		OSTimeDly(10);                                 //等待，直到IP地址有效

	IP4_ADDR(&serveraddr,192.168.1.103);               //构造服务器IP地址

	while(1)
	{
		conn = netconn_new(NETCONN_TCP);               //申请TCP连接结构
		err = netconn_connect(conn,&serveraddr,8080);  //连接服务器，端口号为8080
	}
}