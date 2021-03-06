#include <os/rtt/rtthread.h>

#include <net/lwip/opt.h>
#include <net/lwip/def.h>
#include <net/lwip/mem.h>
#include <net/lwip/pbuf.h>
#include <net/lwip/sys.h>
#include <net/lwip/netif.h>
#include <net/lwip/stats.h>
#include <net/lwip/tcpip.h>

#include <net/lwip/netif/etharp.h>
#include <net/lwip/netif/ethernetif.h>

#define ETH_RX_THREAD_ENABLE			1
#define ETH_TX_THREAD_ENABLE			0

/* eth rx/tx thread */
#ifndef TCPPS_ETH_PRIORITY
#define RT_ETHERNETIF_THREAD_PREORITY	0x90
#else
#define RT_ETHERNETIF_THREAD_PREORITY	TCPPS_ETH_PRIORITY
#endif

#if ETH_RX_THREAD_ENABLE
static struct rt_mailbox eth_rx_thread_mb;
static struct rt_thread eth_rx_thread;
#ifndef TCPPS_ETH_PRIORITY
static char eth_rx_thread_mb_pool[48 * 4];
static char eth_rx_thread_stack[1024];
#else
static char eth_rx_thread_mb_pool[TCPPS_ETH_MBOX_SIZE * 4];
static char eth_rx_thread_stack[TCPPS_ETH_STACKSIZE];
#endif
#endif

#if ETH_TX_THREAD_ENABLE
struct eth_tx_msg
{
	struct netif 	*netif;
	struct pbuf 	*buf;
};
static struct rt_mailbox eth_tx_thread_mb;
static struct rt_thread eth_tx_thread;
#ifndef TCPPS_ETH_PRIORITY
static char eth_tx_thread_mb_pool[32 * 4];
static char eth_tx_thread_stack[512];
#else
static char eth_tx_thread_mb_pool[TCPPS_ETH_MBOX_SIZE * 4];
static char eth_tx_thread_stack[TCPPS_ETH_STACKSIZE];
#endif
#endif

/* the interface provided to LwIP */
err_t eth_init(struct netif *netif)
{
	return ERR_OK;
}

err_t eth_input(struct pbuf *p, struct netif *inp)
{
	struct eth_hdr *ethhdr;

	if(p != RT_NULL)
	{
#ifdef LINK_STATS
		LINK_STATS_INC(link.recv);
#endif /* LINK_STATS */

		ethhdr = p->payload;

		switch(htons(ethhdr->type))
		{
		case ETHTYPE_IP:
			etharp_ip_input(inp, p);
			pbuf_header(p, -((rt_int16_t)sizeof(struct eth_hdr)));
			if (tcpip_input(p, inp) != ERR_OK)
			{
				/* discard packet */
				pbuf_free(p);
			}
			break;

		case ETHTYPE_ARP:
			etharp_arp_input(inp, (struct eth_addr *)inp->hwaddr, p);
			break;

		default:
			pbuf_free(p);
			p = RT_NULL;
			break;
		}
	}

	return ERR_OK;
}

err_t ethernetif_output(struct netif *netif, struct pbuf *p, struct ip_addr *ipaddr)
{
	return etharp_output(netif, p, ipaddr);
}

err_t ethernetif_linkoutput(struct netif *netif, struct pbuf *p)
{
#if ETH_TX_THREAD_ENABLE
	struct eth_tx_msg msg;
#endif
	struct eth_device* enetif;

	enetif = (struct eth_device*)netif->state;

#if ETH_TX_THREAD_ENABLE
	/* send a message to eth tx thread */
	msg.netif = netif;
	msg.buf   = p;
	rt_mb_send(&eth_tx_thread_mb, (rt_uint32_t) &msg);

	/* waiting for ack */
	rt_sem_take(&(enetif->tx_ack), RT_WAITING_FOREVER);
#else
	if (enetif != RT_NULL)
	{
		/* call driver's interface */
		if (enetif->eth_tx(&(enetif->parent), p) != RT_EOK)
		{
			rt_kprintf("transmit eth packet failed\n");
		}
	}

#endif

	return ERR_OK;
}

