/*
 * Copyright (c) 2011 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under the OpenIB.org BSD license
 * below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <_errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef _WIN32
typedef unsigned long long ssize_t;
#else
#include <sys/mman.h>
#include <strings.h>
#endif

#include <rdma/rsocket.h>
#include <rdma/rwinsock.h>

//#include <getopt.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>


#include "my_getopt.h"
#ifdef _WIN32
#include "gtod.c" // gettimeofday()
#endif		

#ifdef _WIN32
#define MSG_DONTWAIT 0x80

#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif

HANDLE file_handle;

#endif

int use_rs = 1;

#define rs_socket(f,t,p)          use_rs ? WSASocket(f,t,p,rsGetProtocolInfo(NULL),0,0) : socket(f,t,p)
#define rs_bind(s,a,l)            bind(s,a,l)
#define rs_listen(s,b)            listen(s,b)
#define rs_connect(s,a,l)         connect(s,a,l)
#define rs_accept(s,a,l)          accept(s,a,l)
#define rs_shutdown(s,h)          shutdown(s,h)
#define rs_close(s)               closesocket(s)
#define rs_recv(s,b,l,f)          recv(s,b,l,f)
#define rs_send(s,b,l,f)          send(s,b,l,f)
#define rs_recvfrom(s,b,l,f,a,al) recvfrom(s,b,l,f,a,al)
#define rs_sendto(s,b,l,f,a,al)   sendto(s,b,l,f,a,al)
#define rs_select(n,rf,wf,ef,t)	  select(n,rf,wf,ef,t)
#define rs_ioctlsocket(s,c,p)     ioctlsocket(s,c,p)
#define rs_setsockopt(s,l,n,v,ol) setsockopt(s,l,n,v,ol)
#define rs_getsockopt(s,l,n,v,ol) getsockopt(s,l,n,v,ol)

union rsocket_address {
	struct sockaddr		sa;
	struct sockaddr_in	sin;
	struct sockaddr_in6	sin6;
	struct sockaddr_storage storage;
};

static const char *port = "7427";
static char *dst_addr;
static char *dst_file;
static char *src_file;
static struct timeval start, end;
//static void buf[1024 * 1024];
static uint64_t bytes;
int fd;
void *file_addr;

enum {
	CMD_NOOP,
	CMD_OPEN,
	CMD_CLOSE,
	CMD_WRITE,
	CMD_RESP = 0x80,
};

/* TODO: handle byte swapping */
struct msg_hdr {
	uint8_t  version;
	uint8_t  command;
	uint16_t len;
	uint32_t data;
	uint64_t id;
};

struct msg_open {
	struct msg_hdr hdr;
	char path[0];
};

struct msg_write {
	struct msg_hdr hdr;
	uint64_t size;
};

static void show_perf(void)
{
	float usec;

	usec = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);

	printf("%lld bytes in %.2f seconds = %.2f Gb/sec\n",
	       (long long) bytes, usec / 1000000., (bytes * 8) / (1000. * usec));
}

static char *_ntop(union rsocket_address *rsa)
{
	static char addr[32];

	switch (rsa->sa.sa_family) {
	case AF_INET:
		inet_ntop(AF_INET, &rsa->sin.sin_addr, addr, sizeof addr);
		break;
	case AF_INET6:
		inet_ntop(AF_INET6, &rsa->sin6.sin6_addr, addr, sizeof addr);
		break;
	default:
		addr[0] = '\0';
		break;
	}

	return addr;
}

static size_t _recv(int rs, char *msg, size_t len)
{
	size_t ret, offset;

	for (offset = 0; offset < len; offset += ret) {
		ret = rs_recv(rs, msg + offset, len - offset, 0);
		if (ret <= 0)
			return ret;
	}

	return len;
}

static int msg_recv_hdr(int rs, struct msg_hdr *hdr)
{
	int ret;

	ret = _recv(rs, (char *) hdr, sizeof *hdr);
	if (ret != sizeof *hdr)
		return -1;

	if (hdr->version || hdr->len < sizeof *hdr) {
		printf("invalid version %d or length %d\n",
		       hdr->version, hdr->len);
		return -1;
	}

	return sizeof *hdr;
}

