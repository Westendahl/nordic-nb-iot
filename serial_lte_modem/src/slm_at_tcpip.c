/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */
#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <nrf_socket.h>
#include <net/socket.h>
#include "slm_at_tcpip.h"
#include "slm_at_gps.h"

LOG_MODULE_REGISTER(tcpip, CONFIG_SLM_LOG_LEVEL);

#define INVALID_SOCKET	-1
#define TCPIP_MAX_URL	128

#define AT_XSYSTEMMODE  "AT\%XSYSTEMMODE=0,1,1,0"
#define AT_XRFTEST		"AT\%XRFTEST=1,1,5,8300,14,0,3,1,0,0"
#define AT_CFUN         "AT+CFUN=1"
#define AT_CFUN0        "AT+CFUN=0"
#define AT_XBANDLOCK	"AT\%XBANDLOCK=1,\"10100\""
#define AT_CGDCONT		"AT+CGDCONT=1,\"IP\",\"iot.orange.be\""
#define AT_COPS			"AT+COPS=1,2,\"20610\""

#define AT_XSOCKET		"AT#XSOCKET=1,2"

#define AT_CEREG		"AT+CEREG?"
#define AT_CESQ			"AT+CESQ"
#define AT_NBRGRSRP		"AT\%NBRGRSRP"

static const char nb_init_at_commands[][34] = {
				// AT_CFUN0,
				// AT_XRFTEST,
				AT_XSYSTEMMODE,
				//AT_XBANDLOCK,
				AT_CFUN,
				AT_CGDCONT,
				AT_COPS
			};

// Network stats
char current_cell_id[10];
uint8_t current_rsrp;
char neighbors[100];

//extern nrf_gnss_data_frame_t gps_data;
extern struct gps_client gps_client_inst;

/*
 * Known limitation in this version
 * - Multiple concurrent sockets
 * - Socket type other than SOCK_STREAM(1) and SOCK_DGRAM(2)
 * - IP Protocol other than TCP(6) and UDP(17)
 * - TCP/UDP server mode
 * - Receive more than IPv4 MTU one-time
 * - IPv6 support
 */

/**@brief Socket operations. */
enum slm_socket_operation {
	AT_SOCKET_CLOSE,
	AT_SOCKET_OPEN
};

/**@brief List of supported AT commands. */
enum slm_tcpip_at_cmd_type {
	AT_SOCKET,
	AT_BIND,
	AT_TCP_CONNECT,
	AT_TCP_SEND,
	AT_TCP_RECV,
	AT_UDP_SENDTO,
	AT_UDP_RECVFROM,
	AT_TCPIP_MAX
};

/** forward declaration of cmd handlers **/
static int handle_at_socket(const char *at_cmd, size_t param_offset);
static int handle_at_bind(const char *at_cmd, size_t param_offset);
static int handle_at_tcp_conn(const char *at_cmd, size_t param_offset);
static int handle_at_tcp_send(const char *at_cmd, size_t param_offset);
static int handle_at_tcp_recv(const char *at_cmd, size_t param_offset);
static int handle_at_udp_sendto(const char *at_cmd, size_t param_offset);
static int handle_at_udp_recvfrom(const char *at_cmd, size_t param_offset);

/**@brief SLM AT Command list type. */
static slm_at_cmd_list_t m_at_list[AT_TCPIP_MAX] = {
	{AT_SOCKET, "AT#XSOCKET", "at#xsocket",
		handle_at_socket},
	{AT_BIND, "AT#XBIND", "at#xbind",
		handle_at_bind},
	{AT_TCP_CONNECT, "AT#XTCPCONN", "at#xtcpconn",
		handle_at_tcp_conn},
	{AT_TCP_SEND, "AT#XTCPSEND", "at#xtcpsend",
		handle_at_tcp_send},
	{AT_TCP_RECV, "AT#XTCPRECV", "at#xtcprecv",
		handle_at_tcp_recv},
	{AT_UDP_SENDTO, "AT#XUDPSENDTO", "at#xudpsendto",
		handle_at_udp_sendto},
	{AT_UDP_RECVFROM, "AT#XUDPRECVFROM", "at#xudprecvfrom",
		handle_at_udp_recvfrom},
};

