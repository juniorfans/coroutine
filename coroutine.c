#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MACH__
	#include <sys/ucontext.h>
#else 
	#include <ucontext.h>
#endif 

#define STACK_SIZE (1024*1024)
#define DEFAULT_COROUTINE 16

struct coroutine;

struct schedule {
	char stack[STACK_SIZE];		//线程栈
	ucontext_t main;		//上下文
	int nco;				//now coroutine: 当前的 coroutine
	int cap;				//已分配的容量: 协和个数
	int running;			//当前正在运行的是哪一个协程
	struct coroutine **co;	//coroutine 指针数组
};

struct coroutine {
	coroutine_func func;	//执行的函数
	void *ud;				//函数参数
	ucontext_t ctx;			//协程上下文
	struct schedule * sch;	//调用器
	ptrdiff_t cap;			//栈容量。考虑到复用协程的栈, 当前栈大小大于已分配的栈容量则需要重新分配, 否则可以复用.
	ptrdiff_t size;			//当前协程的栈的大小
	int status;				//协程状态, 与线程状态对应
	char *stack;			//协和执行的栈
};

struct coroutine * 
_co_new(struct schedule *S , coroutine_func func, void *ud) {
	struct coroutine * co = malloc(sizeof(*co));
	co->func = func;
	co->ud = ud;
	co->sch = S;
	co->cap = 0;
	co->size = 0;
	co->status = COROUTINE_READY;
	co->stack = NULL;
	return co;
}

void
_co_delete(struct coroutine *co) {
	free(co->stack);
	free(co);
}

struct schedule * 
coroutine_open(void) {
	struct schedule *S = malloc(sizeof(*S));
	S->nco = 0;
	S->cap = DEFAULT_COROUTINE;
	S->running = -1;
	S->co = malloc(sizeof(struct coroutine *) * S->cap);
	memset(S->co, 0, sizeof(struct coroutine *) * S->cap);
	return S;
}

void 
coroutine_close(struct schedule *S) {
	int i;
	for (i=0;i<S->cap;i++) {
		struct coroutine * co = S->co[i];
		if (co) {
			_co_delete(co);
		}
	}
	free(S->co);
	S->co = NULL;
	free(S);
}

//倍增法扩充 s->co
int 
coroutine_new(struct schedule *S, coroutine_func func, void *ud) {
	struct coroutine *co = _co_new(S, func , ud);
	if (S->nco >= S->cap) {
		int id = S->cap;
		//保证 S->co 扩充到两倍之原空间大小
		S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
		//置扩充后的空间零值
		memset(S->co + S->cap , 0 , sizeof(struct coroutine *) * S->cap);
		S->co[S->cap] = co;
		S->cap *= 2;
		++S->nco;
		return id;
	} else {
		int i;
		for (i=0;i<S->cap;i++) {
			int id = (i+S->nco) % S->cap;
			//查看 coroutine 数组，重复利用已被回收的 coroutine 的 id
			if (S->co[id] == NULL) {
				S->co[id] = co;
				++S->nco;
				return id;
			}
		}
	}
	
	//we not get here or error
	assert(false);
	return -1;
}

static void
mainfunc(uint32_t low32, uint32_t hi32) {
	uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
	struct schedule *S = (struct schedule *)ptr;
	int id = S->running;
	struct coroutine *C = S->co[id];
	C->func(S,C->ud);
	
	//此处初看时觉得很诡异: 为什么会需要删除协程 C 呢? 这是因为当代码执行到此处时, 说明客户指定的 func 已经执行完毕, 协程可以被删除
	//若客户的函数 func 内部是一个死循环则不会执行到此处, 也就不会删除协程，反之需要删除
	_co_delete(C);
	S->co[id] = NULL;
	--S->nco;
	S->running = -1;
}

void 
coroutine_resume(struct schedule * S, int id) {
	assert(S->running == -1);
	assert(id >=0 && id < S->cap);
	struct coroutine *C = S->co[id];
	if (C == NULL)
		return;
	int status = C->status;
	switch(status) {
	case COROUTINE_READY:
		getcontext(&C->ctx);
		C->ctx.uc_stack.ss_sp = S->stack;		//栈顶指针(栈向下增长. 最上面的是当前栈桢的栈底，最下面是栈顶. 往栈顶压入时栈顶会向下变化)
		C->ctx.uc_stack.ss_size = STACK_SIZE;
		C->ctx.uc_link = &S->main;	//uc_link指向当前的上下文结束时要恢复到的上下文
		S->running = id;
		C->status = COROUTINE_RUNNING;
		uintptr_t ptr = (uintptr_t)S;
		//线程上下文 + 线程堆栈() 构成了线程的执行环境
		//先调用 mainfunc, 再切换到 C->ctx 的上下文
		makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr>>32));
		//把当前的上下文保存在 S->main 中, 再把上下文切换到 C->ctx 中
		swapcontext(&S->main, &C->ctx);
		break;
	case COROUTINE_SUSPEND:
		//在 _save_stack 中保存的是整个线程的栈，所以此处直接恢复整个栈即可
		memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size);
		S->running = id;
		C->status = COROUTINE_RUNNING;
		swapcontext(&S->main, &C->ctx);
		break;
	default:
		assert(0);
	}
}

/*当前函数调用栈是: 
ebp
top
C
dummy	---> esp
*/

static void
_save_stack(struct coroutine *C, char *top) {
	char dummy = 0;
	assert(top - &dummy <= STACK_SIZE);
	if (C->cap < top - &dummy) {
		free(C->stack);
		C->cap = top-&dummy;
		C->stack = malloc(C->cap);
	}
	C->size = top - &dummy;
	
	//注意栈是向下增长的, 所以下面的拷贝实际上拷贝了整个线程的栈，而非只有当前协程的栈.
	memcpy(C->stack, &dummy, C->size);
}

void
coroutine_yield(struct schedule * S) {
	int id = S->running;
	assert(id >= 0);
	struct coroutine * C = S->co[id];
	assert((char *)&C > S->stack);
	_save_stack(C,S->stack + STACK_SIZE);
	C->status = COROUTINE_SUSPEND;
	S->running = -1;
	swapcontext(&C->ctx , &S->main);	//当前上下文保存到 C->ctx 中且切换到 S->main 上下文并执行
}

int 
coroutine_status(struct schedule * S, int id) {
	assert(id>=0 && id < S->cap);
	if (S->co[id] == NULL) {
		return COROUTINE_DEAD;
	}
	return S->co[id]->status;
}

int 
coroutine_running(struct schedule * S) {
	return S->running;
}

