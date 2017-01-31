/*
 * Players stuff.
 */

struct player;
struct game;
int player_new(
		struct player **p_out,
		struct game *g,
		void (*partial_update)(
			void *user,
			unsigned n_tiles,
			unsigned *coords),
		void (*refresh_screen)(void *user),
		void (*stop)(void *user),
		void *user);
void player_free(struct player *p);

void player_get_level_size(
		struct player *p,
		unsigned *w_out,
		unsigned *h_out);
void player_get_tile(
		struct player *p,
		unsigned x,
		unsigned y,
		unsigned *ch_out,
		unsigned *bg_out,
		unsigned *fg_out);

void player_key(struct player *p, unsigned char ch);
void player_left(struct player *p);
void player_right(struct player *p);
void player_up(struct player *p);
void player_down(struct player *p);
