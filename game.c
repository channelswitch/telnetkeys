#include "player.h"
#include "game.h"
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/timerfd.h>

struct object;
struct pusher;

static void refresh_all(struct game *g);
static void update_coords_all(struct game *g, unsigned n, unsigned *coords);
static void object_free(struct object *o);
static void free_level(struct game *g);
static int timer_f(void *user1, unsigned revents);
static int add_player_to_game(struct player *p);
static int pusher_new(struct pusher **p_out, struct game *g, int x, int y,
		char type);

#define MAX_PLAYERS 16
#define MAX_INVALID 16

#define SLIDE_TIME_NSEC 50000000

enum tile_flags {
	HAS_OBJECT = 1,
};

struct tile {
	unsigned char flags;
	char base;
};

struct object;
struct class {
	/* Object is removed from game. No other callbacks will be called after
	 * this. */
	void (*remove)(struct object *o);
	/* Object is pushed in direction. Returns 1 if square can be entered
	 * by another object after. */
	unsigned (*push)(struct object *o, struct object *o1, int dx, int dy,
			unsigned strength);
	/* Called when another object will share a square with object. */
	void (*enter)(struct object *o, struct object *other, int dx, int dy);
	/* Called when another object will stop sharing a square with object. */
	void (*leave)(struct object *o, struct object *other);
	/* We want the object to have a key. */
	void (*give_key)(struct object *o, char key);
	/* Does the object have this key? */
	unsigned (*has_key)(struct object *o, char key);
	/* Tell object to start sliding in this direction. */
	void (*slip)(struct object *o, int dx, int dy);
	/* Draw the object. */
	void (*draw)(struct object *o, unsigned *ch_out, unsigned *fg_out,
			unsigned *bg_out);
};

struct object {
	struct game *g;
	struct class *class;
	void *user;
	unsigned x, y, z;
};

struct game {
	void *(*add_fd)(
		void *user,
		int fd,
		unsigned events,
		int (*callback)(void *user1, unsigned revents),
		void *user1);
	void (*remove_fd)(void *user, void *fd_ptr);
	void *user;

	enum {
		GAME_NONE,
		GAME_READYING,
		GAME_PLAYING,
		GAME_FINISHED,
	} state;

	unsigned countdown;
	int timer_fd;
	void *timer_fd_ptr;

	struct player *players[MAX_PLAYERS];
	struct {
		unsigned x, y;
	} start_pos[MAX_PLAYERS];

	int level_fd;
	off_t level_offset;

	enum {
		LEVEL_LOADING = 1,
	} level_flags;

	unsigned w, h;
	struct tile *level;

	unsigned n_objects;
	struct object **objects;

	unsigned n_invalid_coords;
	unsigned invalid_coords[2 * MAX_INVALID];
};

static int game(
		struct game *g,
		struct game **g_out,
		void *(*add_fd)(
			void *user,
			int fd,
			unsigned events,
			int (*callback)(void *user1, unsigned revents),
			void *user1),
		void (*remove_fd)(void *user, void *fd_ptr),
		void *user)
{
	if(g) goto free;

	g = malloc(sizeof *g);
	if(!g) goto e_malloc;

	g->add_fd = add_fd;
	g->remove_fd = remove_fd;
	g->user = user;
	g->state = GAME_NONE;

	g->w = 0;
	g->h = 0;
	g->level = NULL;
	g->n_objects = 0;
	g->objects = NULL;
	g->countdown = 0;
	g->n_invalid_coords = 0;
	g->level_flags = 0;

	unsigned i;
	for(i = 0; i < MAX_PLAYERS; ++i) {
		g->players[i] = NULL;
		g->start_pos[i].x = UINT_MAX;
		g->start_pos[i].y = UINT_MAX;
	}

	g->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if(g->timer_fd < 0) goto e_timerfd;

	g->timer_fd_ptr = g->add_fd(g->user, g->timer_fd, 1, timer_f, g);
	if(!g->timer_fd_ptr) goto e_timer_fd_add;

	*g_out = g;
	return 0;

free:
	free_level(g);
	g->remove_fd(g->user, g->timer_fd_ptr);
e_timer_fd_add:
	close(g->timer_fd);
e_timerfd:
	free(g);
e_malloc:
	return -1;
}

int game_new(
		struct game **game_out,
		void *(*add_fd)(
			void *user,
			int fd,
			unsigned events,
			int (*callback)(void *user1, unsigned revents),
			void *user1),
		void (*remove_fd)(void *user, void *fd_ptr),
		void *user)
{
	return game(NULL, game_out, add_fd, remove_fd, user);
}

void game_free(struct game *g)
{
	if(g) game(g, NULL, NULL, NULL, NULL);
}

static void start_countdown(struct game *g)
{
	g->countdown = 2;
	struct itimerspec spec;
	spec.it_interval.tv_sec = 1;
	spec.it_interval.tv_nsec = 0;
	spec.it_value.tv_sec = 1;
	spec.it_value.tv_nsec = 0;
	timerfd_settime(g->timer_fd, 0, &spec, NULL);
}

