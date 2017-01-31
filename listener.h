struct listener;
struct game;

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
		void *user);
void listener_stop(struct listener *l, void (*callback)(void *user));
void listener_free(struct listener *l);
