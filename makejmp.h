#include <setjmp.h>
void makejmp(
		jmp_buf new_context,
		void *stack,
		unsigned stack_sz,
		void (*f)(void *user),
		void *user);
void swapjmp(jmp_buf from, jmp_buf to);
void copyjmp(jmp_buf dst, jmp_buf src);