static void stop_countdown(struct game *g)
{
	struct itimerspec spec = {};
	spec.it_value.tv_sec = 0;
	spec.it_value.tv_nsec = 0;
	timerfd_settime(g->timer_fd, 0, &spec, NULL);
}

static int update_countdown(struct game *g)
{
	if(g->countdown == 0) {
		stop_countdown(g);
		g->state = GAME_PLAYING;
		refresh_all(g);
		return 0;
	}
	--g->countdown;
	unsigned coords[] = {
		35, 4, 35, 5, 35, 6, 35, 7, 35, 8, 35, 9, 35, 10,
		36, 4, 36, 5, 36, 6, 36, 7, 36, 8, 36, 9, 36, 10,
		37, 4, 37, 5, 37, 6, 37, 7, 37, 8, 37, 9, 37, 10,
		38, 4, 38, 5, 38, 6, 38, 7, 38, 8, 38, 9, 38, 10,
		39, 4, 39, 5, 39, 6, 39, 7, 39, 8, 39, 9, 39, 10,
		40, 4, 40, 5, 40, 6, 40, 7, 40, 8, 40, 9, 40, 10,
	};
	update_coords_all(g, sizeof coords / sizeof coords[0] / 2, coords);
	return 0;
}

static int timer_f(void *user1, unsigned revents)
{
	struct game *g = user1;
	uint64_t expirations;
	read(g->timer_fd, &expirations, sizeof expirations);
	while(expirations) {
		update_countdown(g);
		--expirations;
	}
	return 0;
}

/*
 * Loading levels
 */

static int resize_level(
		struct game *g,
		unsigned w,
		unsigned h)
{
	unsigned w1, h1;
	/* Only increase size. */
	w1 = w > g->w ? w : g->w;
	h1 = h > g->h ? h : g->h;
	if(w1 == g->w && h1 == g->h) return 0;

	static struct tile default_tile = {
		.flags = 0,
		.base = ' ',
	};
	struct tile *new_level;
	unsigned new_sz = sizeof *new_level * w1 * h1;
	new_level = malloc(new_sz);
	if(!new_level) {
		fprintf(stderr, "%s", strerror(errno));
		return -1;
	}

	unsigned x, y;
	for(y = 0; y < h1; ++y) {
		for(x = 0; x < w1; ++x) {
			if(y < g->h && x < g->w) {
				new_level[y * w1 + x] = g->level[y * g->w + x];
			}
			else {
				new_level[y * w1 + x] = default_tile;
			}
		}
	}

	free(g->level);
	g->level = new_level;
	g->w = w1;
	g->h = h1;
}

static void invalidate(struct game *g, unsigned x, unsigned y)
{
	if(g->level_flags & LEVEL_LOADING) return;
	assert(g->n_invalid_coords + 1 < MAX_INVALID);
	unsigned i = g->n_invalid_coords;
	++g->n_invalid_coords;
	g->invalid_coords[2 * i] = x;
	g->invalid_coords[2 * i + 1] = y;
}

static int coords_cmp(const void *a, const void *b)
{
	unsigned *aptr = (void *)a, *bptr = (void *)b;
	unsigned ax = aptr[0], ay = aptr[1], bx = bptr[0], by = bptr[1];
	if(ay < by) return -1;
	else if(ay > by) return 1;
	else {
		if(ax < bx) return -1;
		else if(ax > bx) return 1;
		else return 0;
	}
}

static void update_invalid(struct game *g)
{
	/* Sort so we don't need as many terminal warp commands. */
	qsort(g->invalid_coords, g->n_invalid_coords,
			2 * sizeof g->invalid_coords[0], coords_cmp);
	update_coords_all(g, g->n_invalid_coords, g->invalid_coords);
	g->n_invalid_coords = 0;
}

static void recheck_tile_for_objects(struct game *g, unsigned x, unsigned y)
{
	unsigned has_object = 0;
	unsigned i;
	for(i = 0; i < g->n_objects; ++i) {
		if(g->objects[i]->x == x && g->objects[i]->y == y) {
			has_object = 1;
		}
	}
	if(has_object) g->level[y * g->w + x].flags |= HAS_OBJECT;
	else g->level[y * g->w + x].flags &= ~HAS_OBJECT;
	invalidate(g, x, y);
}

static void object_remove_2(struct object *o, unsigned call_callback)
{
	if(!o) return;

	struct game *g = o->g;
	unsigned i;
	for(i = 0; i < g->n_objects; ++i) {
		if(g->objects[i] == o) {
			o->g = NULL;
			int x = o->x, y = o->y;
			/* Might free o. */
			if(call_callback) o->class->remove(o);
			memmove(g->objects + i, g->objects + i + 1,
					sizeof *g->objects *
					(g->n_objects - i - 1));
			--g->n_objects;
			recheck_tile_for_objects(g, x, y);
			return;
		}
	}
}

static void object_remove(struct object *o)
{
	object_remove_2(o, 1);
}

