# fiberlib

# 基于setjmp/longjmp的协程实现

## setjmp()、longjmp()函数
### goto语句
goto语句又被成为无条件跳转语句，其格式如下：
```c
label：...
/*
执行语句
*/
goto label;
```
优点：它可以无条件的跳转到同一函数内的被标记的语句。
缺点：无法在不同函数间进行跳转
### setjmp、longjmp解决的问题
setjmp和longjmp的存在初衷就是为了解决goto语句只能本地跳转这么一个局限性
setjmp函数动态地建立了一个目标点供longjmp进行返回
程序执行的秘密：
	首先我们要知道的是，程序就是一个状态机，切换过程就是状态机进行转移的过程。而状态指的就是一系列资源在此刻的值，比如pc指针指向的代码行数，此刻堆栈中的信息等。
	有一个想法：将”jmp“点的资源保存下来，待需要jmp的时候再跳转回去岂不是就实现了函数间的跳转，这就相当于将此刻状态保存起来，待到需要的时候再重新将状态提出来继续运行。

#### 函数调用过程：
1.确定好传参方向（左或右）
2.保存返回地址
3.保存调用方栈信息（调用方的栈底位置）
4.更新栈位置到被调用方的栈底
5.在栈内开辟局部变量空间
6.保存寄存器环境
7.执行函数
8.恢复寄存器环境
9.释放局部变量空间
10.恢复栈信息到调用方

#### setjmp
在程序中调用setjmp()时，会将此刻的上下文环境（包括函数栈指针，pc指针等寄存器,信号值等）等保存在struct __jmp_buf_tag中。保存成功会返回0
```c
struct __jmp_buf_tag {
	__jmp_buf __jmpbuf;	//环境
	int	__mask_was_saved; //记录是否保存了信号掩码
	__sigset_t __saved_mask; //信号掩码
};
```

#### longjmp
longjmp(__jmp_buf_tag， value)恢复__jmp_buf_tag所保存的上下文,同时将value作为返回值传给传给保存__jmp_buf_tag的调用点。因为有返回值，就可以分辨此时setjmp的返回是单纯一次执行setjmp还是longjmp进行返回，可以根据返回值做出相应的动作。

## 协程的实现
将协程包装成一个结构体
```c
//协程的状态
enum co_status {
	CO_NEW = 1,		//新创建，还未执行
	CO_RUNNING,		//已经被执行过
	CO_WAITTING,	//在co_wait
	CO_DEAD,		//已结束，但未释放资源
}

struct co {
	const char 		*name; 				//协程名字
	void 			(*func)(void*);		//协程指定入口地址
	void 			*arg;				// 函数参数
	
	enum co_status 	status;				//协程状态
	struct co 		*waiter;			//等待在当前协程下的其他协程
	jmp_buf 		context;			//状态机执行的状态
	unsigned char 	stack[STACK+SIZE]; 	//协程的堆栈
}
```
声明一个协程
```c
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
```

### 切换
在协程切换时，首先需要做的是将当前执行流封存下来，否则，如果你和着急的执行切换，那么当前执行流的栈顶指针将会永远消失，具体来说需要的保存的数据为：
1.所有被调用者保存的通用寄存器
2.所有栈帧上的数据，当co_yield()返回后，汇编代码会持续访问堆栈中的内容
因此我们需要做的事情是：
·为每个协程分配独立的堆栈；栈顶指针由rsp寄存器确定
·在co_yield发生时，将寄存器保存到属于该协程的struct co 中，其实也就是我们的jmp_buf变量
·切换到另一个协程中执行，在系统中找到另一个协程，然后恢复struct co中的寄存器现场
#### 切换的实现
```c
//主要是初次切换需要设置好协程的入口地址，和协程任务的堆栈，初次设置好后，再之后的切换会默认在此堆栈上执行任务
static inline void stack_switch_call(void *sp, void *entry, void *arg) {
   asm volatile (
#if __x86_64__
  	"movq %%rcx, 0(%0); movq %0, %%rsp; movq %2, %%rdi; call *%1"
	::"b"((uintptr_t)sp - 16), "d"((uintptr_t)entry), "a“((uintptr_t)arg)
#else
	 "movl %%ecx, 4(%0); movl %0, %%esp; movl %2, 0(%0); call *%1"
 	:: "b"((uintptr_t)sp - 8), "d"((uintptr_t)entry), "a"(uintptr_t)arg)
#endif
  );
}
```

