#include "console.h"
#include "game.h"
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

#define STDIN 0
#define MAX_LEN 1000

static int read_nonblocking(int fd, char *buf, size_t sz);
static int stdin_readable(void *user, unsigned revents);

struct console {
	void *(*add_fd)(
		void *user,
		int fd,
		unsigned events,
		int (*callback)(void *user1, unsigned revents),
		void *user1);
	void (*remove_fd)(void *user, void *fd_ptr);
	void (*quit_f)(void *);
	void *fd_ptr;
	void *user;

	struct game *g;

	unsigned old_flags;
	unsigned waiting_on_stdin;

	unsigned buf_len;
	char buf[MAX_LEN];
};

static int console(
		struct console *c,
		struct console **c_out,
		struct game *g,
		void *(*add_fd)(
			void *user,
			int fd,
			unsigned events,
			int (*callback)(void *user1, unsigned revents),
			void *user1),
		void (*remove_fd)(void *user, void *fd_ptr),
		void (*quit_f)(void *),
		void *user)
{
	if(c) goto free;

	c = malloc(sizeof *c);
	if(!c) goto e_malloc;

	c->add_fd = add_fd;
	c->remove_fd = remove_fd;
	c->user = user;
	c->g = g;
	c->buf_len = 0;
	c->quit_f = quit_f;

	c->old_flags = fcntl(STDIN, F_GETFL, 0);
	if(c->old_flags < 0) goto e_fcntl;
	if(fcntl(STDIN, F_SETFL, c->old_flags | O_NONBLOCK) < 0) goto e_fcntl;

	c->fd_ptr = c->add_fd(c->user, STDIN, 1, stdin_readable, c);
	if(!c->fd_ptr) goto e_add_fd;
	c->waiting_on_stdin = 1;

	*c_out = c;
	return 0;

free:
	if(c->waiting_on_stdin) c->remove_fd(c->user, c->fd_ptr);
e_add_fd:
	fcntl(STDIN, F_SETFL, c->old_flags);
e_fcntl:
	free(c);
e_malloc:
	return -1;
}

int console_new(
		struct console **console_out,
		struct game *g,
		void *(*add_fd)(
			void *user,
			int fd,
			unsigned events,
			int (*callback)(void *user1, unsigned revents),
			void *user1),
		void (*remove_fd)(void *user, void *fd_ptr),
		void (*quit_f)(void *),
		void *user)
{
	return console(NULL, console_out, g, add_fd, remove_fd, quit_f, user);
}

void console_free(struct console *c)
{
	if(c) console(c, NULL, NULL, NULL, NULL, NULL, NULL);
}

static unsigned one_argument(char *str, char *word, char **arg_out)
{
	char *space = strchr(str, ' ');
	if(!space) return 0;

	unsigned i;
	for(i = 0; i < space - str; ++i) {
		if(str[i] != word[i]) return 0;
	}

	*arg_out = space + 1;
	return 1;
}

static void run_command(struct console *c, char *str)
{
	char *arg1;
	if(!strcmp(str, "")) {
		/* Do nothing */
	}
	else if(!strcmp(str, "quit")) {
		c->quit_f(c->user);
	}
	else if(one_argument(str, "load", &arg1)) {
		if(game_load(c->g, arg1) < 0) {
			printf("Could not load \"%s\".\n", arg1);
		}
		else {
			printf("Loaded \"%s\".\n", arg1);
		}
	}
	else if(!strcmp(str, "help")) {
		printf("The following commands are supported:\n"
				"  help       Print this text.\n"
				"  quit       Stop the server and exit.\n"
				"  load FILE  Load a set of levels.\n");
	}
	else {
		printf("Invalid command. Write \"help\" for help.\n", str);
	}
}

static void input_char(struct console *c, char input)
{
	if(c->buf_len == MAX_LEN) {
		if(input == '\n') {
			printf("Command too long.\n");
			c->buf_len = 0;
		}
	}
	else if(input == '\n') {
		c->buf[c->buf_len] = '\0';
		run_command(c, c->buf);
		c->buf_len = 0;
	}
	else {
		c->buf[c->buf_len++] = input;
	}
}


static int stdin_readable(void *user, unsigned revents)
{
	struct console *c = user;
	char read_buf[1024];
	int status;
read_more:
	status = read_nonblocking(STDIN, read_buf, sizeof read_buf);
	if(status < 0 && errno == EINTR) goto read_more;
	if(status < 0) return -1;
	if(status == 0) run_command(c, "quit");
	unsigned i;
	for(i = 0; i < status; ++i) input_char(c, read_buf[i]);
	if(status == sizeof read_buf) goto read_more;
	return 0;
}

static int read_nonblocking(int fd, char *buf, size_t sz)
{
	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	int status = read(fd, buf, sz);
	fcntl(fd, F_SETFL, flags);
	return status;
}
