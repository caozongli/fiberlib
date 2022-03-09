#include "co.h"
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include <assert.h>

const int N = 128;
#define STACK_SIZE 1024 * 2

enum co_status {
  CO_NEW = 1, 	// 新创建，还未执行过
  CO_RUNNING,		// 已经执行过了
  CO_WAITTING,	// 在co_wait上等待 
  CO_DEAD,		// 已经结束，但还未释放资源
};

struct co {
  const char *name;
  void (*func)(void *); // co_start 指定的入口地址和参数
  void *arg;
  
  enum co_status 	status; // 协程状态
  struct co *		waiter; // 是否有其他协程在等待当前协程
  jmp_buf			context;// 寄存器线程(setjmp.h)
  unsigned char		stack[STACK_SIZE]; // 协程的堆栈
};

typedef struct CONODE {
  struct co *coroutine;
  struct CONODE *pre, *next;
} CoNode;

static CoNode *co_node = NULL;
static struct co *current = NULL;

static void co_insert(struct co *coroutine) {
  CoNode *p = (CoNode*)malloc(sizeof(CoNode));
  assert(p);
  p->coroutine = coroutine;

  if (!co_node) {
    co_node = p;
	co_node->pre = co_node->next = p;
  } else {
	p->next = co_node;
	p->pre = co_node->pre;
	p->next->pre = p->pre->next = p;
  }
}

static CoNode *co_node_remove() {
  CoNode *p = NULL;

  if (co_node == NULL) return NULL;
  else if (co_node->next == co_node) {
    p = co_node;
	co_node = NULL;
  } else {
	p = co_node;
	co_node->next->pre = co_node->pre;
	co_node->pre->next = co_node->next;
	co_node = co_node->next;
  }
  return p;
}


struct co *co_start(const char *name, void (*func)(void *), void *arg) {
  struct co *cs = (struct co*)malloc(sizeof(struct co));
  assert(cs);
  
  cs->name = name;
  cs->func = func;
  cs->arg = arg;
  cs->status = CO_NEW;
  cs->waiter = NULL;

  co_insert(cs);

  return cs;
}

static inline void stack_switch_call(void *sp, void *entry, void *arg) {
  asm volatile (
#if __x86_64__
  "movq %%rcx, 0(%0); movq %0, %%rsp; movq %2, %%rdi; call *%1"
  :: "b"((uintptr_t)sp - 16), "d"((uintptr_t)entry), "a"((uintptr_t)arg)
#else
  "movl %%ecx, 4(%0); movl %0, %%esp; movl %2, 0(%0); call *%1"
  :: "b"((uintptr_t)sp - 8), "d"((uintptr_t)entry), "a"((uintptr_t)arg)
#endif 
  );
}

static inline void restore_return() {
  asm volatile (
#if __x86_64__
  "movq 0(%%rsp), %%rcx" ::
#else
  "movl 4(%%esp), %%ecx" ::
#endif
  );
}

void co_wait(struct co *cs) {
  assert(cs);
  if (cs->status != CO_DEAD) {
	cs->waiter = current;
	current->status = CO_WAITTING;
	co_yield();
  }

  while (co_node->coroutine != cs) co_node = co_node->next;
  assert(co_node->coroutine == cs);

  free(cs);
  free(co_node_remove());
}

void co_yield() {
  int status = setjmp(current->context);
  if (!status) {
    co_node = co_node->next;

	while (!((current = co_node->coroutine)->status == CO_NEW || current->status == CO_RUNNING)) co_node = co_node->next;

	assert(current);

    if (current->status == CO_RUNNING)
	  longjmp(current->context, 1);
	else {
	  ((struct co volatile*)current)->status = CO_RUNNING;

	  stack_switch_call(current->stack + STACK_SIZE, current->func, current->arg);
	  restore_return();
	  current->status = CO_DEAD;

	  if (current->waiter) current->waiter->status = CO_RUNNING;
	  co_yield();
	}
  }
  assert(status && current->status == CO_RUNNING);
}

static __attribute__((constructor)) void co_constructor(void) {
  current = co_start("main", NULL, NULL);
  current->status = CO_RUNNING;
}

static __attribute__((destructor)) void co_destructor(void) {
  while (co_node) {
    current = co_node->coroutine;
	free(current);
	free(co_node_remove());
  }
}