static struct sockaddr_storage remote;

static struct tcpip_client {
	int sock; /* Socket descriptor. */
	enum net_ip_protocol ip_proto; /* IP protocol */
	bool connected; /* TCP connected flag */
	at_cmd_handler_t callback;
} client;

static char buf[64];

/* global variable defined in different files */
extern struct at_param_list m_param_list;

/**@brief Check whether a string has valid IPv4 address or not
 */
static bool check_for_ipv4(const char *address, u8_t length)
{
	int index;

	for (index = 0; index < length; index++) {
		char ch = *(address + index);

		if ((ch == '.') || (ch >= '0' && ch <= '9')) {
			continue;
		} else {
			return false;
		}
	}

	return true;
}

/**@brief Resolves host IPv4 address and port
 */
static int parse_host_by_ipv4(const char *ip, u16_t port)
{
	struct sockaddr_in *address4 = ((struct sockaddr_in *)&remote);

	address4->sin_family = AF_INET;
	address4->sin_port = htons(port);
	LOG_DBG("IPv4 Address %s", log_strdup(ip));
	/* NOTE inet_pton() returns 1 as success */
	if (inet_pton(AF_INET, ip, &address4->sin_addr) == 1) {
		return 0;
	} else {
		return -EINVAL;
	}
}


static int parse_host_by_name(const char *name, u16_t port, int socktype)
{
	int err;
	struct addrinfo *result;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = socktype
	};
	char ipv4_addr[NET_IPV4_ADDR_LEN];

	err = getaddrinfo(name, NULL, &hints, &result);
	if (err) {
		LOG_ERR("ERROR: getaddrinfo failed %d", err);
		return err;
	}

	if (result == NULL) {
		LOG_ERR("ERROR: Address not found\n");
		return -ENOENT;
	}

	/* IPv4 Address. */
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&remote);

	server4->sin_addr.s_addr =
		((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
	server4->sin_family = AF_INET;
	server4->sin_port = htons(port);

	inet_ntop(AF_INET, &server4->sin_addr.s_addr, ipv4_addr,
		  sizeof(ipv4_addr));
	LOG_DBG("IPv4 Address found %s\n", ipv4_addr);

	/* Free the address. */
	freeaddrinfo(result);

	return 0;
}

static int do_socket_open(u8_t type)
{
	int ret = 0;

	if (type == SOCK_STREAM) {
		client.sock = socket(AF_INET, SOCK_STREAM,
				IPPROTO_TCP);
		client.ip_proto = IPPROTO_TCP;
	} else if (type == SOCK_DGRAM) {
		client.sock = socket(AF_INET, SOCK_DGRAM,
				IPPROTO_UDP);
		client.ip_proto = IPPROTO_UDP;
	}
	if (client.sock < 0) {
		LOG_ERR("socket() failed: %d", -errno);
		sprintf(buf, "#XSOCKET: %d\r\n", -errno);
		client.callback(buf);
		client.ip_proto = IPPROTO_IP;
		ret = -errno;
	} else {
		sprintf(buf, "#XSOCKET: %d, %d\r\n", client.sock,
			client.ip_proto);
		client.callback(buf);
	}

	LOG_DBG("Socket opened");
	return ret;
}

static int do_socket_close(int error)
{
	int ret = 0;

	if (client.sock > 0) {
		ret = close(client.sock);
		if (ret < 0) {
			LOG_WRN("close() failed: %d", -errno);
			ret = -errno;
		}
		client.sock = INVALID_SOCKET;
		client.ip_proto = IPPROTO_IP;
		client.connected = false;

		sprintf(buf, "#XSOCKET: %d\r\n", error);
		client.callback(buf);
		LOG_DBG("Socket closed");
	}

	return ret;
}

static int do_bind(const char *ip, u16_t port)
{
	int ret;
	struct sockaddr_in local;

	LOG_DBG("%s:%d", log_strdup(ip), port);

	if (!check_for_ipv4(ip, strlen(ip))) {
		LOG_ERR("Not IPv4 address");
		return -EINVAL;
	}

	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	/* NOTE inet_pton() returns 1 as success */
	if (inet_pton(AF_INET, ip, &local.sin_addr) != 1) {
		LOG_ERR("Parse failed: %d", -errno);
		return -EINVAL;
	}

	ret = bind(client.sock, (struct sockaddr *)&local,
		 sizeof(struct sockaddr_in));
	if (ret < 0) {
		LOG_ERR("bind() failed: %d", -errno);
		do_socket_close(-errno);
		return -errno;
	}

	return 0;
}

static int do_tcp_connect(const char *url, u16_t port)
{
	int ret;

	LOG_DBG("%s:%d", log_strdup(url), port);

	if (check_for_ipv4(url, strlen(url))) {
		ret = parse_host_by_ipv4(url, port);
	} else {
		ret = parse_host_by_name(url, port, SOCK_STREAM);
	}
	if (ret) {
		LOG_ERR("Parse failed: %d", ret);
		return ret;
	}

	ret = connect(client.sock, (struct sockaddr *)&remote,
		 sizeof(struct sockaddr_in));
	if (ret < 0) {
		LOG_ERR("connect() failed: %d", -errno);
		do_socket_close(-errno);
		return -errno;
	}

	client.connected = true;
	client.callback("#XTCPCONN: 1\r\n");
	return 0;
}

static int do_tcp_send(const char *data)
{
	u32_t offset = 0;
	u32_t datalen = strlen(data);
	int ret;

	while (offset < datalen) {
		ret = send(client.sock, data + offset,
			   datalen - offset, 0);
		if (ret < 0) {
			do_socket_close(-errno);
			LOG_WRN("send() failed: %d", -errno);
			break;
		}

		offset += ret;
	}

	sprintf(buf, "#XTCPSEND: %d\r\n", offset);
	client.callback(buf);
	LOG_DBG("TCP sent");
	return 0;
}

static int do_tcp_receive(u16_t length, u16_t time)
{
	int ret;
	char data[NET_IPV4_MTU];
	struct timeval tmo = {
		.tv_sec = time
	};

	ret = setsockopt(client.sock, SOL_SOCKET, SO_RCVTIMEO,
			&tmo, sizeof(struct timeval));
	if (ret < 0) {
		do_socket_close(-errno);
		LOG_ERR("setsockopt() error: %d", -errno);
		return ret;
	}

	if (length > NET_IPV4_MTU) {
		ret = recv(client.sock, data, NET_IPV4_MTU, 0);
	} else {
		ret = recv(client.sock, data, length, 0);
	}
	if (ret < 0) {
		LOG_WRN("recv() error: %d", -errno);
		do_socket_close(-errno);
		ret = -errno;
	} else if (ret == 0) {
		/**
		 * When a stream socket peer has performed an orderly shutdown,
		 * the return value will be 0 (the traditional "end-of-file")
		 * The value 0 may also be returned if the requested number of
		 * bytes to receive from a stream socket was 0
		 * In both cases, treat as normal shutdown by remote
		 */
		LOG_WRN("recv() return 0");
		do_socket_close(0);
	} else {
		data[ret] = '\0';
		client.callback("#XTCPRECV: ");
		client.callback(data);
		client.callback("\r\n");
		sprintf(buf, "#XTCPRECV: %d\r\n", ret);
		client.callback(buf);
		ret = 0;
	}

	LOG_DBG("TCP received");
	return ret;
}

static int do_udp_init(const char *url, u16_t port)
{
	int ret;

	if (check_for_ipv4(url, strlen(url))) {
		ret = parse_host_by_ipv4(url, port);
	} else {
		ret = parse_host_by_name(url, port, SOCK_DGRAM);
	}
	if (ret) {
		LOG_ERR("Parse failed: %d", ret);
		return ret;
	}

	LOG_DBG("UDP initialized");
	return 0;
}

static int do_udp_sendto(const char *url, u16_t port, const char *data)
{
	u32_t offset = 0;
	u32_t datalen = strlen(data);
	int ret;

	ret = do_udp_init(url, port);
	if (ret < 0) {
		return ret;
	};

	while (offset < datalen) {
		ret = sendto(client.sock, data + offset,
			   datalen - offset, 0,
			   (struct sockaddr *)&remote,
			   sizeof(struct sockaddr_in));
		if (ret <= 0) {
			LOG_ERR("sendto() failed: %d", -errno);
			do_socket_close(-errno);
			return -errno;
		}

		offset += ret;
	}

	sprintf(buf, "#XUDPSENDTO: %d\r\n", offset);
	client.callback(buf);
	LOG_DBG("UDP sent");
	return 0;
}

static int do_udp_recvfrom(const char *url, u16_t port, u16_t length,
			u16_t time)
{
	int ret;
	char data[NET_IPV4_MTU];
	int sockaddr_len = sizeof(struct sockaddr);
	struct timeval tmo = {
		.tv_sec = time
	};

	ret = do_udp_init(url, port);
	if (ret < 0) {
		return ret;
	};

	ret = setsockopt(client.sock, SOL_SOCKET, SO_RCVTIMEO,
			&tmo, sizeof(struct timeval));
	if (ret < 0) {
		LOG_ERR("setsockopt() error: %d", -errno);
		do_socket_close(-errno);
		return ret;
	}

	if (length > NET_IPV4_MTU) {
		ret = recvfrom(client.sock, data, NET_IPV4_MTU, 0,
			(struct sockaddr *)&remote, &sockaddr_len);
	} else {
		ret = recvfrom(client.sock, data, length, 0,
			(struct sockaddr *)&remote, &sockaddr_len);
	}
	if (ret < 0) {
		LOG_WRN("recvfrom() error: %d", -errno);
		do_socket_close(-errno);
		ret = -errno;
	} else {
		/**
		 * Datagram sockets in various domains permit zero-length
		 * datagrams. When such a datagram is received, the return
		 * value is 0. Treat as normal case
		 */
		data[ret] = '\0';
		client.callback("#XUDPRECV: ");
		client.callback(data);
		client.callback("\r\n");
		sprintf(buf, "#XUDPRECV: %d\r\n", ret);
		client.callback(buf);
		ret = 0;
	}

	LOG_DBG("UDP received");
	return ret;
}

/**@brief handle AT#XSOCKET commands
 *  AT#XSOCKET=<op>[,<type>]
 *  AT#XSOCKET?
 *  AT#XSOCKET=? TEST command not supported
 */
static int handle_at_socket(const char *at_cmd, size_t param_offset)
{
	int err = -EINVAL;
	char *at_param = (char *)at_cmd + param_offset;
	u16_t op;

	if (*(at_param) == '=') {
		at_param++;
		if (*(at_param) == '?') {
			return err;
		}
		err = at_parser_params_from_str(at_cmd, NULL, &m_param_list);
		if (err < 0) {
			return err;
		};
		if (at_params_valid_count_get(&m_param_list) < 2) {
			return -EINVAL;
		}
		err = at_params_short_get(&m_param_list, 1, &op);
		if (err < 0) {
			return err;
		};
		if (op == 1) {
			u16_t type;

			if (at_params_valid_count_get(&m_param_list) < 3) {
				return -EINVAL;
			}
			err = at_params_short_get(&m_param_list, 2, &type);
			if (err < 0) {
				return err;
			};
			if (client.sock > 0) {
				LOG_WRN("Socket is already opened");
			} else {
				err = do_socket_open(type);
			}
		} else if (op == 0) {
			if (client.sock < 0) {
				LOG_WRN("Socket is not opened yet");
			} else {
				err = do_socket_close(0);
			}
		}
	} else if (*(at_param) == '?') {
		if (client.sock != INVALID_SOCKET) {
			sprintf(buf, "#XSOCKET: %d, %d\r\n", client.sock,
				client.ip_proto);
		} else {
			sprintf(buf, "#XSOCKET: 0\r\n");
		}
		client.callback(buf);
		err = 0;
	}

	return err;
}

/**@brief handle AT#XBIND commands
 *  AT#XBIND=<local_ip>,<port>
 *  AT#XBIND?
 *  AT#XBIND=? TEST command not supported
 */
static int handle_at_bind(const char *at_cmd, size_t param_offset)
{
	int err = -EINVAL;
	char *at_param = (char *)at_cmd + param_offset;
	char ip[TCPIP_MAX_URL];
	int size = TCPIP_MAX_URL;
	u16_t port;

	if (*(at_param) == '=') {
		at_param++;
		if (*(at_param) == '?') {
			return err;
		}
		err = at_parser_params_from_str(at_cmd, NULL, &m_param_list);
		if (err < 0) {
			return err;
		};
		if (at_params_valid_count_get(&m_param_list) < 3) {
			return -EINVAL;
		}
		err = at_params_string_get(&m_param_list, 1, ip, &size);
		if (err < 0) {
			return err;
		};
		ip[size] = '\0';
		err = at_params_short_get(&m_param_list, 2, &port);
		if (err < 0) {
			return err;
		};
		err = do_bind(ip, port);
	}

	return err;
}

/**@brief handle AT#XTCPCONN commands
 *  AT#XTCPCONN=<url>,<port>
 *  AT#XTCPCONN?
 *  AT#XTCPCONN=? TEST command not supported
 */
static int handle_at_tcp_conn(const char *at_cmd, size_t param_offset)
{
	int err = -EINVAL;
	char *at_param = (char *)at_cmd + param_offset;
	char url[TCPIP_MAX_URL];
	int size = TCPIP_MAX_URL;
	u16_t port;

	if (client.sock < 0) {
		LOG_ERR("Socket not opened yet");
		return err;
	}

	if (*(at_param) == '=') {
		at_param++;
		if (*(at_param) == '?') {
			return err;
		}
		err = at_parser_params_from_str(at_cmd, NULL, &m_param_list);
		if (err < 0) {
			return err;
		};
		if (at_params_valid_count_get(&m_param_list) < 3) {
			return -EINVAL;
		}
		err = at_params_string_get(&m_param_list, 1, url, &size);
		if (err < 0) {
			return err;
		};
		url[size] = '\0';
		err = at_params_short_get(&m_param_list, 2, &port);
		if (err < 0) {
			return err;
		};
		err = do_tcp_connect(url, port);
	} else if (*(at_param) == '?') {
		if (client.connected) {
			client.callback("+XTCPCONN: 1\r\n");
		} else {
			client.callback("+XTCPCONN: 0\r\n");
		}
		err = 0;
	}

	return err;
}

/**@brief handle AT#XTCPSEND commands
 *  AT#XTCPSEND=<data>
 *  AT#XTCPSEND? READ command not supported
 *  AT#XTCPSEND=? TEST command not supported
 */
static int handle_at_tcp_send(const char *at_cmd, size_t param_offset)
{
	int err = -EINVAL;
	char *at_param = (char *)at_cmd + param_offset;
	char data[NET_IPV4_MTU];
	int size = NET_IPV4_MTU;

	if (!client.connected) {
		LOG_ERR("TCP not connected yet");
		return err;
	}

	if (*(at_param) == '=') {
		at_param++;
		if (*(at_param) == '?') {
			return err;
		}
		err = at_parser_params_from_str(at_cmd, NULL, &m_param_list);
		if (err < 0) {
			return err;
		};
		if (at_params_valid_count_get(&m_param_list) < 2) {
			return -EINVAL;
		}
		err = at_params_string_get(&m_param_list, 1, data, &size);
		if (err < 0) {
			return err;
		};
		data[size] = '\0';
		err = do_tcp_send(data);
	}

	return err;
}

/**@brief handle AT#XTCPRECV commands
 *  AT#XTCPRECV=<length>
 *  AT#XTCPRECV? READ command not supported
 *  AT#XTCPRECV=? TEST command not supported
 */
static int handle_at_tcp_recv(const char *at_cmd, size_t param_offset)
{
	int err = -EINVAL;
	char *at_param = (char *)at_cmd + param_offset;
	u16_t length, time;

	if (!client.connected) {
		LOG_ERR("TCP not connected yet");
		return err;
	}

	if (*(at_param) == '=') {
		at_param++;
		if (*(at_param) == '?') {
			return err;
		}
		err = at_parser_params_from_str(at_cmd, NULL, &m_param_list);
		if (err < 0) {
			return err;
		};
		if (at_params_valid_count_get(&m_param_list) < 3) {
			return -EINVAL;
		}
		err = at_params_short_get(&m_param_list, 1, &length);
		if (err < 0) {
			return err;
		};
		err = at_params_short_get(&m_param_list, 2, &time);
		if (err < 0) {
			return err;
		};
		err = do_tcp_receive(length, time);
	}

	return err;
}

/**@brief handle AT#XUDPSENDTO commands
 *  AT#XUDPSENDTO=<url>,<port>,<data>
 *  AT#XUDPSENDTO? READ command not supported
 *  AT#XUDPSENDTO=? TEST command not supported
 */
static int handle_at_udp_sendto(const char *at_cmd, size_t param_offset)
{
	int err = -EINVAL;
	char *at_param = (char *)at_cmd + param_offset;
	char url[TCPIP_MAX_URL];
	u16_t port;
	char data[NET_IPV4_MTU];
	int size;

	if (client.sock < 0) {
		LOG_ERR("Socket not opened yet");
		return err;
	} else if (client.ip_proto != IPPROTO_UDP) {
		LOG_ERR("Invalid socket");
		return err;
	}

	if (*(at_param) == '=') {
		at_param++;
		if (*(at_param) == '?') {
			return err;
		}
		err = at_parser_params_from_str(at_cmd, NULL, &m_param_list);
		if (err < 0) {
			return err;
		};
		if (at_params_valid_count_get(&m_param_list) < 4) {
			return -EINVAL;
		}
		size = TCPIP_MAX_URL;
		err = at_params_string_get(&m_param_list, 1, url, &size);
		if (err < 0) {
			return err;
		};
		url[size] = '\0';
		err = at_params_short_get(&m_param_list, 2, &port);
		if (err < 0) {
			return err;
		};
		size = NET_IPV4_MTU;
		err = at_params_string_get(&m_param_list, 3, data, &size);
		if (err < 0) {
			return err;
		};
		data[size] = '\0';
		err = do_udp_sendto(url, port, data);
	}

	return err;
}

/**@brief handle AT#XUDPRECVFROM commands
 *  AT#XUDPRECVFROM=<url>,<port>,<length>
 *  AT#XUDPRECVFROM? READ command not supported
 *  AT#XUDPRECVFROM=? TEST command not supported
 */
static int handle_at_udp_recvfrom(const char *at_cmd, size_t param_offset)
{
	int err = -EINVAL;
	char *at_param = (char *)at_cmd + param_offset;
	char url[TCPIP_MAX_URL];
	int size = TCPIP_MAX_URL;
	u16_t port, length, time;

	if (client.sock < 0) {
		LOG_ERR("Socket not opened yet");
		return err;
	} else if (client.ip_proto != IPPROTO_UDP) {
		LOG_ERR("Invalid socket");
		return err;
	}

	if (*(at_param) == '=') {
		at_param++;
		if (*(at_param) == '?') {
			return err;
		}
		err = at_parser_params_from_str(at_cmd, NULL, &m_param_list);
		if (err < 0) {
			return err;
		};
		if (at_params_valid_count_get(&m_param_list) < 5) {
			return -EINVAL;
		}
		err = at_params_string_get(&m_param_list, 1, url, &size);
		if (err < 0) {
			return err;
		};
		url[size] = '\0';
		err = at_params_short_get(&m_param_list, 2, &port);
		if (err < 0) {
			return err;
		};
		err = at_params_short_get(&m_param_list, 3, &length);
		if (err < 0) {
			return err;
		};
		err = at_params_short_get(&m_param_list, 4, &time);
		if (err < 0) {
			return err;
		};
		err = do_udp_recvfrom(url, port, length, time);
	}

	return err;
}

/**@brief API to handle TCP/IP AT commands
 */
int slm_at_tcpip_parse(const u8_t *param, u8_t length)
{
	int ret = -ENOTSUP;

	ARG_UNUSED(length);

	for (int i = 0; i < AT_TCPIP_MAX; i++) {
		u8_t cmd_len = strlen(m_at_list[i].string_upper);

		if (strncmp(param, m_at_list[i].string_upper,
			cmd_len) == 0) {
			ret = m_at_list[i].handler(param, cmd_len);
			break;
		} else if (strncmp(param, m_at_list[i].string_lower,
			cmd_len) == 0) {
			ret = m_at_list[i].handler(param, cmd_len);
			break;
		}
	}

	return ret;
}

static int init_nb_iot_parameters(void)
{
	LOG_INF("Initializing NB-IoT Parameters");
	
	int  at_sock;
	int  bytes_sent;
	int  bytes_received;
	char buf[150];

	at_sock = socket(AF_LTE, 0, NPROTO_AT);
	if (at_sock < 0) {
		return -1;
	}

	for (int i = 0; i < ARRAY_SIZE(nb_init_at_commands); i++) {
		LOG_INF("%s",nb_init_at_commands[i]);
		bytes_sent = send(at_sock, nb_init_at_commands[i],
				  strlen(nb_init_at_commands[i]), 0);

		if (bytes_sent < 0) {
			LOG_INF("NO BYTES SENT");
			close(at_sock);
			return -1;
		}

		do {
			bytes_received = recv(at_sock, buf, 2, 0);
		} while (bytes_received == 0);

		if (memcmp(buf, "OK", 2) != 0) {
			LOG_INF("NOK");
			close(at_sock);
			return -1;
		}
		else
		{
			LOG_INF("OK");
		}
		//at_cmd_write(nb_init_at_commands[i], NULL, 0, NULL);
		
		k_sleep(K_SECONDS(3));
	}
	
	// // Keep requesting for neighboring cells until some are found.
	// int neighbors_found = 0;
	// while(neighbors_found == 0)
	// {
	// 	bytes_sent = send(at_sock, AT_NBRGRSRP, strlen(AT_NBRGRSRP), 0);
	// 	if (bytes_sent < 0) {
	// 		LOG_INF("NBRGRSRP send error");
	// 		close(at_sock);
	// 		return -1;
	// 	}
	// 	do {
	// 		bytes_received = recv(at_sock, buf, 150, 0);
	// 	} while (bytes_received == 0);
	// 	LOG_INF("NBRGRSRP RESPONSE: %s", buf);
	// 	if(strstr(buf, "NBRGRSRP:") != NULL)
	// 	{
	// 		neighbors_found = 1;
	// 	}
	// 	k_sleep(K_SECONDS(5));
	// }

	close(at_sock);
	LOG_INF("NB-IoT Parameters Initialized");

	return 0;
}

int request_nb_iot_network_stats()
{
	LOG_INF("Requesting NB-IoT network stats...");

	int  at_sock;
	int  bytes_sent;
	int  bytes_received;
	char buf[150];

	at_sock = socket(AF_LTE, 0, NPROTO_AT);
	if (at_sock < 0) {
		return -1;
	}

	// Get and parse current cell ID: AT+CEREG?
	LOG_INF("CEREG");
	bytes_sent = send(at_sock, AT_CEREG, strlen(AT_CEREG), 0);
	if (bytes_sent < 0) {
		LOG_INF("CEREG send error");
		close(at_sock);
		return -1;
	}
	do {
		bytes_received = recv(at_sock, buf, 100, 0);
	} while (bytes_received == 0);
	
	LOG_INF("CEREG RESPONSE: %s", buf); // +CEREG: 0,5,"5276","0101D268",9
	if(strstr(buf, "OK") != NULL)
	{
		char* pos = strstr(buf, "\",\"")+3;		
		for(uint8_t i=0; i<8; i++)
		{
			current_cell_id[i] = pos[i];
		}
		LOG_INF("Current cell ID = %s", current_cell_id);
	} 
	else if (strstr(buf, "ERROR") != NULL) 
	{
		LOG_ERR("Error while getting current cell ID!");
		close(at_sock);
		return -1;
	}
	k_sleep(K_SECONDS(2));

	// Get and parse current RSRP: AT+CESQ
	LOG_INF("CESQ");
	bytes_sent = send(at_sock, AT_CESQ, strlen(AT_CESQ), 0);
	if (bytes_sent < 0) {
		LOG_INF("CESQ send error");
		close(at_sock);
		return -1;
	}
	do {
		bytes_received = recv(at_sock, buf, 100, 0);
	} while (bytes_received == 0);

	LOG_INF("CESQ RESPONSE: %s", buf); // +CESQ: 99,99,255,255,17,54 \n OK		
	if(strstr(buf, "OK") != NULL)
	{
		char *pos1 = strrchr(buf, ',') + 1;
		char *pos2 = strstr(pos1, "\n");
		char rsrp[2];
		memcpy(rsrp, pos1, strlen(pos1)-strlen(pos2));
		char* ptr;
		current_rsrp = (uint8_t) strtol(rsrp, &ptr, 10);
		LOG_INF("Current RSRP = %d", current_rsrp);
	} 
	else if (strstr(buf, "ERROR") != NULL) 
	{
		LOG_ERR("Error while getting current RSRP!");
		close(at_sock);
		return -1;
	}

	k_sleep(K_SECONDS(2));

	// // Get and parse neighboring cell IDs and RSRP values: AT+NBRGRSRP
	// LOG_INF("NBRGRSRP");
	// bytes_sent = send(at_sock, AT_NBRGRSRP, strlen(AT_NBRGRSRP), 0);
	// if (bytes_sent < 0) {
	// 	LOG_INF("NBRGRSRP send error");
	// 	close(at_sock);
	// 	return -1;
	// }
	// do {
	// 	bytes_received = recv(at_sock, buf, 150, 0);
	// } while (bytes_received == 0);

	// LOG_INF("NBRGRSRP RESPONSE: %s", buf); // %NBRGRSRP: 179,6447,57,11,6447,54
	// if(strstr(buf, "OK") != NULL)
	// {
	// 	if(strstr(buf, "NBRGRSRP") != NULL)
	// 	{
	// 		char* pos1 = strstr(buf, "\%NBRGRSRP: ") + strlen("\%NBRGRSRP: ");
	// 		char* pos2 = strstr(pos1, "\n");
	// 		for(uint8_t i=0; i<strlen(pos1)-strlen(pos2); i++)
	// 		{
	// 			neighbors[i] = pos1[i];
	// 		}
	// 		LOG_INF("Neighbors = %s", neighbors);
	// 	}
	// 	else
	// 	{
	// 		LOG_INF("No neighbors found.");
	// 		neighbors[0] = '\0';
	// 	}	
	// }
	// else if (strstr(buf, "ERROR") != NULL) 
	// {
	// 	LOG_ERR("Error while getting neighbor data!");
	// 	close(at_sock);
	// 	return -1;
	// }
	
	// k_sleep(K_SECONDS(2));

	close(at_sock);
	LOG_INF("NB-IoT network stats requested.");
	
	return 0;
}

/**@brief API to initialize TCP/IP AT commands handler
 */
int slm_at_tcpip_init(at_cmd_handler_t callback)
{
	if (callback == NULL) {
		LOG_ERR("No callback");
		return -EINVAL;
	}
	client.sock = INVALID_SOCKET;
	client.connected = false;
	client.ip_proto = IPPROTO_IP;
	client.callback = callback;
	//init nb_iot module & udp socket
	init_nb_iot_parameters();
	do_socket_open(2);
	return 0;
}

void send_message(void)
{
	// Request network stats: Current and neighbor Cell ID, RSRP etc.
	int error = request_nb_iot_network_stats();
	LOG_INF("error = %d", error);
	if(error == 0)
	{
		// Put all stats in a buffer
		char payloadstring[300] = "";

		strcat(payloadstring, current_cell_id);
		strcat(payloadstring, ";");

		char* rsrp = (char*) &current_rsrp;
		strcat(payloadstring, rsrp);
		strcat(payloadstring, ";");

		if(neighbors[0] != '\0')
		{
			strcat(payloadstring, neighbors);
		}
		strcat(payloadstring, ";");
		
		LOG_INF("Payloadstring = %s", payloadstring);

		LOG_INF("GPS client running = %d", gps_client_inst.running);
		//LOG_INF("lat = %d, lon = %d", gps_data.pvt.latitude, gps_data.pvt.longitude);


		// memcpy(payloadstring, latitude, 8);

		// memcpy(payloadstring+9, longitude, strlen(payloadstring)+1);
		// memcpy(payloadstring, longitude, strlen(payloadstring)+1);


		// // Send message to UDP server
		// do_udp_sendto("nbiot.idlab.uantwerpen.be", 161, payloadstring);
		// LOG_INF("Message sent: %s", payloadstring);
	}
}
