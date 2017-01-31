#include "makejmp.h"
#include <ucontext.h>
#include <stdlib.h>
#include <string.h>

struct data {
	ucontext_t new, old;
	jmp_buf *buf;
	void (*f)(void *user);
	void *user;
};

static void wrapper(struct data *data)
{
	void (*f)(void *user) = data->f;
	void *user = data->user;
	if(setjmp(*data->buf)) f(user);
	else swapcontext(&data->new, &data->old);
	abort();
}

void makejmp(
		jmp_buf new_context,
		void *stack,
		unsigned stack_sz,
		void (*f)(void *user),
		void *user)
{
	struct data data;
	data.f = f;
	data.buf = (jmp_buf *)new_context;
	data.user = user;
	getcontext(&data.new);
	data.new.uc_stack.ss_sp = stack;
	data.new.uc_stack.ss_size = stack_sz;
	makecontext(&data.new, (void (*)())wrapper, 1, &data);
	swapcontext(&data.old, &data.new);
}

void swapjmp(jmp_buf from, jmp_buf to)
{
	if(!setjmp(from)) longjmp(to, 1);
}

void copyjmp(jmp_buf dst, jmp_buf src)
{
	jmp_buf *dst1 = (jmp_buf *)dst, *src1 = (jmp_buf *)src;
	memcpy(dst1, src1, sizeof *src1);
}
