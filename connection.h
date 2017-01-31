struct connection;
struct game;

/* Socket is an fd on which accept() will be called. */
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
		void *user);
void connection_stop(struct connection *c, void (*cb)(void *));
void connection_free(struct connection *c);