static int add_object_to_level(
		struct object **o_out,
		struct game *g,
		struct class *class,
		unsigned x,
		unsigned y,
		unsigned z,
		void *user)
{
	struct object *o = malloc(sizeof *o);
	if(!o) return -1;

	struct object **new_objects = realloc(g->objects,
			sizeof *new_objects * (g->n_objects + 1));
	if(!new_objects) {
		free(o);
		return -1;
	}
	g->objects = new_objects;

	/* Insert to keep list z-sorted. Lowest first. */
	unsigned i;
	for(i = 0; i < g->n_objects; ++i) {
		if(g->objects[i]->z > z) break;
	}

	memmove(new_objects + i + 1, new_objects + i,
			sizeof *new_objects * (g->n_objects - i));

	g->objects[i] = o;
	g->objects[i]->g = g;
	g->objects[i]->x = x;
	g->objects[i]->y = y;
	g->objects[i]->z = z;
	g->objects[i]->class = class;
	g->objects[i]->user = user;
	++g->n_objects;

	assert(g->w > x && g->h > y);
	recheck_tile_for_objects(g, x, y);

	*o_out = o;
	return 0;
}

static void object_free(struct object *o)
{
	if(o) {
		if(o->g) object_remove_2(o, 0);
		free(o);
	}
}

static void free_level(struct game *g)
{
	g->level_flags |= LEVEL_LOADING;
	while(g->n_objects) object_remove(g->objects[0]);
	free(g->objects);
	g->objects = NULL;

	unsigned i;
	for(i = 0; i < MAX_PLAYERS; ++i) {
		g->start_pos[i].x = UINT_MAX;
		g->start_pos[i].y = UINT_MAX;
	}

	free(g->level);
	g->w = 0;
	g->h = 0;
	g->level = NULL;
	g->level_flags &= ~LEVEL_LOADING;
}

static struct class key_class;
static struct class door_class;
static struct class ice_class;
static struct class bldr_class;
struct boulder;
static int boulder_new(struct boulder **b_out, struct game *g, unsigned x,
		unsigned y);
static char keys[] = "abcdefghijklmnopqrstuwxyz";
static char doors[] = "ABCDEFGHIJKLMNOPQRSTUWXYZ";
static int load_level(struct game *g)
{
	unsigned i;

	free_level(g);

	g->level_flags |= LEVEL_LOADING;

	unsigned x = 0, y = 0;
	unsigned was_newline = 0;
	unsigned start_pos_n = 0;

	g->level_offset = lseek(g->level_fd, 0, SEEK_CUR);

	char ch;
	while(1) {
		int status = read(g->level_fd, &ch, 1);
		if(status < 0) {
			fprintf(stderr, "%s", strerror(errno));
			goto error;
		}
		if(status == 0) goto level_done;

		if(ch == '\n') {
			if(was_newline) {
				goto level_done;
			}
			x = 0;
			++y;
			was_newline = 1;
			continue;
		}
		else {
			was_newline = 0;
		}

		if(ch == ' ' || ch == '\t') {
			continue;
		}

		if(resize_level(g, x + 1, y + 1) < 0) goto error;
		struct tile *tile = &g->level[y * g->w + x];

		if(ch == '.') {
			/* Ground */
			tile->base = ' ';
			tile->flags = 0;
		}
		else if(ch == '#') {
			/* Walls */
			tile->base = '#';
			tile->flags = 0;
		}
		else if(ch == '=') {
			/* Goal */
			tile->base = '=';
			tile->flags = 0;
		}
		else if(ch == '@') {
			/* Start position */
			tile->base = ' ';
			tile->flags = 0;
			if(start_pos_n < MAX_PLAYERS) {
				g->start_pos[start_pos_n].x = x;
				g->start_pos[start_pos_n].y = y;
				++start_pos_n;
			}
		}
		else if(ch == '_') {
			/* Ice */
			tile->base = ' ';
			tile->flags = 0;
			struct object *o;
			add_object_to_level(&o, g, &ice_class, x, y, 0, NULL);
		}
		else if(ch == '0') {
			/* Boulder */
			tile->base = ' ';
			tile->flags = 0;
			struct boulder *b;
			if(boulder_new(&b, g, x, y) < 0) goto error;
		}
		else if(ch == '<' || ch == '>' || ch == '^' || ch == 'v') {
			/* Pusher */
			tile->base = ' ';
			tile->flags = 0;
			struct pusher *p;
			if(pusher_new(&p, g, x, y, ch)) goto error;
		}
		else if(strchr(keys, ch)) {
			/* Keys */
			tile->base = ' ';
			tile->flags = 0;
			struct object *o;
			add_object_to_level(&o, g, &key_class, x, y, 1,
					(void *)(uintptr_t)ch);
		}
		else if(strchr(doors, ch)) {
			/* Doors */
			tile->base = ' ';
			tile->flags = 0;
			struct object *o;
			add_object_to_level(&o, g, &door_class, x, y, 1,
					(void *)(uintptr_t)ch);
		}
		else {
			tile->base = ch;
			tile->flags = 0;
		}

		++x;
	}

level_done:
	if(g->w == 0 || g->h == 0) {
		g->state = GAME_FINISHED;
		goto refresh;
	}

	for(i = 0; i < MAX_PLAYERS; ++i) {
		if(!g->players[i]) continue;
		add_player_to_game(g->players[i]);
	}

	g->state = GAME_READYING;
	start_countdown(g);
refresh:
	refresh_all(g);
	g->level_flags &= ~LEVEL_LOADING;
	return 0;

error:
	g->level_flags &= ~LEVEL_LOADING;
	free_level(g);
	return -1;
}

