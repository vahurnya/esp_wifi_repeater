#include "ds18b20.h"
#include <mem.h>
#include <os_type.h>
#include <gpio.h>
#include <osapi.h>
#include "tempsensor.h"
#include "ip_addr.h"
#include "espconn.h"

uint8_t ds18b20dev[8];
static os_timer_t ds18b20_timer;
struct espconn sensorhost;
ip_addr_t sensorhostip;
esp_tcp webtcp;
char buffer[ 2048 ];

void ICACHE_FLASH_ATTR ds18b20p2(void *arg);
void ICACHE_FLASH_ATTR search_sensor();

//  ds18b20 part 1
//  initiate a conversion
void ICACHE_FLASH_ATTR ds18b20p1(void *arg)
{
	os_timer_disarm(&ds18b20_timer);

	if (!ds18b20devfound)
	{
        search_sensor();
		if (!ds18b20devfound)
        {
//            os_printf("ds18b20: no devices found on GPIO %d. Retrying.\n",DS18B20_PIN);
            os_timer_setfn(&ds18b20_timer, (os_timer_func_t *)ds18b20p1, (void *)0);
            os_timer_arm(&ds18b20_timer, 10000, 0);
		    return;
        }
	}

	// perform the conversion
	reset();
	select(ds18b20dev);
	write(DS1820_CONVERT_T, 1); // perform temperature conversion

	// tell me when its been 750ms, please
	os_timer_setfn(&ds18b20_timer, (os_timer_func_t *)ds18b20p2, (void *)0);
	os_timer_arm(&ds18b20_timer, 750, 0);

}

void ICACHE_FLASH_ATTR data_received( void *arg, char *pdata, unsigned short len )
{
    struct espconn *conn = arg;
    espconn_disconnect( conn );
}

void ICACHE_FLASH_ATTR tcp_connected( void *arg )
{
	struct espconn *conn = arg;
	uint8_t query[128];
	uint8_t tempval[6];
	os_sprintf(tempval,"%s%d.%02d",(rr<0?"-":""),abs(rr)/10000,abs(rr)%10000/100);
	os_sprintf(query,sensor_url,tempval);
//	os_printf("%s\n\r",query);
	os_sprintf( buffer, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", query, sensor_host);
    espconn_regist_recvcb( conn, data_received );
	espconn_sent( conn, buffer, os_strlen( buffer ) );
}

void ICACHE_FLASH_ATTR dns_done( const char *name, ip_addr_t *ipaddr, void *arg )
{
	if (ipaddr!=NULL)
	{
		struct espconn *conn = arg;
		conn->type = ESPCONN_TCP;
		conn->state = ESPCONN_NONE;
		conn->proto.tcp=&webtcp;
		conn->proto.tcp->local_port = espconn_port();
		conn->proto.tcp->remote_port = 80;
		os_memcpy( conn->proto.tcp->remote_ip, &ipaddr->addr, 4 );
		espconn_regist_connectcb( conn, tcp_connected );
		espconn_connect( conn );
	}
}

// ds18b20 part 2
// conversion should be done, get the result
// report it
// check for next device, call part 1 again
// or sleep if done
void ICACHE_FLASH_ATTR ds18b20p2(void *arg)
{
	int i;
	int tries = 5;
	uint8_t data[12];
	os_timer_disarm(&ds18b20_timer);
	while(tries > 0)
	{
#ifdef DSDEBUG
		ets_uart_printf("Scratchpad: ");
#endif
		reset();
		select(ds18b20dev);
		write(DS1820_READ_SCRATCHPAD, 0); // read scratchpad

		for(i = 0; i < 9; i++)
		{
			data[i] = read();
#ifdef DSDEBUG
			ets_uart_printf("%02x ", data[i]);
#endif
		}
#ifdef DSDEBUG
		ets_uart_printf("\n");
		ets_uart_printf("crc calc=%02x read=%02x\n",crc8(data,8),data[8]);
#endif
		if(crc8(data,8) == data[8]) break;
		tries--;
	}
	uint8_t *addr = ds18b20dev;
	rr = data[1] << 8 | data[0];
	if (rr & 0x8000) rr |= 0xffff0000; // sign extend
	if (addr[0] == 0x10)
	{
		//DS18S20
		rr = rr * 10000 / 2; // each bit is 1/2th of a degree C, * 10000 just keeps us as an integer
	}
	else
	{
		//DS18B20
		rr = rr * 10000 / 16; // each bit is 1/16th of a degree C, * 10000 just keeps us as an integer
	}
#ifdef DSDEBUG
//  THIS FAILS with NEGATIVE NUMBERS
	ets_uart_printf("int reading=%d r2=%d.%04d hex=%02x%02x\n",rr,rr/10000,abs(rr)%10000,data[1],data[0]);
#endif
//	char out[50];th Ubuntu that pre-dates this, and throws a compiler error when I try to pass a float to String(). – Cerin Mar 31 at 18:03 

/*	ets_printf(out,"%02x%02x%02x%02x%02x%02x%02x%02x:%d.%04d",
					addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
					rr/10000,abs(rr)%10000);
*/
	
//	os_printf("%s\n",out);
//    ets_uart_printf("%s\n",out);
/*    os_printf("%02x%02x%02x%02x%02x%02x%02x%02x:%d.%04d\n",
					addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
					rr/10000,abs(rr)%10000);
*/

	espconn_gethostbyname(&sensorhost,sensor_host,&sensorhostip,dns_done);

    os_timer_disarm(&ds18b20_timer);
    os_timer_setfn(&ds18b20_timer, (os_timer_func_t *)ds18b20p1, (void *)0);
    os_timer_arm(&ds18b20_timer, sensor_read_interval*1000, 0);
	
}


void ICACHE_FLASH_ATTR search_sensor()
{
	int r;
	uint8_t addr[8];

	// find the 18b20s on the bus
	if (r = ds_search(addr))
    {
        os_printf("Found:%02x%02x%02x%02x%02x%02x%02x%02x\n",
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
        if (addr[0] == 0x10 || addr[0] == 0x28)
        {
            os_memcpy(ds18b20dev,addr,8);
            ds18b20devfound=TRUE;
        }
    }
    else
    {
//        os_printf("No ds18b20 was found.\r\n");    
    }   
}    

void ICACHE_FLASH_ATTR init_tempsensor()
{
    ds18b20devfound=FALSE;
    rr=0;
    ds_init();
    search_sensor();

	os_timer_disarm(&ds18b20_timer);
	os_timer_setfn(&ds18b20_timer, (os_timer_func_t *)ds18b20p1, (void *)0);
	os_timer_arm(&ds18b20_timer, 500, 0);
}

