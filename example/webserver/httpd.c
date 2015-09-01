#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

#include "ofp.h"

#include "httpd.h"

void httpd_main(uint32_t addr);

#define logprint(a...) do {} while (0)
//#define logprint OFP_LOG

int sigreceived = 0;
static uint32_t myaddr;

/* Set www_dir to point to your web directory. */
static const char *www_dir = "/home/hjokinen/Dropbox/kolumbus-web";

/* Table of concurrent connections */
#define NUM_CONNECTIONS 16
static struct {
	int fd;
	uint32_t addr;
	int closed;
	FILE *post;
} connections[NUM_CONNECTIONS];

/* Sending function with some debugging. */
static int mysend(int s, char *p, int len)
{
	int n;

	while (len > 0) {
		n = ofp_send(s, p, len, 0);
		if (n < 0) {
			OFP_LOG("mysend: cannot send (%d): %s\n",
				  n, ofp_strerror(ofp_errno));
			return n;
		}
		len -= n;
		p += n;
		if (len) {
			logprint("mysend: only %d bytes sent\n", n);
		}
	}
	return len;
}

static int sendf(int fd, const char *fmt, ...)
{
	char buf[1024];
	int ret;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ret = mysend(fd, buf, n);
	return ret;
}

/* Send one file. */
static void get_file(int s, char *url)
{
	char bufo[512];
	int n, w;

	const char *mime = NULL;
	const char *p = url + 1;

	if (*p == 0)
		p = "index.html";

	char *p2 = strrchr(p, '.');
	if (p2) {
		p2++;
		if (!strcmp(p2, "html")) mime = "text/html";
		else if (!strcmp(p2, "htm")) mime = "text/html";
		else if (!strcmp(p2, "css")) mime = "text/css";
		else if (!strcmp(p2, "txt")) mime = "text/plain";
		else if (!strcmp(p2, "png")) mime = "image/png";
		else if (!strcmp(p2, "jpg")) mime = "image/jpg";
		else if (!strcmp(p2, "class")) mime = "application/x-java-applet";
		else if (!strcmp(p2, "jar")) mime = "application/java-archive";
		else if (!strcmp(p2, "pdf")) mime = "application/pdf";
		else if (!strcmp(p2, "swf")) mime = "application/x-shockwave-flash";
		else if (!strcmp(p2, "ico")) mime = "image/vnd.microsoft.icon";
		else if (!strcmp(p2, "js")) mime = "text/javascript";
	}

	snprintf(bufo, sizeof(bufo), "%s/%s", www_dir, p);
	FILE *f = fopen(bufo, "rb");

	if (!f) {
		sendf(s, "HTTP/1.0 404 NOK\r\n\r\n");
		return;
	}

	sendf(s, "HTTP/1.0 200 OK\r\n");
	if (mime)
		sendf(s, "Content-Type: %s\r\n\r\n", mime);
	else
		sendf(s, "\r\n");

	while ((n = fread(bufo, 1, sizeof(bufo), f)) > 0)
		if ((w = mysend(s, bufo, n)) < 0)
			break;
	fclose(f);
}

static int analyze_http(char *http, int s) {
	char *url;

	if (!strncmp(http, "GET ", 4)) {
		url = http + 4;
		while (*url == ' ')
			url++;
		char *p = strchr(url, ' ');
		if (p)
			*p = 0;
		else
			return -1;
		logprint("GET %s (fd=%d)\n", url, s);
		get_file(s, url);
	} else if (!strncmp(http, "POST ", 5)) {
		/* Post is not supported. */
		logprint("%s\n", http);
	}

	return 0;
}