/* ethernetif APIs */
rt_err_t eth_device_init(struct eth_device* dev, const char* name)
{
	struct netif* netif;

	netif = (struct netif*) rt_malloc (sizeof(struct netif));
	if (netif == RT_NULL)
	{
		rt_kprintf("malloc netif failed\n");
		return -RT_ERROR;
	}
	rt_memset(netif, 0, sizeof(struct netif));

	/* set netif */
	dev->netif = netif;
	/* register to rt-thread device manager */
	rt_device_register(&(dev->parent), name, RT_DEVICE_FLAG_RDWR);
	dev->parent.type = RT_Device_Class_NetIf;
	rt_sem_init(&(dev->tx_ack), name, 0, RT_IPC_FLAG_FIFO);

	/* set name */
	netif->name[0] = name[0];
	netif->name[1] = name[1];

	/* set hw address to 6 */
	netif->hwaddr_len	= 6;
	/* maximum transfer unit */
	netif->mtu			= ETHERNET_MTU;
	/* broadcast capability */
	netif->flags		= NETIF_FLAG_BROADCAST;

	/* get hardware address */
	rt_device_control(&(dev->parent), NIOCTL_GADDR, netif->hwaddr);

	/* set output */
	netif->output		= ethernetif_output;
	netif->linkoutput	= ethernetif_linkoutput;

	/* add netif to lwip */
	if (netif_add(netif, IP_ADDR_ANY, IP_ADDR_BROADCAST, IP_ADDR_ANY, dev,
		eth_init, eth_input) == RT_NULL)
	{
		/* failed, unregister device and free netif */
		rt_device_unregister(&(dev->parent));
		rt_free(netif);
		return -RT_ERROR;
	}

	netif_set_default(netif);

	return RT_EOK;
}

#if ETH_TX_THREAD_ENABLE
/* ethernet buffer */
void eth_tx_thread_entry(void* parameter)
{
	struct eth_tx_msg* msg;

	while (1)
	{
		if (rt_mb_recv(&eth_tx_thread_mb, (rt_uint32_t*)&msg, RT_WAITING_FOREVER) == RT_EOK)
 		{
			struct eth_device* enetif;

			RT_ASSERT(msg->netif != RT_NULL);
			RT_ASSERT(msg->buf   != RT_NULL);

			enetif = (struct eth_device*)msg->netif->state;
			if (enetif != RT_NULL)
			{
				/* call driver's interface */
				if (enetif->eth_tx(&(enetif->parent), msg->buf) != RT_EOK)
				{
					rt_kprintf("transmit eth packet failed\n");
				}
			}

			/* send ack */
			rt_sem_release(&(enetif->tx_ack));
		}
	}
}
#endif

#if ETH_RX_THREAD_ENABLE
/* ethernet buffer */
void eth_rx_thread_entry(void* parameter)
{
	struct eth_device* device;

	while (1)
	{
		if (rt_mb_recv(&eth_rx_thread_mb, (rt_uint32_t*)&device, RT_WAITING_FOREVER) == RT_EOK)
 		{
			struct pbuf *p;

			/* receive all of buffer */
			while (1)
			{
				p = device->eth_rx(&(device->parent));
				if (p != RT_NULL)
				{
					/* notify to upper layer */
					eth_input(p, device->netif);
				}
				else break;
			}
		}
	}
}
#endif

rt_err_t eth_device_ready(struct eth_device* dev)
{
#if ETH_RX_THREAD_ENABLE
	/* post message to ethernet thread */
	return rt_mb_send(&eth_rx_thread_mb, (rt_uint32_t)dev);
#else
	struct pbuf *p;

	/* receive all of buffer */
	while (1)
	{
		p = dev->eth_rx(&(dev->parent));
		if (p != RT_NULL)
		{
			/* notify to upper layer */
			eth_input(p, dev->netif);
		}
		else break;
	}
#endif
}

