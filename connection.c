#include "connection.h"
#include "player.h"
#define _GNU_SOURCE
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include "makejmp.h"

/* Size of stack for writer() and reader() in bytes. */
#define STACK 4096

/* Max updates we can buffer. */
#define WRITEBUF_LEN 1024

/* Make sure there is room for updates in the buffer if neccesary. */
#define RESERVED_FOR_UPDATES 256

#define READBUF_LEN 256

struct connection {
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

	void (*stop_request)(void *user);

	unsigned char *reader_stack, *writer_stack;
	jmp_buf reader, writer, main;

	int fd;
	enum {
		/* Status of the fd. Updated by fd_event and reading/writing
		 * functions within reader/writer. */
		READABLE = 1,
		WRITABLE = 2,
		/* Commands from main to reader/writer. On STOP, they shall
		 * begin the process of stopping and notify main using
		 * READER_STOPPED/WRITER_STOPPED when done. On FREE, they must
		 * exit immediately. */
		STOP = 4,
		FREE = 8,
		/* Contexts telling main of errors. */
		ERROR = 16,
		/* Status of contexts. */
		READER_STOPPED = 32,
		WRITER_STOPPED = 64,
		/* Was fd removed? */
		FD_REMOVED = 128,
		/* Have we been asked by the game to stop? */
		WANT_STOP = 256,
	} flags;

	struct player *player;

	/* Write buffer. */
	unsigned refresh_progress;
	unsigned writebuf_start, writebuf_len;
	char writebuf[WRITEBUF_LEN];
	unsigned char atomic_delimiter[(WRITEBUF_LEN + 7) / 8];

	/* Read buffer. */
	unsigned readbuf_len;
	unsigned char readbuf[READBUF_LEN];

	/* Terminal info. */
	unsigned w, h;
	unsigned cursor_x, cursor_y;
	unsigned fg, bg;

	/* Reader state */
	enum {
		TELNET_NORMAL,
		TELNET_IAC,
		TELNET_SB,
		TELNET_BYTE1,
	} telnet_state;

	enum {
		TERMINAL_NORMAL,
		TERMINAL_ESC,
		TERMINAL_1,
	} terminal_state;
};

static int fd_event(void *user, unsigned revents);
static void reader(void *user);
static void writer(void *user);

static void try_remove_fd(struct connection *c)
{
	if(c->flags & FD_REMOVED) return;
	if(c->flags & READER_STOPPED && c->flags & WRITER_STOPPED) {
		c->remove_fd(c->user, c->fd_ptr);
		c->flags |= FD_REMOVED;
		if(c->stop_callback) c->stop_callback(c->user);
	}
}

void update(void *user, unsigned n_tiles, unsigned *coords);
void refresh(void *user);
void game_stop_player(void *user);

