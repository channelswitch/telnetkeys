struct commands;
int commands_new(struct commands **c_out, int in_fd);
void commands_free(struct commands *c);

int commands_get_fd(struct commands *c);

struct commands_event {
	enum {
		COMMANDS_QUIT,
		COMMANDS_LOAD,
	} type;
	union {
		struct {
			char *level;
		} load;
	};
};
int commands_event(struct commands *c, struct commands_event *ev_out);

int commands_stop(struct commands *c);