static int levelset(
		unsigned freeing,
		struct game *g,
		char *level)
{
	if(freeing) goto free;

	g->level_fd = open(level, O_RDONLY);
	if(g->level_fd < 0) {
		fprintf(stderr, strerror(errno));
		goto e_open;
	}

	if(load_level(g) < 0) goto e_level;

	return 0;

free:
	if(g->state != GAME_NONE) free_level(g);
e_level:
	close(g->level_fd);
e_open:
	return -1;
}

int game_load(struct game *g, char *level)
{
	if(g->state != GAME_NONE) {
		levelset(1, g, NULL);
	}
	return levelset(0, g, level);
}

/*
 * Player
 */

struct player {
	struct game *g;
	void (*partial_update)(
		void *user,
		unsigned n_tiles,
		unsigned *coords);
	void (*refresh_screen)(void *user);
	void (*stop)(void *user);
	void *user;

	unsigned number;

	/* The player's character if ingame. */
	struct object *o;
	char key;
	int timer_fd;
	void *fd_ptr;
	int dx, dy;

	enum {
		PLAYER_INITIALIZING = 1,
		PLAYER_SLIDING = 2,
		PLAYER_CONTINUE_SLIDE = 4,
	} flags;
};

static int slide_callback(void *user, unsigned revents);
static int player(
		struct player *p,
		struct player **p_out,
		struct game *g,
		void (*update)(
			void *user,
			unsigned n_tiles,
			unsigned *coords),
		void (*refresh)(void *user),
		void (*stop)(void *user),
		void *user)
{
	if(p) goto free;

	p = malloc(sizeof *p);
	if(!p) goto e_malloc;

	p->g = g;
	p->partial_update = update;
	p->refresh_screen = refresh;
	p->stop = stop;
	p->user = user;
	p->o = NULL;
	p->key = 0;
	p->flags = PLAYER_INITIALIZING;

	p->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if(p->timer_fd < 0) goto e_timerfd;

	p->fd_ptr = p->g->add_fd(p->g->user, p->timer_fd, 1, slide_callback, p);
	if(!p->fd_ptr) goto e_add_fd;

	unsigned i;
	for(i = 0; i < MAX_PLAYERS; ++i) {
		if(!g->players[i]) {
			p->number = i;
			g->players[i] = p;
			goto found_number;
		}
	}
	goto e_number;
found_number:

	if(add_player_to_game(p)) goto e_add_object;
	p->flags &= ~PLAYER_INITIALIZING;

	*p_out = p;
	return 0;

free:
	object_free(p->o);
e_add_object:
	p->flags |= PLAYER_INITIALIZING;
	update_invalid(p->g);
	p->g->players[p->number] = NULL;
e_number:
	p->g->remove_fd(p->g->user, p->fd_ptr);
e_add_fd:
	close(p->timer_fd);
e_timerfd:
	free(p);
e_malloc:
	return -1;
}

int player_new(
		struct player **p_out,
		struct game *g,
		void (*update)(
			void *user,
			unsigned n_tiles,
			unsigned *coords),
		void (*refresh)(void *user),
		void (*stop)(void *user),
		void *user)
{
	return player(NULL, p_out, g, update, refresh, stop, user);
}

void player_free(struct player *p)
{
	if(p) player(p, NULL, NULL, NULL, NULL, NULL, NULL);
}

static void refresh_all(struct game *g)
{
	unsigned i;
	for(i = 0; i < MAX_PLAYERS; ++i) {
		if(!g->players[i] || (g->players[i]->flags &
				PLAYER_INITIALIZING)) continue;
		g->players[i]->refresh_screen(g->players[i]->user);
	}
}

static void update_coords_all(struct game *g, unsigned n, unsigned *coords)
{
	unsigned i;
	for(i = 0; i < MAX_PLAYERS; ++i) {
		if(!g->players[i] || (g->players[i]->flags &
				PLAYER_INITIALIZING)) continue;
		g->players[i]->partial_update(g->players[i]->user, n, coords);
	}
}

static unsigned get_player_color(unsigned player_number) {
	static unsigned colors[] = { 1, 4, 2, 3, 5, 6, 7 };
	return colors[player_number % (sizeof colors / sizeof colors[0])];
}

/*
 * The player object
 */

static void player_remove(struct object *o)
{
	/* Don't free because players are persistent between levels. */
	struct player *p = o->user;
	p->key = '\0';
	object_free(p->o);
	p->o = NULL;
	if(p->flags & PLAYER_SLIDING) {
		struct itimerspec it = {};
		it.it_value.tv_sec = 0;
		it.it_value.tv_nsec = 0;
		timerfd_settime(p->timer_fd, 0, &it, NULL);
	}
	p->flags = 0;
}

