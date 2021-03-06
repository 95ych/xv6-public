#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);


//MLFQ components 
int slices_in_q[5] = {1*5, 2*5, 4*5, 8*5, 16*5};
struct proc* ques[5][NPROC];
int startq[5]={-1,-1,-1,-1,-1};
int endq[5]={-1,-1,-1,-1,-1};

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->ctime = ticks;    // creation time = current time
  p->etime = 0;
  p->rtime = 0;
  p->iotime=0;
  p->priority =60;     // default priority
  #if SCHEDULER == SCHED_MLFQ
    p->age = ticks;
    for(int i=0;i<5;i++)
    {
      p->q[i]=0;
    }
    p->num_run=0;
    ques[0][++endq[0]]=p;
    startq[0]=0
;    p->cur_q=0;
    p->ticks_slice=0;
  #endif

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
  
}



//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  #if SCHEDULER == SCHED_MLFQ
    ques[0][++endq[0]]=np;
    startq[0] =0;
  #endif
  release(&ptable.lock);
  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  curproc->etime = ticks;  //end time = current time

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

int
waitx(int* wtime, int* rtime)                 // modified verision of wait()
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        *rtime= p->rtime;                         //update rtime and wtime;
        *wtime=(p->etime - p->ctime) - p->rtime -p->iotime; 
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

int
set_priority(int new_priority, int pid){
  if(new_priority>100 || new_priority<0)
    return -1;

  acquire(&ptable.lock);
  int pid_valid_flag=0;
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->pid == pid){
      pid_valid_flag=1;
      break;
    }

  if(pid_valid_flag==0){
    release(&ptable.lock);
    return -1;
  }

  int oldpriority = p->priority;
  p->priority = new_priority;
  release(&ptable.lock);
  
  if (new_priority < oldpriority)  //if priority is lifted up, reschedule it
    yield();

  return oldpriority;
}

void
inc_runtime()
{
  
  acquire(&ptable.lock);
  for(struct proc* p=ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if(p->state == RUNNING)
    {
      p->rtime++; 
      p->age=ticks;
      #if SCHEDULER == SCHED_MLFQ
        p->q[p->cur_q]++;
        p->ticks_slice++;
      #endif      
    }
    if(p->state== SLEEPING)
      p->iotime++;
    else if (p->state ==RUNNABLE){ // when in queue waiting for cpu
     #if SCHEDULER == SCHED_MLFQ
        
      #endif
    }
  }
  release(&ptable.lock);

}

