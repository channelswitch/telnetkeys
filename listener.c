#include "listener.h"
#include "connection.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <unistd.h>

struct list {
	struct list **prev_p, *next;
	struct listener *l;
	struct connection *connection;
};

struct listener {
	struct game *g;
	void *(*add_fd)(
		void *user,
		int fd,
		unsigned events,
		int (*callback)(void *user1, unsigned revents),
		void *user1);
	void (*remove_fd)(void *user, void *fd_ptr);
	void *fd_ptr;
	void *user;
	void (*stop_callback)(void *user);

	int socket;

	enum {
		FD_REMOVED = 1,
	} flags;

	struct list *active_connections,
		    *stopping_connections,
		    *stopped_connections;
};

static int incoming(void *user, unsigned revents);

static void free_stopped_connections(struct listener *l);
static int listener(
		struct listener *l,
		struct listener **l_out,
		int port,
		struct game *g,
		void *(*add_fd)(
			void *user,
			int fd,
			unsigned events,
			int (*callback)(void *user1, unsigned revents),
			void *user1),
		void (*remove_fd)(void *user, void *fd_ptr),
		void *user)
{
	if(l) goto free;

	l = malloc(sizeof *l);
	if(!l) goto e_malloc;

	l->g = g;
	l->add_fd = add_fd;
	l->remove_fd = remove_fd;
	l->user = user;
	l->flags = 0;
	l->stop_callback = NULL;
	l->active_connections = NULL;
	l->stopping_connections = NULL;
	l->stopped_connections = NULL;

	/* Create a socket. */
	l->socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK
			| SOCK_CLOEXEC, 0);
	if(l->socket < 0) goto e_socket;

	/* Bind socket to port and listen to it. */
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if(bind(l->socket, (struct sockaddr *)&addr, sizeof addr) < 0) {
		fprintf(stderr, "Could not bind socket to port %d: %s\n",
				port, strerror(errno));
		goto e_bind;
	}
	if(listen(l->socket, 5) < 0) goto e_listen;

	/* Wait for it. */
	l->fd_ptr = l->add_fd(l->user, l->socket, 1, incoming, l);
	if(!l->fd_ptr) goto e_add_fd;

	*l_out = l;
	return 0;

free:
	free_stopped_connections(l);
	while(l->active_connections) {
		struct list *lst = l->active_connections;
		connection_free(lst->connection);
		l->active_connections = lst->next;
		free(lst);
	}
	while(l->stopping_connections) {
		struct list *lst = l->stopping_connections;
		connection_free(lst->connection);
		l->stopping_connections = lst->next;
		free(lst);
	}
	if(!(l->flags & FD_REMOVED)) l->remove_fd(l->user, l->fd_ptr);
e_add_fd:
e_listen:
e_bind:
	if(!(l->flags & FD_REMOVED)) close(l->socket);
e_socket:
	free(l);
e_malloc:
	return -1;
}

int listener_new(
		struct listener **listener_out,
		int port,
		struct game *g,
		void *(*add_fd)(
			void *user,
			int fd,
			unsigned events,
			int (*callback)(void *user1, unsigned revents),
			void *user1),
		void (*remove_fd)(void *user, void *fd_ptr),
		void *user)
{
	return listener(NULL, listener_out, port, g, add_fd, remove_fd, user);
}

void listener_free(struct listener *l)
{
	if(l) listener(l, NULL, 0, NULL, NULL, NULL, NULL);
}

static void connection_was_stopped(void *user)
{
	struct list *lst = user;
	struct listener *l = lst->l;

	/* Remove from whichever list we're in (should be
	 * stopping_connections). */
	if(lst->next) lst->next->prev_p = lst->prev_p;
	*lst->prev_p = lst->next;

	/* Put in stopped_connections. */
	lst->next = l->stopped_connections;
	if(lst->next) lst->next->prev_p = &lst->next;
	l->stopped_connections = lst;
	lst->prev_p = &l->stopped_connections;
}

static void free_stopped_connections(struct listener *l)
{
	while(l->stopped_connections) {
		struct list *lst = l->stopped_connections;
		*lst->prev_p = lst->next;
		if(lst->next) lst->next->prev_p = lst->prev_p;

		connection_free(lst->connection);
		free(lst);
	}
	if(!l->active_connections && !l->stopping_connections) {
		if(l->stop_callback) l->stop_callback(l->user);
	}
}

static void stop_connection(struct list *lst)
{
	struct listener *l = lst->l;

	/* Remove from current position. */
	*lst->prev_p = lst->next;
	if(lst->next) lst->next->prev_p = lst->prev_p;

	/* Prepend to stopping_connections. */
	lst->next = l->stopping_connections;
	if(lst->next) lst->next->prev_p = &lst->next;
	l->stopping_connections = lst;
	lst->prev_p = &l->stopping_connections;

	/* Stop. If callback is called immediately, everything is where
	 * it should be. */
	connection_stop(lst->connection, connection_was_stopped);

	free_stopped_connections(l);
}

void listener_stop(struct listener *l, void (*cb)(void *))
{
	if(!(l->flags & FD_REMOVED)) {
		l->remove_fd(l->user, l->fd_ptr);
		close(l->socket);
		l->flags |= FD_REMOVED;
	}

	if(!l->active_connections && !l->stopping_connections) {
		cb(l->user);
		return;
	}

	l->stop_callback = cb;
	while(l->active_connections) {
		/* Remove from active_connections. */
		stop_connection(l->active_connections);
	}
}

static void *add_fd(
		void *user,
		int fd,
		unsigned events,
		int (*callback)(void *user1, unsigned revents),
		void *user1)
{
	struct list *lst = user;
	struct listener *l = lst->l;
	return l->add_fd(l->user, fd, events, callback, user1);
}

static void remove_fd(void *user, void *fd_ptr)
{
	struct list *lst = user;
	struct listener *l = lst->l;
	l->remove_fd(l->user, fd_ptr);
}

static void stop_request(void *user)
{
	struct list *lst = user;
	stop_connection(lst);
}

static int incoming(void *user, unsigned revents)
{
	struct listener *l = user;
	struct list *lst = malloc(sizeof *lst);
	if(!lst) goto e_malloc;

	lst->l = l;
	if(connection_new(&lst->connection,
				l->g,
				l->socket,
				add_fd,
				remove_fd,
				stop_request,
				lst) < 0) goto e_connection;

	lst->prev_p = &l->active_connections;
	lst->next = l->active_connections;
	if(lst->next) lst->next->prev_p = &lst->next;
	l->active_connections = lst;

	return 0;

e_connection:
	free(lst);
e_malloc:
	return 0;
}