rt_err_t eth_system_device_init()
{
	rt_err_t result = RT_EOK;

#if ETH_RX_THREAD_ENABLE
	/* init rx thread */
	/* init mailbox and create ethernet thread */
	result = rt_mb_init(&eth_rx_thread_mb, "erxmb",
		&eth_rx_thread_mb_pool[0], sizeof(eth_rx_thread_mb_pool)/4,
		RT_IPC_FLAG_FIFO);
	RT_ASSERT(result == RT_EOK);

	result = rt_thread_init(&eth_rx_thread, "erx", eth_rx_thread_entry, RT_NULL,
		&eth_rx_thread_stack[0], sizeof(eth_rx_thread_stack),
		RT_ETHERNETIF_THREAD_PREORITY, 16);
	RT_ASSERT(result == RT_EOK);

	result = rt_thread_startup(&eth_rx_thread);
	RT_ASSERT(result == RT_EOK);
#endif

#if ETH_TX_THREAD_ENABLE
	/* init tx thread */
	/* init mailbox and create ethernet thread */
	result = rt_mb_init(&eth_tx_thread_mb, "etxmb",
		&eth_tx_thread_mb_pool[0], sizeof(eth_tx_thread_mb_pool)/4,
		RT_IPC_FLAG_FIFO);
	RT_ASSERT(result == RT_EOK);

	result = rt_thread_init(&eth_tx_thread, "etx", eth_tx_thread_entry, RT_NULL,
		&eth_tx_thread_stack[0], sizeof(eth_tx_thread_stack),
		RT_ETHERNETIF_THREAD_PREORITY, 16);
	RT_ASSERT(result == RT_EOK);

	result = rt_thread_startup(&eth_tx_thread);
	RT_ASSERT(result == RT_EOK);
#endif

	return result;
}
							  
#ifdef RT_USING_FINSH
#include <hi/finsh/finsh.h>
void set_if(char* ip_addr, char* gw_addr, char* nm_addr)
{
	struct ip_addr *ip;
	struct in_addr addr;

	ip = (struct ip_addr *)&addr;

	/* set ip address */
	if ((ip_addr != RT_NULL) && inet_aton(ip_addr, &addr))
	{
		netif_set_ipaddr(netif_default, ip);
	}

	/* set gateway address */
	if ((gw_addr != RT_NULL) && inet_aton(gw_addr, &addr))
	{
		netif_set_gw(netif_default, ip);
	}

	/* set netmask address */
	if ((nm_addr != RT_NULL) && inet_aton(nm_addr, &addr))
	{
		netif_set_netmask(netif_default, ip);
	}
}
FINSH_FUNCTION_EXPORT(set_if, set network interface address);

#if LWIP_DNS
#include <net/lwip/dns.h>
void set_dns(char* dns_server)
{
	struct in_addr addr;
	
	if ((dns_server != RT_NULL) && inet_aton(dns_server, &addr))
	{
		dns_setserver(0, (struct ip_addr *)&addr);
	}
}
FINSH_FUNCTION_EXPORT(set_dns, set DNS server address);
#endif

void list_if()
{
	rt_kprintf("Default network interface: %c%c\n", netif_default->name[0], netif_default->name[1]);
	rt_kprintf("ip address: %s\n", inet_ntoa(*((struct in_addr*)&(netif_default->ip_addr))));
	rt_kprintf("gw address: %s\n", inet_ntoa(*((struct in_addr*)&(netif_default->gw))));
	rt_kprintf("net mask  : %s\n", inet_ntoa(*((struct in_addr*)&(netif_default->netmask))));

#if LWIP_DNS
	{
		struct ip_addr ip_addr;

		ip_addr = dns_getserver(0);
		rt_kprintf("dns server: %s\n", inet_ntoa(*((struct in_addr*)&(ip_addr))));
	}
#endif
}
FINSH_FUNCTION_EXPORT(list_if, list network interface information);
#endif
