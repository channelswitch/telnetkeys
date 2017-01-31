#include "game.h"
#include "console.h"
#include "listener.h"
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>

struct main_data {
	int epoll_fd;
	struct fd_list *fd_list;
	unsigned to_quit;
	unsigned objects_stopping;
};

struct fd_struct {
	int fd;
	int (*callback)(void *user1, unsigned revents);
	void *user1;
};


static int fd_struct(
		struct fd_struct *s,
		struct fd_struct **s_out,
		struct main_data *data,
		int fd,
		unsigned events,
		int (*callback)(void *user1, unsigned revents),
		void *user1)
{
	if(s) goto free;

	s = malloc(sizeof *s);
	if(!s) goto e_malloc;

	s->callback = callback;
	s->user1 = user1;
	s->fd = fd;

	struct epoll_event ev = {};
	ev.events =
		((events & 1) ? EPOLLIN : 0) |
		((events & 2) ? EPOLLOUT : 0) |
		((events & 4) ? EPOLLET : 0);
	ev.data.ptr = s;
	if(epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, s->fd, &ev) < 0)
		goto e_epoll_ctl;

	*s_out = s;
	return 0;

free:
	epoll_ctl(data->epoll_fd, EPOLL_CTL_DEL, s->fd, NULL);
e_epoll_ctl:
	free(s);
e_malloc:
	return -1;
}

static void *reg_fd(
		void *user,
		int fd,
		unsigned events,
		int (*f)(void *user1, unsigned revents),
		void *user1)
{
	struct fd_struct *s;
	if(fd_struct(NULL, &s, user, fd, events, f, user1) < 0) return NULL;
	return s;
}

static void unreg_fd(void *user, void *fd_ret)
{
	fd_struct(fd_ret, NULL, user, 0, 0, NULL, NULL);
}

static int event_on_fd(struct main_data *data, struct epoll_event *ev)
{
	struct fd_struct *s = ev->data.ptr;
	return s->callback(s->user1,
			(ev->events & EPOLLIN ? 1 : 0) |
			(ev->events & EPOLLOUT ? 2 : 0) |
			(ev->events & EPOLLERR ? 4 : 0));
}

static void quit_req(void *user)
{
	struct main_data *data = user;
	data->to_quit = 1;
}

static void quit_notify(void *user)
{
	struct main_data *data = user;
	--data->objects_stopping;
}

int main(int argc, char **argv)
{
	int err = 1;

	int port = argc == 2 ? atoi(argv[1]) : 23;

	struct main_data data;
	data.to_quit = 0;
	data.fd_list = NULL;

	data.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if(data.epoll_fd < 0) goto e_epoll_create;

	struct game *game;
	if(game_new(&game, reg_fd, unreg_fd, &data) < 0) goto e_game_new;

	struct listener *listener;
	if(listener_new(&listener, port, game, reg_fd, unreg_fd, &data) < 0)
		goto e_listener_new;

	struct console *console;
	if(console_new(&console, game, reg_fd, unreg_fd, quit_req, &data) < 0)
		goto e_console_new;

	while(!data.to_quit) {
		struct epoll_event ev;
		if(epoll_wait(data.epoll_fd, &ev, 1, -1) < 0) goto e_event;
		if(event_on_fd(&data, &ev) < 0) goto e_event;
	}

	data.objects_stopping = 1;
	listener_stop(listener, quit_notify);

	while(data.objects_stopping) {
		struct epoll_event ev;
		if(epoll_wait(data.epoll_fd, &ev, 1, -1) < 0) goto e_event;
		if(event_on_fd(&data, &ev) < 0) goto e_event;
	}

	err = 0;

e_event:
	console_free(console);
e_console_new:
	listener_free(listener);
e_listener_new:
	game_free(game);
e_game_new:
	close(data.epoll_fd);
e_epoll_create:
	return err;
}