static int msg_get_resp(int rs, struct msg_hdr *msg, uint8_t cmd)
{
	int ret;

	ret = msg_recv_hdr(rs, msg);
	if (ret != sizeof *msg)
		return ret;

	if ((msg->len != sizeof *msg) || (msg->command != (cmd | CMD_RESP))) {
		printf("invalid length %d or bad command response %x:%x\n",
		       msg->len, msg->command, cmd | CMD_RESP);
		return -1;
	}

	return msg->data;
}

static void msg_send_resp(int rs, struct msg_hdr *msg, uint32_t status)
{
	struct msg_hdr resp;

	resp.version = 0;
	resp.command = msg->command | CMD_RESP;
	resp.len = sizeof resp;
	resp.data = status;
	resp.id = msg->id;
	rs_send(rs, (char *) &resp, sizeof resp, 0);
}

static int server_listen(void)
{
	struct addrinfo hints, *res;
	int ret, rs;

	memset(&hints, 0, sizeof hints);
	hints.ai_flags = RAI_PASSIVE;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

 	ret = getaddrinfo(NULL, port, &hints, &res);
	if (ret) {
		perror("getaddrinfo failed\n");
		return ret;
	}

	rs = rs_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (rs < 0) {
		perror("rsocket failed\n");
		ret = rs;
		goto free;
	}

	ret = 1;
	ret = rs_setsockopt(rs, SOL_SOCKET, SO_REUSEADDR, &ret, sizeof ret);
	if (ret) {
		perror("rsetsockopt failed");
		goto close;
	}

	ret = rs_bind(rs, res->ai_addr, res->ai_addrlen);
	if (ret) {
		perror("rbind failed");
		goto close;
	}

	ret = rs_listen(rs, 1);
	if (ret) {
		perror("rlisten failed");
		goto close;
	}

	ret = rs;
	goto free;

close:
	rs_close(rs);
free:
	freeaddrinfo(res);
	return ret;
}

static int server_open(int rs, struct msg_hdr *msg)
{
	char *path = NULL;
	int ret, len;

	printf("opening: ");
	fflush(NULL);
	if (file_addr || fd > 0) {
		printf("cannot open another file\n");
		ret = EBUSY;
		goto out;
	}

	len = msg->len - sizeof *msg;
	path = malloc(len);
	if (!path) {
		printf("cannot allocate path name\n");
		ret = ENOMEM;
		goto out;
	}

	ret = _recv(rs, path, len);
	if (ret != len) {
		printf("error receiving path\n");
		goto out;
	}

	printf("%s, ", path);
	fflush(NULL);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, msg->data);
	if (fd < 0) {
		printf("unable to open destination file\n");
		ret = errno;
	}

	ret = 0;
out:
	if (path)
		free(path);

	msg_send_resp(rs, msg, ret);
	return ret;
}

static void server_close(int rs, struct msg_hdr *msg)
{
	printf("closing...");
	fflush(NULL);
	msg_send_resp(rs, msg, 0);

	if (file_addr) {
#ifdef _WIN32
		UnmapViewOfFile(file_addr);
#else
		munmap(file_addr, bytes);
#endif
		file_addr = 0;
	}

	if (fd > 0) {
		close(fd);
		fd = 0;
	}
	printf("done\n");
}

static int server_write(int rs, struct msg_hdr *msg)
{
	size_t len;
	int ret;

	printf("transferring");
	fflush(NULL);
	if (fd <= 0) {
		printf("...file not opened\n");
		ret = EINVAL;
		goto out;
	}

	if (msg->len != sizeof(struct msg_write)) {
		printf("...invalid message length %d\n", msg->len);
		ret = EINVAL;
		goto out;
	}

	ret = _recv(rs, (char *) &bytes, sizeof bytes);
	if (ret != sizeof bytes)
		goto out;

#ifdef _WIN32 
	ret = _chsize(fd, bytes);
#else
	ret = ftruncate(fd, bytes);
#endif
	if (ret)
		goto out;

#ifdef _WIN32
	file_handle = CreateFileMapping((HANDLE)_get_osfhandle(fd), 0, PAGE_READWRITE, 0, bytes, 0);
	file_addr = MapViewOfFile(file_handle, FILE_MAP_WRITE, 0, 0, 0);
#else
	file_addr = mmap(NULL, bytes, PROT_WRITE, MAP_SHARED, fd, 0);
#endif

	if (file_addr == (void *)-1) {
		printf("...error mapping file\n");
		ret = errno;
		goto out;
	}

	printf("...%lld bytes...", (long long) bytes);
	fflush(NULL);
	len = _recv(rs, file_addr, bytes);
	if (len != bytes) {
		printf("...error receiving data\n");
		ret = (int) len;
	}
out:
	msg_send_resp(rs, msg, ret);
	return ret;
}