static unsigned player_push(struct object *o, struct object *o1, int dx, int dy,
		unsigned strength)
{
	/* Does not move and blocks movement. */
	return 0;
}

static void player_enter(struct object *o, struct object *o1, int dx, int dy)
{
}

static void player_leave(struct object *o, struct object *o1)
{
}

static void player_give_key(struct object *o, char key)
{
	struct player *p = o->user;
	p->key = key;
}

static unsigned player_has_key(struct object *o, char key)
{
	struct player *p = o->user;
	return p->key == key;
}

static void player_slip(struct object *o, int dx, int dy)
{
	struct player *p = o->user;
	p->dx = dx;
	p->dy = dy;
	if(!(p->flags & PLAYER_SLIDING)) {
		p->flags |= PLAYER_SLIDING;
		struct itimerspec it = {};
		it.it_interval.tv_sec = 0;
		it.it_interval.tv_nsec = SLIDE_TIME_NSEC;
		it.it_value = it.it_interval;
		timerfd_settime(p->timer_fd, 0, &it, NULL);
		p->flags |= PLAYER_SLIDING;
	}
	else {
		p->flags |= PLAYER_CONTINUE_SLIDE;
	}
}

static void player_move(struct player *p, int dx, int dy);
static int slide_callback1(struct player *p)
{
	player_move(p, p->dx, p->dy);
	if(!(p->flags & PLAYER_CONTINUE_SLIDE)) {
		struct itimerspec it = {};
		it.it_value.tv_sec = 0;
		it.it_value.tv_nsec = 0;
		timerfd_settime(p->timer_fd, 0, &it, NULL);
		p->flags &= ~PLAYER_SLIDING;
	}
	p->flags &= ~PLAYER_CONTINUE_SLIDE;
	return 0;
}

static int slide_callback(void *user, unsigned revents)
{
	struct player *p = user;
	uint64_t n;
	read(p->timer_fd, &n, sizeof n);
	while(n--) slide_callback1(p);
}

static void player_draw(struct object *o, unsigned *ch_out, unsigned *fg_out,
		unsigned *bg_out)
{
	struct player *p = o->user;
	*ch_out = '@';
	*fg_out = get_player_color(p->number);
}

static struct class player_class = {
	.remove = player_remove,
	.push = player_push,
	.enter = player_enter,
	.leave = player_leave,
	.give_key = player_give_key,
	.has_key = player_has_key,
	.slip = player_slip,
	.draw = player_draw,
};

static int add_player_to_game(struct player *p)
{
	if(p->g->start_pos[p->number].x == UINT_MAX) return 0;
	int status = add_object_to_level(
			&p->o,
			p->g,
			&player_class,
			p->g->start_pos[p->number].x,
			p->g->start_pos[p->number].y,
			10,
			p);
	update_invalid(p->g);
	return status;
}

/*
 * Drawing the game
 */

struct tileout {
	struct player *p;
	unsigned x;
	unsigned y;
	unsigned *ch_out;
	unsigned *bg_out;
	unsigned *fg_out;
};

static void draw_string_left(
		struct tileout *to,
		unsigned x0,
		unsigned x1,
		unsigned y,
		char *str,
		unsigned bg,
		unsigned fg)
{
	if(to->y != y) return;
	if(to->x < x0) return;
	if(to->x > x1) return;
	if(to->x - x0 >= strlen(str)) {
		*to->ch_out = ' ';
	}
	else {
		*to->ch_out = str[to->x - x0];
	}
	*to->bg_out = bg;
	*to->fg_out = fg;
}

static void draw_string_centered(
		struct tileout *to,
		unsigned x0,
		unsigned x1,
		unsigned y,
		char *str,
		unsigned bg,
		unsigned fg)
{
	if(to->y != y) return;
	if(to->x < x0) return;
	if(to->x > x1) return;
	unsigned len = strlen(str);
	int start = (x1 + x0 - len) / 2;
	int str_offs = (to->x - x0) - start;
	if(str_offs < 0 || str_offs >= len) {
		*to->ch_out = ' ';
	}
	else {
		*to->ch_out = str[str_offs];
	}
	*to->bg_out = bg;
	*to->fg_out = fg;
}

void draw_player_status(
		struct tileout *to,
		struct player *p,
		unsigned x0,
		unsigned x1,
		unsigned y)
{
	if(to->x < x0) return;
	if(to->x >= x1) return;
	if(to->y != y) return;

	static char msg[] = "Du är ";
	unsigned len = strlen(msg);
	draw_string_left(to, x0, x1, y, msg, 0, 7);
	if(to->x == x0 + len) {
		*to->bg_out = 0;
		*to->fg_out = get_player_color(p->number);
		*to->ch_out = '@';
	}
}

