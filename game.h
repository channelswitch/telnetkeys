struct game;

int game_new(
		struct game **game_out,
		void *(*add_fd)(
			void *user,
			int fd,
			unsigned events,
			int (*callback)(void *user1, unsigned revents),
			void *user1),
		void (*remove_fd)(void *user, void *fd_ptr),
		void *user);
void game_free(struct game *g);

int game_load(struct game *g, char *level);