static void server_process(int rs)
{
	struct msg_hdr msg;
	int ret;

	do {
		ret = msg_recv_hdr(rs, &msg);
		if (ret != sizeof msg)
			break;

		switch (msg.command) {
		case CMD_OPEN:
			ret = server_open(rs, &msg);
			break;
		case CMD_CLOSE:
			server_close(rs, &msg);
			ret = 0;
			break;
		case CMD_WRITE:
			ret = server_write(rs, &msg);
			break;
		default:
			msg_send_resp(rs, &msg, EINVAL);
			ret = -1;
			break;
		}

	} while (!ret);
}

static int server_run(void)
{
	int lrs, rs;
	union rsocket_address rsa;
	socklen_t len;

	lrs = server_listen();
	if (lrs < 0)
		return lrs;

	while (1) {
		len = sizeof rsa;
		printf("waiting for connection...");
		fflush(NULL);
		rs = rs_accept(lrs, &rsa.sa, &len);

		if (rs < 0) {
			printf("accept failed.\n");
			exit(1);
		}

		printf("client: %s\n", _ntop(&rsa));
		server_process(rs);
		rs_shutdown(rs, SHUT_RDWR);
		rs_close(rs);
	}
	return 0;
}

static int client_connect(void)
{
	struct addrinfo *res;
	int ret, rs;

 	ret = getaddrinfo(dst_addr, port, NULL, &res);
	if (ret) {
		perror("getaddrinfo failed\n");
		return ret;
	}

	rs = rs_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (rs < 0) {
		perror("rsocket failed\n");
		goto free;
	}

	ret = rs_connect(rs, res->ai_addr, res->ai_addrlen);
	if (ret) {
		perror("rconnect failed\n");
		rs_close(rs);
		rs = ret;
	}

free:
	freeaddrinfo(res);
	return rs;
}

static int client_open(int rs)
{
	struct msg_open *msg;
	struct stat stats;
	uint32_t len;
	int ret;

	printf("opening...");
	fflush(NULL);
	fd = open(src_file, O_RDONLY);
	if (fd < 0)
		return fd;

	ret = fstat(fd, &stats);
	if (ret < 0)
		goto err1;

	bytes = (uint64_t) stats.st_size;
#ifdef _WIN32
	file_handle = CreateFileMapping((HANDLE)_get_osfhandle(fd), 0, PAGE_READONLY, 0, bytes, 0);
	file_addr = MapViewOfFile(file_handle, FILE_MAP_READ, 0, 0, 0);
#else
	file_addr = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
	if (file_addr == (void *) -1) {
		ret = errno;
		goto err1;
	}
#endif

	len = (((uint32_t)strlen(dst_file)) + 8) & 0xFFFFFFF8;
	msg = calloc(1, sizeof(*msg) + len);
	if (!msg) {
		ret = -1;
		goto err2;
	}

	msg->hdr.command = CMD_OPEN;
	msg->hdr.len = sizeof(*msg) + len;
	msg->hdr.data = (uint32_t) stats.st_mode;
	strcpy(msg->path, dst_file);
	ret = rs_send(rs, msg, msg->hdr.len, 0);
	if (ret != msg->hdr.len)
		goto err3;

	ret = msg_get_resp(rs, &msg->hdr, CMD_OPEN);
	if (ret)
		goto err3;

	return 0;

err3:
	free(msg);
err2:
#ifdef _WIN32
	UnmapViewOfFile(file_addr);
	CloseHandle(file_handle);
#else
	munmap(file_addr, bytes);
#endif
err1:
	close(fd);
	return ret;
}