static void *webserver(void *arg)
{
	int serv_fd, tmp_fd;
	unsigned int alen;
	struct ofp_sockaddr_in my_addr, caller;
	ofp_fd_set read_fd;

	(void)arg;

	logprint("HTTP thread started\n");

	odp_init_local(ODP_THREAD_CONTROL);
	ofp_init_local();
	sleep(1);

	myaddr = ofp_port_get_ipv4_addr(0, 0, OFP_PORTCONF_IP_TYPE_IP_ADDR);

	if ((serv_fd = ofp_socket(OFP_AF_INET, OFP_SOCK_STREAM, OFP_IPPROTO_TCP)) < 0) {
		perror("serv socket");
		logprint("Cannot open http socket!\n");
		return NULL;
	}

	memset(&my_addr, 0, sizeof(my_addr));
	my_addr.sin_family = OFP_AF_INET;
	my_addr.sin_port = odp_cpu_to_be_16(2048);
	my_addr.sin_addr.s_addr = myaddr;
	my_addr.sin_len = sizeof(my_addr);

	if (ofp_bind(serv_fd, (struct ofp_sockaddr *)&my_addr,
		       sizeof(struct ofp_sockaddr)) < 0) {
		logprint("Cannot bind http socket (%s)!\n", ofp_strerror(ofp_errno));
		return 0;
	}

	ofp_listen(serv_fd, 10);
	OFP_FD_ZERO(&read_fd);
	OFP_FD_SET(serv_fd, &read_fd);

	for ( ; ; )
	{
		int r, i;
		static char buf[1024];
		struct ofp_timeval timeout;

		timeout.tv_sec = 0;
		timeout.tv_usec = 200000;

		r = ofp_select(32, &read_fd, NULL, NULL, &timeout);
		if (r <= 0)
			continue;

		if (OFP_FD_ISSET(serv_fd, &read_fd)) {
			alen = sizeof(caller);
			if ((tmp_fd = ofp_accept(serv_fd,
						   (struct ofp_sockaddr *)&caller,
						   &alen)) > 0) {
				logprint("accept %d\n", tmp_fd);

				for (i = 0; i < NUM_CONNECTIONS; i++)
					if (connections[i].fd == 0)
						break;

				if (i >= NUM_CONNECTIONS) {
					logprint("Node cannot accept new connections!\n");
					ofp_close(tmp_fd);
					continue;
				}

#if 0
				struct ofp_linger so_linger;
				so_linger.l_onoff = 1;
				so_linger.l_linger = 0;
				int r1 = ofp_setsockopt(tmp_fd,
							  OFP_SOL_SOCKET,
							  OFP_SO_LINGER,
							  &so_linger,
							  sizeof so_linger);
				if (r1) OFP_LOG("SO_LINGER failed!\n");
#endif
				struct ofp_timeval tv;
				tv.tv_sec = 3;
				tv.tv_usec = 0;
				int r2 = ofp_setsockopt(tmp_fd,
							  OFP_SOL_SOCKET,
							  OFP_SO_SNDTIMEO,
							  &tv,
							  sizeof tv);
				if (r2) OFP_LOG("SO_SNDTIMEO failed!\n");

				connections[i].fd = tmp_fd;
				connections[i].addr = caller.sin_addr.s_addr;
				connections[i].closed = FALSE;

				OFP_FD_SET(tmp_fd, &read_fd);
			}
		}

		for (i = 0; i < NUM_CONNECTIONS; i++) {
			if (connections[i].fd == 0)
				continue;

			if (!(OFP_FD_ISSET(connections[i].fd, &read_fd)))
				continue;

			r = ofp_recv(connections[i].fd, buf, sizeof(buf)-1, 0);
			if (r > 0) {
				buf[r] = 0;
				logprint("DATA='%s'\n", buf);

				if (!strncmp(buf, "GET", 3))
					analyze_http(buf, connections[i].fd);
				else
					logprint("http req error\n");

				logprint("closing %d\n", connections[i].fd);
				OFP_FD_CLR(connections[i].fd, &read_fd);
				while (ofp_close(connections[i].fd) < 0) {
					OFP_LOG("Socket %d close err: %s\n",
						  connections[i].fd,
						  ofp_strerror(ofp_errno));
					sleep(1);
				}
				logprint("closed %d\n", connections[i].fd);
				connections[i].fd = 0;
			} else if (r == 0) {
				if (connections[i].post) {
					printf("file download finished\n");
					fclose(connections[i].post);
					connections[i].post = NULL;
				}
				ofp_close(connections[i].fd);
				OFP_FD_CLR(connections[i].fd, &read_fd);
				connections[i].fd = 0;
			}
		}
	}

	logprint("httpd exit\n");
	return NULL;
}

void ofp_start_webserver_thread(int core_id)
{
	odph_linux_pthread_t test_linux_pthread;
	odp_cpumask_t cpumask;

	odp_cpumask_zero(&cpumask);
	odp_cpumask_set(&cpumask, core_id);

	odph_linux_pthread_create(&test_linux_pthread,
				  &cpumask,
				  webserver,
				  NULL);
}