### 再讲co_yield之前我们还需要明确一下协程的管理方式
协程采用了双向链表进行管理
每个协程是一个node，有指向前后的指针
有一个指向当前的协程
和遍历用的Node
```c
struct CONODE {
	struct co *coroutine;
	struct CONODE *pre, *next;
} CoNode;

static CoNode *co_node = NULL；
static struct co *current = NULL;
```
链表插入----头插法
```c
static void co_insert(struct co *coroutine) {
  CoNode *p = (CoNode*)malloc(sizeof(CoNode));
  assert(p);
  p->coroutine = coroutine;
  
  if (!co_node) {
    co_node = p;
    co_node->pre = co_node->next = p;
  } esle {
    p->next = co_node;
    p->pre = co_node->pre;
    p->next->pre = p->pre->next = p;
  }
}
```

链表移除
```c
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
```

## co_yield()的实现
```c
void co_yield() {
  int status = setjmp(current->context);  // 保存当前状态
  if(!status) {				//根据返回值判断是否为longjmp返回
    co_node = co_node->next;	//将节点往后移
    
    // 当当前节点不是新节点或者并不是在运行中时，当当前节点下移
    while (!((current = co_node->coroutine)->status == CO_NEW || current->status == CO_RUNNING)) co_node = co_node->next;
    
    assert(current);
    
    // 如果是已经运行过的协程那么协程堆栈已经被设置过了直接longjmp即可
    if (current->status == CO_RUNNING) longjmp(current->context, 1);
    else {
    //现将状态置为运行过
      ((stuct co volatile*)current)->status = CO_RUNNING;
      //将协程栈和指令切换
      stack_switch_call (current->stack + STACK_SIZE, current->func, current->arg);
      //由于rsp已经切换了，所以已经在协程任务中运行了
      restore_return();
      //此时已经运行完了将状态置为DEAD
      current->status = CO_DEAD;
      //将等待在此协程的任务唤醒
      if (current->waiter) current->waiter->status = CO_RUNNING;
      //切换出去
      co_yield();
    }
  }
  assert(status && current->status == CO_RUNNING);
}

```

## 协程的回收
```c
void co_wait(struct co *cs) {
  assert(cs);
  //如果此时想要释放的协程的转态还不为DEAD，那么执行此协程并将当前协程挂到后面
  if (cs->status != CO_DEAD) {
    cs->waiter = current;
    current->status = CO_WAITTING;
    co_yield();
  }

  while (co_node->coroutine != cs) co_node = co_node->next; //找下一个可执行协程
  assert(co_node->coroutine == cs);
  free(cs); //释放当前协程
  free(co_node_remove()); // 将协程从协程列表中删除
}
```

## 主协程的实现
利用加载器在执行main函数之前会执行的__attribute__函数，在main执行之前，将main函数包装成协程，这样可以保证线程最后总是会回到main函数执行。
```c
static __attribute__((constructor)) void co_constructor(void) {
  //由于main函数本身是在线程栈上执行的，因此不会与其他协程产生冲突，所以他不用重新设置自己的协程栈
  current = co_start("main", NULL, NULL);
  //设置成可执行状态
  current->status = CO_RUNNING;
}

static __attribute__((destructor)) void co_destructor(void) {
//	回收所有协程
  while (co_node) {
    current = co_node->coroutine;
    free(current);
    free(co_node_remove());
  }
}