static int client_start_write(int rs)
{
	struct msg_write msg;
	int ret;

	printf("transferring");
	fflush(NULL);
	memset(&msg, 0, sizeof msg);
	msg.hdr.command = CMD_WRITE;
	msg.hdr.len = sizeof(msg);
	msg.size = bytes;

	ret = rs_send(rs, &msg, sizeof msg, 0);
	if (ret != msg.hdr.len)
		return ret;

	return 0;
}

static int client_close(int rs)
{
	struct msg_hdr msg;
	int ret;

	printf("closing...");
	fflush(NULL);
	memset(&msg, 0, sizeof msg);
	msg.command = CMD_CLOSE;
	msg.len = sizeof msg;
	ret = rs_send(rs, (char *) &msg, msg.len, 0);
	if (ret != msg.len)
		goto out;

	ret = msg_get_resp(rs, &msg, CMD_CLOSE);
	if (ret)
		goto out;

	printf("done\n");
out:
#ifdef _WIN32
	UnmapViewOfFile(file_addr);
	CloseHandle(file_handle);
#else
	munmap(file_addr, bytes);
#endif
	close(fd);
	return ret;
}

static int client_run(void)
{
	struct msg_hdr ack;
	int ret, rs;
	size_t len;

	rs = client_connect();
	if (rs < 0)
		return rs;

	ret = client_open(rs);
	if (ret)
		goto shutdown;

	ret = client_start_write(rs);
	if (ret)
		goto close;

	printf("...");
	fflush(NULL);
	gettimeofday(&start, NULL);
	len = rs_send(rs, file_addr, bytes, 0);
	if (len == bytes)
		ret = msg_get_resp(rs, &ack, CMD_WRITE);
	else
		ret = (int) len;

	gettimeofday(&end, NULL);

close:
	client_close(rs);
shutdown:
	rs_shutdown(rs, SHUT_RDWR);
	rs_close(rs);
	if (!ret)
		show_perf();
	return ret;
}

static void show_usage(char *program)
{
	printf("usage 1: %s [options]\n", program);
	printf("\t     starts the server application\n");
	printf("\t[-p  port_number]\n");
	printf("usage 2: %s source server[:destination] [options]\n", program);
	printf("\t     source - file name and path\n");
	printf("\t     server - name or address\n");
	printf("\t     destination - file name and path\n");
	printf("\t[-p  port_number]\n");
	exit(1);
}

static void server_opts(int argc, char **argv)
{
	int op;

	while ((op = my_getopt(argc, argv, "p:r")) != -1) {
		switch (op) {
		case 'p':
			port = my_optarg;
			break;
		case 'r':
			use_rs = 1;
			break;
		default:
			show_usage(argv[0]);
		}
	}
}

static void client_opts(int argc, char **argv)
{
	int op;

	if (argc < 3)
		show_usage(argv[0]);

	src_file = argv[1];
	dst_addr = argv[2];
	dst_file = strchr(dst_addr, ':');
	if (dst_file) {
		*dst_file = '\0';
		dst_file++;
	}
	if (!dst_file)
		dst_file = src_file;

	while ((op = my_getopt(argc, argv, "p:")) != -1) {
		switch (op) {
		case 'p':
			port = my_optarg;
			break;
		default:
			show_usage(argv[0]);
		}
	}

}

int main(int argc, char **argv)
{
	int ret;

#ifdef _WIN32
	WSADATA wsaData;

	if (0 != (ret = WSAStartup(0x202, &wsaData))) {
		fprintf(stderr, "WSAStartup failed with error %d\n", ret);
		ret = -1;
		goto out;
	}
#endif

	if (argc == 1 || argv[1][0] == '-') {
		server_opts(argc, argv);
		ret = server_run();
	} else {
		client_opts(argc, argv);
		ret = client_run();
	}

#ifdef _WIN32
out:
	WSACleanup();
#endif

	return ret;
}
