#include "commands.h"
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/epoll.h>

#define MAX_LEN 499

struct commands {
	int epoll, fd, old_flags;
	unsigned stopping;
	unsigned len;
	char buf[MAX_LEN + 1];
};

static int commands(struct commands *c, struct commands **c_out, int fd)
{
	if(c) goto free;

	c = malloc(sizeof *c);
	if(!c) goto err0;

	c->epoll = epoll_create1(EPOLL_CLOEXEC);
	if(c < 0) goto err1;

	c->fd = fd;
	c->len = 0;
	c->stopping = 0;

	c->old_flags = fcntl(c->fd, F_GETFL, 0);
	if(c->old_flags < 0) goto err2;
	if(fcntl(c->fd, F_SETFL, c->old_flags | O_NONBLOCK) < 0) goto err2;

	struct epoll_event ev;
	ev.events = EPOLLIN;
	if(epoll_ctl(c->epoll, EPOLL_CTL_ADD, c->fd, &ev) < 0) goto err3;

	*c_out = c;
	return 0;

free:	if(!c->stopping) epoll_ctl(c->epoll, EPOLL_CTL_DEL, c->fd, NULL);
err3:	fcntl(c->fd, F_SETFL, c->old_flags);
err2:	close(c->epoll);
err1:	free(c);
err0:	return -1;
}

int commands_new(struct commands **c_out, int fd)
{
	return commands(NULL, c_out, fd);
}

void commands_free(struct commands *c)
{
	if(c) commands(c, NULL, 0);
}

int commands_get_fd(struct commands *c)
{
	return c->epoll;
}

static int quit_command(
		struct commands *c,
		char *command,
		struct commands_event *ev_out,
		int *ret)
{
	if(!strcmp(command, "quit")) {
		ev_out->type = COMMANDS_QUIT;

		*ret = 1;
		return 1;
	}
	return 0;
}

static int load_command(
		struct commands *c,
		char *command,
		struct commands_event *ev_out,
		int *ret)
{
	if(!strncmp(command, "load ", 5)) {
		ev_out->type = COMMANDS_LOAD;
		ev_out->load.level = command + 5;

		*ret = 1;
		return 1;
	}
	return 0;
}

static void unknown_command(
		struct commands *c,
		char *command,
		struct commands_event *ev_out,
		int *ret)
{
	printf("Invalid command: \"%s\".\n", command);
	*ret = 0;
}

static int run_command(
		struct commands *c,
		char *command,
		struct commands_event *ev_out)
{
	int ret;
	if(!quit_command(c, command, ev_out, &ret)
			&& !load_command(c, command, ev_out, &ret))
		unknown_command(c, command, ev_out, &ret);
	return ret;
}

int commands_event(struct commands *c, struct commands_event *ev_out)
{
	if(c->stopping) return -2;
	char chr;
	int status = read(c->fd, &chr, 1);
	if(status < 0) return -1;
	if(c->len == MAX_LEN + 1) {
		if(chr == '\n') {
			c->len = 0;
		}
	}
	else {
		if(chr == '\n') {
			c->buf[c->len] = '\0';
			int status = run_command(c, c->buf, ev_out);
			c->len = 0;
			return status;
		}
		else {
			c->buf[c->len++] = chr;
		}
	}
	return 0;
}

int commands_stop(struct commands *c)
{
	c->stopping = 1;
	epoll_ctl(c->epoll, EPOLL_CTL_DEL, c->fd, NULL);
	return -2;
}