static int connection(
		struct connection *c,
		struct connection **c_out,
		struct game *g,
		int socket,
		void *(*add_fd)(
			void *user,
			int fd,
			unsigned events,
			int (*callback)(void *user1, unsigned revents),
			void *user1),
		void (*remove_fd)(void *user, void *fd_ptr),
		void (*stop)(void *user),
		void *user)
{
	if(c) goto free;

	c = malloc(sizeof *c);
	if(!c) goto e_malloc;

	c->add_fd = add_fd;
	c->remove_fd = remove_fd;
	c->stop_request = stop;
	c->user = user;
	c->flags = READABLE | WRITABLE;

	c->writebuf_start = 0;
	c->writebuf_len = 0;

	c->readbuf_len = 0;

	c->w = 80;
	c->h = 24;
	c->cursor_x = UINT_MAX;
	c->cursor_y = UINT_MAX;
	c->fg = 7;
	c->bg = 0;

	c->refresh_progress = 0;
	c->stop_callback = NULL;

	c->telnet_state = TELNET_NORMAL;
	c->terminal_state = TERMINAL_NORMAL;

	c->fd = accept4(socket, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if(c->fd < 0) goto e_accept;

	c->fd_ptr = c->add_fd(c->user, c->fd, 7, fd_event, c);
	if(!c->fd_ptr) goto e_add_fd;

	c->reader_stack = malloc(STACK);
	if(!c->reader_stack) goto e_malloc_reader;

	c->writer_stack = malloc(STACK);
	if(!c->writer_stack) goto e_malloc_writer;

	if(player_new(&c->player, g, update, refresh,
				game_stop_player, c) < 0) goto e_player;

	makejmp(c->reader, c->reader_stack, STACK, reader, c);
	makejmp(c->writer, c->writer_stack, STACK, writer, c);

	/* Do initial setup. */
	swapjmp(c->main, c->reader);
	if(c->flags & ERROR) goto e_reader;

	swapjmp(c->main, c->writer);
	if(c->flags & ERROR) goto e_writer;

	*c_out = c;
	return 0;

free:
	c->flags |= FREE;
	while(!(c->flags & READER_STOPPED)) swapjmp(c->main, c->reader);
e_writer:
	c->flags |= FREE;
	while(!(c->flags & WRITER_STOPPED)) swapjmp(c->main, c->writer);
e_reader:
	player_free(c->player);
e_player:
	free(c->writer_stack);
e_malloc_writer:
	free(c->reader_stack);
e_malloc_reader:
	try_remove_fd(c);
e_add_fd:
	close(c->fd);
e_accept:
	free(c);
e_malloc:
	return -1;
}

int connection_new(
		struct connection **c_out,
		struct game *g,
		int socket,
		void *(*add_fd)(
			void *user,
			int fd,
			unsigned events,
			int (*callback)(void *user1, unsigned revents),
			void *user1),
		void (*remove_fd)(void *user, void *fd_ptr),
		void (*stop)(void *user),
		void *user)
{
	return connection(NULL, c_out, g, socket, add_fd, remove_fd, stop,
			user);
}

void connection_free(struct connection *c)
{
	if(c) connection(c, NULL, NULL, 0, NULL, NULL, NULL, NULL);
}

void connection_stop(struct connection *c, void (*cb)(void *))
{
	c->stop_callback = cb;
	c->flags |= STOP;
	swapjmp(c->main, c->reader);
	swapjmp(c->main, c->writer);
	try_remove_fd(c);
}

static void call_reader(struct connection *c);
static void call_writer(struct connection *c);

static int fd_event(void *user, unsigned revents)
{
	struct connection *c = user;
	unsigned old_flags = c->flags;
	if(revents & 1) c->flags |= READABLE;
	else c->flags &= ~READABLE;
	if(revents & 2) c->flags |= WRITABLE;
	else c->flags &= ~WRITABLE;

	call_reader(c);
	call_writer(c);

	try_remove_fd(c);

	int ret = c->flags & ERROR ? -1 : 0;
	if(c->flags & WANT_STOP) {
		c->flags &= ~WANT_STOP;
		c->stop_request(c->user);
	}

	return ret;
}

/*
 * Game events.
 */

void game_stop_player(void *user)
{
	struct connection *c = user;
	c->flags |= WANT_STOP;
}

static int buffer_write(struct connection *c, char *data, unsigned len);
static void clear_buffer(struct connection *c);

static int update_tile(struct connection *c, unsigned x, unsigned y)
{
	char data[32];
	unsigned len = 0;

	unsigned ch, bg, fg;
	player_get_tile(c->player, x, y, &ch, &bg, &fg);

	/* Move cursor if neccesary. */
	if(x != c->cursor_x || y != c->cursor_y) {
		len += snprintf(data + len, sizeof data - len,
				"\x1b[%d;%dH", y + 1, x + 1);
	}
	c->cursor_x = x + 1;
	c->cursor_y = y;

	/* Change background and foreground colors. */
	if(fg != c->fg || bg != c->bg) {
		len += snprintf(data + len, sizeof data - len, "\x1b[");
	}
	if(fg != c->fg) {
		len += snprintf(data + len, sizeof data - len, "%d", fg + 30);
	}
	if(fg != c->fg && bg != c->bg) {
		len += snprintf(data + len, sizeof data - len, ";");
	}
	if(bg != c->bg) {
		len += snprintf(data + len, sizeof data - len, "%d", bg + 40);
	}
	if(fg != c->fg || bg != c->bg) {
		len += snprintf(data + len, sizeof data - len, "m");
	}
	c->fg = fg;
	c->bg = bg;

	/* Write the character. */
	data[len++] = ch;

	if(buffer_write(c, data, len) < 0) return -1;

	return 0;
}

void update(void *user, unsigned n_tiles, unsigned *coords)
{
	struct connection *c = user;
	unsigned i;
	for(i = 0; i < n_tiles; ++i) {
		if(update_tile(c, coords[2 * i], coords[2 * i + 1]) < 0) {
			clear_buffer(c);
			c->refresh_progress = 0;
			break;
		}
	}
	swapjmp(c->main, c->writer);
}

void refresh(void *user)
{
	struct connection *c = user;
	clear_buffer(c);
	c->refresh_progress = 0;
	swapjmp(c->main, c->writer);
}

/*
 * Reader.
 */

static void read_terminal_char(struct connection *c, unsigned char ch)
{
	if(c->terminal_state == TERMINAL_NORMAL) {
		if(ch == 27) {
			c->terminal_state = TERMINAL_ESC;
		}
		else {
			/* Key pressed */
			player_key(c->player, ch);
		}
	}
	else if(c->terminal_state == TERMINAL_ESC) {
		/* TODO: Other char sequences? */
		c->terminal_state = TERMINAL_1;
	}
	else if(c->terminal_state == TERMINAL_1) {
		if(ch == 65) player_up(c->player);
		if(ch == 68) player_left(c->player);
		if(ch == 66) player_down(c->player);
		if(ch == 67) player_right(c->player);
		c->terminal_state = TERMINAL_NORMAL;
	}
}

static void read_telnet_char(struct connection *c, unsigned char ch)
{
	if(c->telnet_state == TELNET_NORMAL) {
		if(ch != 255) read_terminal_char(c, ch);
		else c->telnet_state = TELNET_IAC;
	}
	else if(c->telnet_state == TELNET_IAC) {
		if(ch == 255) {
			read_terminal_char(c, 255);
			c->telnet_state = TELNET_NORMAL;
		}
		else if(ch == 250) {
			c->telnet_state = TELNET_SB;
		}
		else if(ch == 251 || ch == 252 || ch == 253 || ch == 254) {
			c->telnet_state = TELNET_BYTE1;
		}
		else {
			c->telnet_state = TELNET_NORMAL;
		}
	}
	else if(c->telnet_state == TELNET_SB) {
		if(ch == 240) c->telnet_state = TELNET_NORMAL;
	}
	else if(c->telnet_state == TELNET_BYTE1) {
		c->telnet_state = TELNET_NORMAL;
	}
}

static void call_reader(struct connection *c)
{
	while(c->flags & READABLE) {
		swapjmp(c->main, c->reader);
		unsigned i;
		for(i = 0; i < c->readbuf_len; ++i) {
			read_telnet_char(c, c->readbuf[i]);
		}
		c->readbuf_len = 0;
	}
}

static void reader(void *user)
{
	struct connection *c = user;

	while(!(c->flags & FREE) && !(c->flags & STOP)) {
		while(c->flags & READABLE) {
			int status = read(c->fd, c->readbuf, sizeof c->readbuf);
			if(status < 0 && errno != EAGAIN) {
				c->flags |= ERROR;
				goto e_read;
			}
			if(status == 0 || status < 0 && errno == EAGAIN) {
				c->flags &= ~READABLE;
				break;
			}
			c->readbuf_len = status;
			swapjmp(c->reader, c->main);
		}
		swapjmp(c->reader, c->main);
	}

e_read:
	c->flags |= READER_STOPPED;
	while(1) swapjmp(c->reader, c->main);
}

/*
 * Writer.
 */

static unsigned is_atomic_delimiter(struct connection *c, unsigned byte)
{
	return (c->atomic_delimiter[byte / 8] >> (byte % 8)) & 1;
}

static void set_atomic_delimiter(struct connection *c, unsigned byte)
{
	c->atomic_delimiter[byte / 8] |= 1 << (byte % 8);
}

static void clear_atomic_delimiter(struct connection *c, unsigned byte)
{
	c->atomic_delimiter[byte / 8] &= ~(1 << (byte % 8));
}

static int buffer_write(struct connection *c, char *data, unsigned len)
{
	if(c->writebuf_len + len > WRITEBUF_LEN) return -1;
	if(!len) return 0;

	unsigned write_start = (c->writebuf_start + c->writebuf_len) %
		WRITEBUF_LEN;

	unsigned i;
	if(write_start + len < WRITEBUF_LEN) {
		/* Normal write. */
		memcpy(c->writebuf + write_start, data, len);
		set_atomic_delimiter(c, write_start);
		for(i = 1; i < len; ++i) {
			clear_atomic_delimiter(c, write_start + i);
		}
	}
	else {
		/* Crosses end of circular buffer. */
		unsigned bytes_1 = WRITEBUF_LEN - write_start;
		memcpy(c->writebuf + write_start, data, bytes_1);
		memcpy(c->writebuf, data + bytes_1, len - bytes_1);
		set_atomic_delimiter(c, write_start);
		for(i = 1; i < bytes_1; ++i) {
			clear_atomic_delimiter(c, write_start + i);
		}
		for(; i < len; ++i) {
			clear_atomic_delimiter(c, i - bytes_1);
		}
	}
	c->writebuf_len += len;
	return 0;
}

/* Remove all writes in the buffer that we can without breaking atomicity. */
static void clear_buffer(struct connection *c)
{
	unsigned i;
	for(i = 0; i < c->writebuf_len; ++i) {
		unsigned pos = c->writebuf_start + i;
		if(pos >= WRITEBUF_LEN) pos -= WRITEBUF_LEN;
		if(is_atomic_delimiter(c, pos)) break;
	}
	c->writebuf_len = i;
	if(!c->writebuf_len) c->writebuf_start = 0;
}

/* Write as much as we can from the circular buffer without blocking. */
static int nonblocking_write(struct connection *c)
{
	if(!(c->flags & WRITABLE)) return 0;

	if(c->writebuf_start + c->writebuf_len > WRITEBUF_LEN) {
		/* Two parts */
		unsigned bytes_1 = WRITEBUF_LEN - c->writebuf_start;
		int status = write(c->fd, c->writebuf + c->writebuf_start,
				bytes_1);
		if(status < 0 && errno != EAGAIN) {
			goto error;
		}
		else if(status == WRITEBUF_LEN -
				c->writebuf_start) {
			/* Full write. */
			c->writebuf_start = 0;
			c->writebuf_len -= bytes_1;
			status = write(c->fd, c->writebuf, c->writebuf_len);
			if(status < 0 && errno != EAGAIN) {
				goto error;
			}
			else if(status >= 0) {
				c->writebuf_start = status;
				c->writebuf_len = c->writebuf_len - status;
			}
		}
		else {
			c->writebuf_start += status;
			c->writebuf_len -= status;
		}
	}
	else {
		/* Buffer is one part. */
		int status = write(c->fd, c->writebuf + c->writebuf_start,
				c->writebuf_len);
		if(status < 0 && errno != EAGAIN) {
			goto error;
		}
		else if(status >= 0) {
			c->writebuf_start += status;
			c->writebuf_len -= status;
		}
	}
	if(!c->writebuf_len) c->writebuf_start = 0;
	return 0;

error:
	c->flags |= ERROR;
	return -1;
}

static void write_level(struct connection *c)
{
	while(1) {
		if(c->refresh_progress == c->w * c->h) return;
		if((WRITEBUF_LEN - c->writebuf_len) < RESERVED_FOR_UPDATES)
			return;
		if(update_tile(c, c->refresh_progress % c->w,
					c->refresh_progress / c->w) < 0)
			return;
		++c->refresh_progress;
	}
}

static void call_writer(struct connection *c)
{
	swapjmp(c->main, c->writer);
}

static void writer(void *user)
{
	struct connection *c = user;

	/* To set up telnet and the terminal. */
	static unsigned char setup[] = {
		255, 253, 34,
		255, 250, 34, 1, 0, 255, 240,
		255, 251, 1,
		255, 251, 3,
		27, '[', '?', '4', '7', 'h',
		27, '[', '?', '2', '5', 'l',
		/* Enable S8C1T: 27, ' ', 'G' */
		27, '[', '?', '1', 'h',
	};
	/* To reset the terminal. */
	static unsigned char finish[] = {
		27, '[', '3', '7', ';', '4', '0', 'm',
		27, '[', '?', '2', '5', 'h',
		27, '[', '?', '1', '0', '4', '7', 'l',
	};

	if(buffer_write(c, setup, sizeof setup) < 0) {
		c->flags |= ERROR;
		goto e_write;
	}

	while(1) {
		while(1) {
			/* Add as much of the level to the write buffer as
			 * possible. */
			write_level(c);

			/* Nothing to do? */
			if(!c->writebuf_len) break;

			/* Write as much of the write buffer as possible. */
			if(nonblocking_write(c) < 0) goto e_write;

			/* Not writable? */
			if(!(c->flags & WRITABLE)) break;
		}

		/* Wait for writability or for something else to happen. */
		swapjmp(c->writer, c->main);
		if(c->flags & FREE) goto free;
		if(c->flags & STOP) break;
	}

	/* Disconnect. */
	clear_buffer(c);
	if(buffer_write(c, finish, sizeof finish) < 0) goto e_write;
	while(1) {
		if(nonblocking_write(c) < 0) goto e_write;
		if(!c->writebuf_len) break;

		/* Wait for write to finish or something else to happen. */
		swapjmp(c->writer, c->main);
		if(c->flags & FREE) goto free;
	}
free:
e_write:
	c->flags |= WRITER_STOPPED;
	while(1) swapjmp(c->writer, c->main);
}