static void draw_countdown(
		struct tileout *to,
		unsigned x,
		unsigned y,
		unsigned n)
{
	if(to->x < x) return;
	if(to->x >= x + 6) return;
	if(to->y < y) return;
	if(to->y >= y + 7) return;

	char *nums[] = {
		"  ##  "
		" ###  "
		"  ##  "
		"  ##  "
		"  ##  "
		"  ##  "
		" #### ",
		" #### "
		"##  ##"
		"    ##"
		"   ## "
		"  ##  "
		" ##   "
		"######",
		" #### "
		"##  ##"
		"    ##"
		"   ## "
		"    ##"
		"##  ##"
		" #### ",
	};
	*to->ch_out = nums[n][(to->y - y) * 6 + (to->x - x)];
	*to->bg_out = 0;
	*to->fg_out = 3;
}

static void draw_game(
		struct tileout *to,
		struct game *g,
		struct player *p)
{
	unsigned x, y;
	x = to->x;
	y = to->y;
	if(x >= g->w) return;
	if(y >= g->h) return;

	*to->ch_out = g->level[y * g->w + x].base;
	*to->bg_out = 0;
	*to->fg_out = 7;

	unsigned i;
	if(g->level[y * g->w + x].flags & HAS_OBJECT) {
		for(i = 0; i < g->n_objects; ++i) {
			if(g->objects[i]->x == x && g->objects[i]->y == y) {
				g->objects[i]->class->draw(g->objects[i],
						to->ch_out, to->fg_out,
						to->bg_out);
			}
		}
	}
}

void player_get_tile(
		struct player *p,
		unsigned x,
		unsigned y,
		unsigned *ch_out,
		unsigned *bg_out,
		unsigned *fg_out)
{
	struct game *g = p->g;
	struct tileout to;
	to.p = p;
	to.x = x;
	to.y = y;
	to.ch_out = ch_out;
	to.bg_out = bg_out;
	to.fg_out = fg_out;
	*ch_out = ' ';
	*bg_out = 0;
	*fg_out = 7;

	if(g->state == GAME_NONE) {
		draw_string_centered(&to, 0, 80, 5, "Servern är inte aktiv",
				0, 7);
		draw_player_status(&to, p, 10, 70, 15);
	}
	else if(g->state == GAME_READYING) {
		draw_countdown(&to, 35, 4, g->countdown);
		draw_player_status(&to, p, 10, 70, 15);
	}
	else if(g->state == GAME_PLAYING) {
		draw_game(&to, g, p);
		draw_player_status(&to, p, 1, 80, 22);
	}
	else if(g->state == GAME_FINISHED) {
		draw_string_centered(&to, 0, 80, 5, "Ni klarade det!",
				0, 7);
	}
}

/*
 * Object utility
 */

/* Push what's at x, y in direction dx, dy and return 1 if the space is
 * unoccupied after. */
static unsigned push(
		struct object *pusher,
		unsigned x,
		unsigned y,
		int dx,
		int dy,
		unsigned strength)
{
	struct game *g = pusher->g;
	struct tile *src = &g->level[y * g->w + x];

	if(!strength) return 0;

	switch(src->base) case '#': return 0;

	if(src->flags & HAS_OBJECT) {
		unsigned blocked = 0;
		unsigned i;
		for(i = 0; i < g->n_objects; ++i) {
			struct object *o = g->objects[i];
			if(o->x == x && o->y == y) {
				if(o->class->push && !o->class->push(
							o, pusher, dx, dy,
							strength)) {
					/* Player is blocked. */
					blocked = 1;
				}
			}
		}
		if(blocked) return 0;
	}
	return 1;
}

static void move_object(
		struct object *o,
		unsigned x1,
		unsigned y1)
{
	struct game *g = o->g;
	unsigned x0 = o->x, y0 = o->y;

	struct tile
		*src = &g->level[y0 * g->w + x0],
		*dst = &g->level[y1 * g->w + x1];

	o->x = x1;
	o->y = y1;

	src->flags &= ~HAS_OBJECT;
	dst->flags |= HAS_OBJECT;

	unsigned i;
	for(i = 0; i < g->n_objects; ++i) {
		struct object *o1 = g->objects[i];
		if(o1->x == x0 && o1->y == y0) {
			src->flags |= HAS_OBJECT;
			if(o1->class->leave) o1->class->leave(o1, o);
		}
	}
	for(i = 0; i < g->n_objects; ++i) {
		struct object *o1 = g->objects[i];
		if(o1->x == x1 && o1->y == y1) {
			if(o1->class->enter) o1->class->enter(o1, o,
					(int)x1 - (int)x0,
					(int)y1 - (int)y0);
		}
	}

	invalidate(g, x0, y0);
	invalidate(g, x1, y1);
}

/*
 * Key object. User pointer is actually the char of the key cast to void *.
 */

static unsigned push_key(struct object *o, struct object *o1, int dx, int dy,
		unsigned strength)
{
	/* Does not block movement. */
	return 1;
}

static void enter_key(struct object *o, struct object *o1, int dx, int dy)
{
	char ch = (char)(uintptr_t)o->user;
	o1->class->give_key(o1, ch);
	object_remove(o);
}