int
procsinfo() {
    struct proc *p;
    acquire(&ptable.lock);
    cprintf("PID\tPriority\tState\tr_time\tw_time\tn_run\tcur_q\tq0 q1 q2 q3 q4\n");
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) 
    {
      if(p->state==UNUSED)
        continue;
      cprintf("%d\t%d\t\t",p->pid,p->priority);
      //enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };
      
      if(p->state==EMBRYO)
        cprintf("EMBRYO    ");
      else if(p->state==SLEEPING)
        cprintf("SLEEPING  ");
      else if(p->state==RUNNABLE)
        cprintf("RUNNABLE  ");
      else if(p->state==RUNNING)
        cprintf("RUNNING   ");
      else if(p->state==ZOMBIE)
        cprintf("ZOMBIE    ");
      cprintf("%d\t%d\t%d\t",p->rtime,ticks-p->age,p->num_run);

      #if SCHEDULER == SCHED_MLFQ
        cprintf("%d\t%d  %d  %d  %d  %d\n",p->cur_q,p->q[0],p->q[1],p->q[2],p->q[3],p->q[4]);          
      #else
        cprintf("-\t-  -  -  -  -\n");
      #endif
      //cprintf("ticks_slice-%d\n",p->ticks_slice);
    }
    release(&ptable.lock);
    return 1;
}
//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  #if SCHEDULER == SCHED_RR
  cprintf("THIS IS RR\n");
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      p->num_run++;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
  }
  #elif SCHEDULER == SCHED_FCFS
  cprintf("THIS IS FCFS\n");
  for(;;){
    sti();
    int lowesttime = ticks+99999;
    struct proc* selected_proc=0;
    acquire(&ptable.lock);
    
    for(p=ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state!=RUNNABLE){
        continue;
      }
      if(!selected_proc)
        selected_proc=p;

      if(p->ctime<lowesttime){
        lowesttime = p->ctime;
        selected_proc =p;
      }
    }
    if(selected_proc){
      c->proc=selected_proc; //c proc aka proc to cpu
      switchuvm(selected_proc);
      selected_proc->state = RUNNING;
      selected_proc->num_run++;
      swtch(&(c->scheduler), selected_proc->context);
      switchkvm();
      c->proc = 0;
    }
    release(&ptable.lock);
  }

  #elif SCHEDULER == SCHED_PBS
  cprintf("THIS IS PBS\n");
  for(;;)
  {
    sti();
    acquire(&ptable.lock);
    struct proc *highest_prior_proc=0;
    int highest_priority = 101;
    for(p=ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if(p->state != RUNNABLE)
        continue;
      
      if(highest_priority==101){
        highest_prior_proc = p;  // if highest_prior_proc not assigned , assign to current proc
        highest_priority = highest_prior_proc->priority;
      }
      
      else if(p->priority < highest_priority){  //priorities are like ranks
        highest_prior_proc = p;    // if current proc priority > highest_priority assign to it
        highest_priority = highest_prior_proc->priority;
      }
      // now we got our highest_prior_proc , lets proceed
    }

    if(highest_prior_proc){
      c->proc = highest_prior_proc;
      switchuvm(highest_prior_proc);
      highest_prior_proc->state = RUNNING;
      highest_prior_proc->num_run++;
      swtch(&(c->scheduler),highest_prior_proc->context);
      switchkvm();
      c->proc=0;
    }

    release(&ptable.lock);

  }
  #elif SCHEDULER == SCHED_MLFQ
  cprintf("THIS IS MLFQ\n");
  for(;;)
  {
    sti();
    acquire(&ptable.lock);
    //demoting expired processes
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if(p->state!=RUNNABLE)
        continue;
      if(p->ticks_slice>= slices_in_q[p->cur_q])
      {
        if(p->cur_q!=4){
          int t = p->cur_q;
          int i=0;
          for(i=0;i <= endq[t];i++)
          {
            if(ques[t][i]==p) break;
          }
          //pop that process out of queue
          for(int j=i;j <=endq[t]-1;j++)
          {
            ques[t][j]=ques[t][j+1];
          }
          endq[t]--;
          p->ticks_slice=0;
          p->age=ticks;
          //add to lower queue
          t=++p->cur_q;
          ques[t][++endq[t]]=p;
        }
      }
    }

    //promote worthy processes
    for(p= ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if(p->state != RUNNABLE)
        continue;
      if(ticks - p->age > 1000)
      {
        if(p->cur_q){
          int t = p->cur_q;
          int i=0;
          for(i=0;i <= endq[t];i++)
          {
            if(ques[t][i]==p) break;
          }
          //pop that process out of queue
          for(int j=i;j <=endq[t]-1;j++)
          {
            ques[t][j]=ques[t][j+1];
          }
          endq[t]--;
          p->ticks_slice=0;
          p->age=ticks;
          p->cur_q--;
          //add to higher queue
          t=p->cur_q;
          ques[t][++endq[t]]=p;
        }

      }
    }
    struct proc* selected_proc=0;
    for(int i=0;i<5;i++)
    {
      if(endq[i]==-1)
        continue;
      for(p= ptable.proc; p < &ptable.proc[NPROC]; p++)
      {
        if(p->state != RUNNABLE)
          continue;
        
        if(p->cur_q==i){
          selected_proc=p;
          goto runproc;
        }
        
      }
    }

    runproc:
      if(selected_proc)
      {
        selected_proc->num_run++;
        p=selected_proc;
        //pop it out from queue---
        int t = p->cur_q;     // selected_proc = p
        int i=0;
        for(i=0;i <= endq[t];i++)
        {
    
          if(ques[t][i]==p) break;
        }
        for(int j=i;j <=endq[t]-1;j++)
        {
          ques[t][j]=ques[t][j+1];
        }
        endq[t]--;
        p->ticks_slice=0;
        p->age=ticks;
        //--------
        c->proc =selected_proc;
        switchuvm(selected_proc);
        selected_proc->state = RUNNING;
        swtch(&(c->scheduler), selected_proc->context);
        switchkvm();
        c->proc = 0;



        //in case of voluntary relinquishment and if still runnable,  rejoin same queue
        if(selected_proc->state ==RUNNABLE){
          p->age=ticks;
          p->ticks_slice=0;
          ques[t][++endq[t]]=p;
        }


      }

      release(&ptable.lock);

  }
  #endif
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
      #if SCHEDULER== SCHED_MLFQ
        ques[p->cur_q][++endq[p->cur_q]]=p;
        p->ticks_slice=0;
        p->age=0;
      #endif
    }

}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING){
        p->state = RUNNABLE;
      #if SCHEDULER== SCHED_MLFQ
        ques[p->cur_q][++endq[p->cur_q]]=p;
        p->ticks_slice=0;
        p->age=0;
      #endif

      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
