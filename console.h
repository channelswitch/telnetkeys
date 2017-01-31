struct console;
struct game;

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
		void (*quit_callback)(void *user),
		void *user);
void console_free(struct console *c);