static void draw_key(struct object *o, unsigned *ch_out, unsigned *fg_out,
		unsigned *bg_out)
{
	char ch = (char)(uintptr_t)o->user;
	*ch_out = ch;
	*fg_out = 7;
}

static struct class key_class = {
	.remove = object_free,
	.push = push_key,
	.enter = enter_key,
	.draw = draw_key,
};

/*
 * Door object.
 */

static unsigned push_door(struct object *o, struct object *o1, int dx, int dy,
		unsigned strength)
{
	char door = (char)(uintptr_t)o->user;
	char key = keys[strchr(doors, door) - doors];
	if(o1->class->has_key(o1, key)) {
		object_remove(o);
		return 1;
	}
	else {
		return 0;
	}
}

static void draw_door(struct object *o, unsigned *ch_out, unsigned *fg_out,
		unsigned *bg_out)
{
	char door = (char)(uintptr_t)o->user;
	*ch_out = door;
	*fg_out = 7;
}

static struct class door_class = {
	.remove = object_free,
	.push = push_door,
	.draw = draw_door,
};

/*
 * Ice object
 */

static void free_ice(struct object *o)
{
}

static unsigned push_ice(struct object *o, struct object *o1, int dx, int dy,
		unsigned strength)
{
	return 1;
}

static void enter_ice(struct object *o, struct object *o1, int dx, int dy)
{
	o1->class->slip(o1, dx, dy);
}

static void leave_ice(struct object *o, struct object *o1)
{
}

static void draw_ice(struct object *o, unsigned *ch_out, unsigned *fg_out,
		unsigned *bg_out)
{
	*ch_out = '_';
	*fg_out = 7;
	*bg_out = 0;
}

static struct class ice_class = {
	.remove = free_ice,
	.push = push_ice,
	.enter = enter_ice,
	.leave = leave_ice,
	.draw = draw_ice,
};

/*
 * Boulder
 */

struct boulder {
	int timer_fd;
	void *fd_ptr;
	enum {
		BOULDER_SLIDING = 1,
		BOULDER_CONTINUE_SLIDING = 2,
	} flags;
	int dx, dy;
	struct game *g;
	struct object *o;
};

static int boulder_cb(void *user, unsigned revents);
static int boulder(struct boulder *b, struct boulder **b_out, struct game *g,
		unsigned x, unsigned y)
{
	if(b) goto free;

	b = malloc(sizeof *b);
	if(!b) goto e_malloc;

	b->flags = 0;
	b->g = g;

	b->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if(b->timer_fd < 0) goto e_timerfd;

	b->fd_ptr = g->add_fd(g->user, b->timer_fd, 1, boulder_cb, b);
	if(!b->fd_ptr) goto e_add_fd;

	if(add_object_to_level(&b->o, g, &bldr_class, x, y, 5, b) < 0) {
		goto e_add_object;
	}

	*b_out = b;
	return 0;

free:
	object_free(b->o);
e_add_object:
	b->g->remove_fd(b->g->user, b->fd_ptr);
e_add_fd:
	close(b->timer_fd);
e_timerfd:
	free(b);
e_malloc:
	return -1;
}

static int boulder_new(struct boulder **b_out, struct game *g, unsigned x,
		unsigned y)
{
	return boulder(NULL, b_out, g, x, y);
}

static void boulder_free(struct boulder *b)
{
	if(b) boulder(b, NULL, NULL, 0, 0);
}

static void free_boulder(struct object *o)
{
	boulder_free(o->user);
}

static unsigned push_boulder(
		struct object *o,
		struct object *o1,
		int dx,
		int dy,
		unsigned strength)
{
	if(push(o, o->x + dx, o->y + dy, dx, dy, strength - 1)) {
		move_object(o, o->x + dx, o->y + dy);
		return 1;
	}
	else {
		return 0;
	}
}

static void draw_boulder(struct object *o, unsigned *ch_out, unsigned *fg_out,
		unsigned *bg_out)
{
	*ch_out = '0';
	*fg_out = 3;
}

static void enter_boulder(struct object *o, struct object *o1, int dx, int dy)
{
}

static void leave_boulder(struct object *o, struct object *o1)
{
}

static void give_boulder_key(struct object *o, char key)
{
}

static unsigned boulder_has_key(struct object *o, char key)
{
	return 0;
}

static void boulder_slip(struct object *o, int dx, int dy)
{
	struct boulder *b = o->user;
	b->dx = dx;
	b->dy = dy;
	if(b->flags & BOULDER_SLIDING) {
		b->flags |= BOULDER_CONTINUE_SLIDING;
	}
	else {
		struct itimerspec it;
		it.it_interval.tv_sec = 0;
		it.it_interval.tv_nsec = SLIDE_TIME_NSEC;
		it.it_value = it.it_interval;
		timerfd_settime(b->timer_fd, 0, &it, NULL);
		b->flags |= BOULDER_SLIDING;
	}
}

