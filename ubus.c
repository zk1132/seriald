#define _GNU_SOURCE
#include <libubus.h>
#include <libubox/blobmsg_json.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "seriald.h"
#include "ubus.h"

static void seriald_ubus_send_event(const char *json);
static void ubus_send_event_line_splitter(const int n, const char *buff_rd);

static struct ubus_context *ubus_ctx = NULL;
static struct blob_buf b;
static const char *ubus_sock;

static void seriald_ubus_send_event(const char *json)
{
	blob_buf_init(&b, 0);

	if (!blobmsg_add_json_from_string(&b, json)) {
		DPRINTF("cannot parse data for ubus send event\n");
		return;
	}

	if (ubus_send_event(ubus_ctx, ubus_path, b.head)) {
		ubus_free(ubus_ctx);
		ubus_ctx = ubus_connect(ubus_sock);
		ubus_send_event(ubus_ctx, ubus_path, b.head);
	}
}

static void ubus_send_event_line_splitter(const int n, const char *buff_rd)
{
	static char buff[TTY_RD_SZ*2+1] = "";
	static int buff_len = 0;
	const char *p;

	p = buff_rd;

	while (p - buff_rd < n) {
			if (buff_len == sizeof(buff) - 1) {
				seriald_ubus_send_event(buff);
				*buff = '\0';
				buff_len = 0;
			}
			if (*p && *p != '\r' && *p != '\n') {
				buff[buff_len] = *p;
				buff[++buff_len] = '\0';
			} else if ((!*p || *p == '\n') && buff_len > 0) {
				seriald_ubus_send_event(buff);
				*buff = '\0';
				buff_len = 0;
			}
			p++;
	}
}

void seriald_ubus_run(const char *sock)
{
	fd_set rdset;
	int r;
	int n;
	char buff_rd[TTY_RD_SZ*2];
	int max_fd;
	eventfd_t efd_value;

	ubus_sock = sock;
	max_fd = (efd_signal > ubus_pipefd[0]) ? efd_signal : ubus_pipefd[0];

	ubus_ctx = ubus_connect(ubus_sock);
	if (!ubus_ctx) {
		fatal("cannot connect to ubus");
	}

	while (!sig_exit) {
		FD_ZERO(&rdset);
		FD_SET(efd_signal, &rdset);
		FD_SET(ubus_pipefd[0], &rdset);

		r = select(max_fd + 1, &rdset, NULL, NULL, NULL);
		if (r < 0)  {
			if (errno == EINTR) continue;
			else fatal("select failed: %d : %s", errno, strerror(errno));
		}

		if (FD_ISSET(ubus_pipefd[0], &rdset)) {
			/* read from pipe */
			do {
				n = read(ubus_pipefd[0], &buff_rd, sizeof(buff_rd));
			} while (n < 0 && errno == EINTR);
			if (n == 0) {
				fatal("pipe closed");
			} else if (n < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK)
					fatal("read from pipe failed: %s", strerror(errno));
			} else {
				ubus_send_event_line_splitter(n, buff_rd);
			}
		}

		if (FD_ISSET(efd_signal, &rdset)) {
			/* Being self-piped */
			if (eventfd_read(efd_signal, &efd_value)) {
				fatal("failed to read efd_signal");
			}
		}
	}
}
