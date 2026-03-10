#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;
  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);
  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  struct proc *p = myproc();

  if(tf->trapno == T_SYSCALL){
    if(p->killed)
      exit();
    p->tf = tf;
    syscall();
    if(p->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;

  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;

  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;

  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;

  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;

  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  case T_PGFLT: {
    uint addr = rcr2();  // faulting virtual address from CR2

    if((tf->cs & 3) == DPL_USER && p){
      // User-mode page fault: print diagnostics and kill process
      cprintf("pid %d (%s) page fault addr 0x%x eip 0x%x\n",
              p->pid, p->name, addr, tf->eip);
      if(addr < PGSIZE)
        cprintf("NULL pointer dereference detected!\n");
      p->killed = 1;
    } else {
      // Kernel-mode page fault: panic
      cprintf("kernel page fault addr 0x%x eip 0x%x\n", addr, tf->eip);
      panic("kernel page fault");
    }
    break;
  }

  default:
    if(p == 0 || (tf->cs & 3) == 0){
      // Kernel trap: must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // User-space trap: kill process
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x -- kill proc\n",
            p->pid, p->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    p->killed = 1;
  }

  // Force process exit if killed and in user space
  if(p && p->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Yield CPU on timer tick
  if(p && p->state == RUNNING && tf->trapno == T_IRQ0 + IRQ_TIMER)
    yield();

  // Check again if killed after yielding
  if(p && p->killed && (tf->cs & 3) == DPL_USER)
    exit();
}