static void boulder_cb1(struct boulder *b)
{
	unsigned x0 = b->o->x, y0 = b->o->y;
	unsigned x1 = b->o->x + b->dx, y1 = b->o->y + b->dy;
	if(push(b->o, x1, y1, b->dx, b->dy, 1)) {
		move_object(b->o, x1, y1);
		if(!(b->flags & BOULDER_CONTINUE_SLIDING)) {
			struct itimerspec it = {};
			it.it_value.tv_sec = 0;
			it.it_value.tv_nsec = 0;
			timerfd_settime(b->timer_fd, 0, &it, NULL);
			b->flags &= ~BOULDER_SLIDING;
		}
		invalidate(b->o->g, x0, y0);
		invalidate(b->o->g, x1, y1);
	}
	else {
		struct itimerspec it = {};
		it.it_value.tv_sec = 0;
		it.it_value.tv_nsec = 0;
		timerfd_settime(b->timer_fd, 0, &it, NULL);
		b->flags &= ~BOULDER_SLIDING;
	}
	b->flags &= ~BOULDER_CONTINUE_SLIDING;
	update_invalid(b->g);
}

static int boulder_cb(void *user, unsigned revents)
{
	struct boulder *b = user;
	uint64_t n;
	read(b->timer_fd, &n, sizeof n);
	while(n--) boulder_cb1(b);
}

static struct class bldr_class = {
	.remove = free_boulder,
	.push = push_boulder,
	.enter = enter_boulder,
	.leave = leave_boulder,
	.give_key = give_boulder_key,
	.has_key = boulder_has_key,
	.slip = boulder_slip,
	.draw = draw_boulder,
};

/*
 * Pusher
 */

struct pusher {
	struct game *g;
	char type;
	int dx, dy;
	struct object *o;
};

static struct class pusher_class;

static int pusher(struct pusher *p, struct pusher **p_out, struct game *g,
		int x, int y, char type)
{
	if(p) goto free;

	p = malloc(sizeof *p);
	if(!p) {
		fprintf(stderr, "Malloc returned NULL at %s: %d.\n",
				__FILE__, __LINE__);
		goto e_malloc;
	}

	p->g = g;
	p->type = type;

	p->dx = type == '<' ? -1 :
		type == '>' ? 1 :
		0;
	p->dy = type == '^' ? -1 :
		type == 'v' ? 1 :
		0;

	if(add_object_to_level(&p->o, g, &pusher_class, x, y, 1, p)) {
		goto e_add_object;
	}

	*p_out = p;
	return 0;

free:
	object_free(p->o);
e_add_object:
	free(p);
e_malloc:
	return -1;
}

static int pusher_new(struct pusher **p_out, struct game *g, int x,
		int y, char type)
{
	return pusher(NULL, p_out, g, x, y, type);
}

static void pusher_free(struct pusher *p)
{
	if(p) pusher(p, NULL, NULL, 0, 0, 0);
}

static void pusher_remove(struct object *o)
{
	pusher_free(o->user);
}

static void pusher_draw(struct object *o, unsigned *ch_out, unsigned *fg_out,
		unsigned *bg_out)
{
	struct pusher *p = o->user;
	*ch_out = p->type;
	*fg_out = 7;
}

static void pusher_enter(struct object *o, struct object *o1, int dx, int dy)
{
	struct pusher *p = o->user;
	o1->class->slip(o1, p->dx, p->dy);
}

static struct class pusher_class = {
	.remove = pusher_remove,
	.enter = pusher_enter,
	.draw = pusher_draw,
};
/*
 * Player input
 */

void player_key(struct player *p, unsigned char ch)
{
	if(ch == 'q' || ch == 'Q') {
		p->stop(p->user);
	}
	else if(ch == 'r' || ch == 'R') {
		lseek(p->g->level_fd, p->g->level_offset, SEEK_SET);
		load_level(p->g);
	}
}

static void player_move(struct player *p, int dx, int dy)
{
	if(p->g->state != GAME_PLAYING) return;
	if(!p->o) return;

	assert(dx > 0 || p->o->x >= -dx);
	assert(dy > 0 || p->o->y >= -dy);
	assert(dx < 0 || p->o->x + dx < p->g->w);
	assert(dy < 0 || p->o->y + dy < p->g->h);

	unsigned
		x0 = p->o->x,
		y0 = p->o->y,
		x1 = p->o->x + dx,
		y1 = p->o->y + dy;
	struct tile *dst = &p->g->level[y1 * p->g->w + x1];

	if(!push(p->o, x1, y1, dx, dy, 2)) return;

	move_object(p->o, x1, y1);
	update_invalid(p->g);

	if(dst->base == '=') {
		load_level(p->g);
	}
}

static void player_move_command(struct player *p, int dx, int dy)
{
	if(p->flags & PLAYER_SLIDING) return;
	player_move(p, dx, dy);
}

void player_left(struct player *p)
{
	player_move_command(p, -1, 0);
}

void player_right(struct player *p)
{
	player_move_command(p, 1, 0);
}

void player_up(struct player *p)
{
	player_move_command(p, 0, -1);
}

void player_down(struct player *p)
{
	player_move_command(p, 0, 1);
}
