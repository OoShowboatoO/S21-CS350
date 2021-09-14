#include <types.h>
#include <kern/errno.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>
#include "opt-A2.h"
#include <mips/trapframe.h>
#include <vm.h>
#include <vfs.h>
#include <kern/fcntl.h>

  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
#if OPT_A2
  bool has_parent = true;
  
  if(p->parent == NULL) {
    has_parent = false;
  }

  p->terminated = true;
  p->exit_code = exitcode;
  if (has_parent) {
    lock_acquire(p->children_lk);
    cv_signal(p->p_cv,p->children_lk);
    lock_release(p->children_lk);
  }
#endif  // OPT_A2

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
#if OPT_A2
  if (has_parent == false) {
    proc_destroy(p);
  }
#else
  proc_destroy(p);
#endif  // OPT_A2

  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  /* for now, this is just a stub that always returns a PID of 1 */
  /* you need to fix this to make it work properly */
  *retval = 1;

#if OPT_A2
  *retval = curproc->pid;
#endif
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;

  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
  /* for now, just pretend the exitstatus is 0 */
  exitstatus = 0;

#if OPT_A2
  if(pid < 0){
    *retval = -1;
    return(ESRCH);
  }

  bool is_found = false;
  int num_of_children = array_num(curproc->children);
  for (int i = 0; i < num_of_children; i++) {
    struct proc* curr_child = array_get(curproc->children,i);
    if (curr_child->pid == pid) {
      is_found = true;
      lock_acquire(curr_child->children_lk);
      while (curr_child->terminated == false) {   // waiting for the child until it terminates
        cv_wait(curr_child->p_cv, curr_child->children_lk);
      }
      exitstatus = _MKWAIT_EXIT(curr_child->exit_code);
      lock_release(curr_child->children_lk);
      break;
    }
  }
  
  if (is_found == false) {  // child is not found
    *retval = -1;
    return (ECHILD);
  }
  
#endif

  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}


#if OPT_A2
/*********************   SYS_fork   **********************/
int sys_fork(pid_t *retval, struct trapframe *tf) {
  
  KASSERT(tf != NULL);
  KASSERT(curproc->pid > 0);
  
  // Create a new process structure for the child process.
  struct proc* c_proc = proc_create_runprogram("child_process");
  if (c_proc == NULL) { // Check whether there run out of memeory
    // kprintf("<one>.\n");
    return ENOMEM;
  }
  
  struct addrspace *curr_addr_space = curproc_getas();
  
  struct addrspace *new_addr_space = as_create();
  if (new_addr_space == NULL) {
    // kprintf("<one>.\n");
    proc_destroy(c_proc);
    return ENOMEM;
  }
  
  int err_msg = as_copy(curr_addr_space, &new_addr_space);
  if (err_msg != 0) { // // Check whether there run out of memeory
    // kprintf("<two>.\n");
    as_destroy(new_addr_space);
    proc_destroy(c_proc);
    return ENOMEM;
  }
  
  
  // Attach the newly created address space to the 
  //  child process structure.
  spinlock_acquire(&c_proc->p_lock);
  c_proc->p_addrspace = new_addr_space;
  spinlock_release(&c_proc->p_lock);
  
  // Assign a PID to the child process (Done in step 1) and 
  //  create the parent/child relationship.

  spinlock_acquire(&curproc->p_lock);
  array_add(curproc->children, c_proc, NULL);
  c_proc->parent = curproc;
  spinlock_release(&curproc->p_lock);
  
  // Create a thread for child process. The OS needs a safe way to pass the 
  //  trapframe to the child thread.
  // Make a copy on the OS heap without synchronization, which is mentioned 
  //  in A2 guide lecture.
  struct trapframe *tf_copy = kmalloc(sizeof(struct trapframe));
  if(tf_copy == NULL) { // Out of memory
    // kprintf("<three>.\n");
    as_destroy(c_proc->p_addrspace);
    proc_destroy(c_proc);
    return ENOMEM;
  }
  memcpy(tf_copy, tf, sizeof(struct trapframe));

  // The child thread needs to put the trapframe onto the
  //  stack and modify it so that it returns the current value
  //  (and executes the next instruction).
  unsigned long data2 = 0; // We will not use it so just randomly assign a number to it
  err_msg = thread_fork(c_proc->p_name, c_proc, enter_forked_process, (void*)tf_copy, data2);
  if (err_msg != 0) {
    // kprintf("<four>.\n");
    kfree(tf_copy);
    as_destroy(c_proc->p_addrspace);
    proc_destroy(c_proc);
    return ENOMEM;
  }

  // Call mips_usermode() in the child to go back 
  //  to user space. (Done in enter_forked_process())
  *retval = c_proc->pid;
  return 0;
}


/*********************   SYS_execv   **********************/
int
sys_execv(const char *progname, char **args)
{
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;
  
  // make sure the arguments are valid
  KASSERT(progname != NULL);
  KASSERT(args != NULL);

  // copy program name
  size_t actual = 0;
  size_t name_len = sizeof(char) * (strlen(progname) + 1);
  char* name_copy = kmalloc(name_len);
  if (name_copy == NULL) {
    return ENOMEM;
  }
  
  // pass program name into kernel
  int err_msg = copyinstr((const_userptr_t)progname, name_copy, name_len, &actual);
  if (err_msg) {
    kfree(name_copy);
    return err_msg;
  }

  // Count the number of arguments and copy them into the kernel.
  // Copy program args
  int args_count = 0;
  for(int i = 0; args[i] != NULL; i++) {
    args_count += 1;
  }
  
  size_t args_array_size = sizeof(char *) * (args_count + 1);
  char **args_copy = kmalloc(args_array_size);
  if (args_copy == NULL) {
    kfree(name_copy);
    return ENOMEM;
  }

  // Copy the program path from user space into the kernel.
  int MAX_LEN = 128;
  for (int i = 0; i < args_count; i++) {
    args_copy[i] = (char *) kmalloc(sizeof(char) * MAX_LEN);
    if(args_copy[i] == NULL) {
      for (int j = 0; j < i; j++) {
        kfree(args_copy[j]);
      }
      kfree(name_copy);
      kfree(args_copy);
      return ENOMEM;
    }
  }

  for(int i = 0; i < args_count; i++) {
    err_msg = copyinstr((const_userptr_t)args[i], args_copy[i], MAX_LEN, &actual);
    if(err_msg) {
      for (int j = 0; j < args_count; j++) {
        kfree(args_copy[j]);
      }
      kfree(name_copy);
      kfree(args_copy);
      return err_msg;
    }
  }

  args_copy[args_count] = NULL;

	/* Open the file. */
	result = vfs_open(name_copy, O_RDONLY, 0, &v);
	if (result) {
    for (int j = 0; j < args_count; j++) {
      kfree(args_copy[j]);
    }
    kfree(name_copy);
    kfree(args_copy);
		return result;
	}

	/* We should be a new process. */
	// KASSERT(curproc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as ==NULL) {
		vfs_close(v);
    for (int j = 0; j < args_count; j++) {
      kfree(args_copy[j]);
    }
    kfree(name_copy);
    kfree(args_copy);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	struct addrspace *old_as = curproc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
    for (int j = 0; j < args_count; j++) {
      kfree(args_copy[j]);
    }
    kfree(name_copy);
    kfree(args_copy);
    vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
    for (int j = 0; j < args_count; j++) {
      kfree(args_copy[j]);
    }
    kfree(name_copy);
    kfree(args_copy);
    as_destroy(old_as);
		return result;
	}

  // Copy the arguments from the user space into the new address space.
  size_t single_arg_len = 0;
  vaddr_t ptr_size = 4;
  vaddr_t *stack_args = kmalloc(sizeof(vaddr_t) * (args_count + 1));
  if (stack_args == NULL) {
    for (int j = 0; j < args_count; j++) {
        kfree(args_copy[j]);
      }
      kfree(name_copy);
      kfree(args_copy);
      as_destroy(old_as);
      return ENOMEM;
  }
  
  stack_args[args_count] = (vaddr_t) NULL;
  for(int i = args_count - 1; i >= 0; i--) {
    single_arg_len = strlen(args_copy[i]) + 1;
    stackptr -= single_arg_len;
    stack_args[i] = stackptr;
    err_msg = (copyout((char *)args_copy[i], (userptr_t) stackptr, single_arg_len));
    if (err_msg) {
      for (int j = 0; j < args_count; j++) {
        kfree(args_copy[j]);
      }
      kfree(name_copy);
      kfree(args_copy);
      as_destroy(old_as);
      kfree(stack_args);
      return err_msg;
    }
  }

  // Modify stackptr if it is not valid
  vaddr_t reminder = stackptr % ptr_size;
  if (reminder > 0) {
    stackptr -= reminder;
  }

  for (int i = args_count; i >= 0 ; i--){
    stackptr -= ptr_size;
    err_msg = (copyout((vaddr_t *) &stack_args[i], (userptr_t) stackptr, sizeof(vaddr_t)));
    if (err_msg) {
      for (int j = 0; j < args_count; j++) {
        kfree(args_copy[j]);
      }
      kfree(name_copy);
      kfree(args_copy);
      as_destroy(old_as);
      kfree(stack_args);
      return err_msg;
    } 
  }

  // Delete the old address space (if none of the previous steps failed).
  for (int j = 0; j < args_count; j++) {
    kfree(args_copy[j]);
  }
  kfree(name_copy);
  kfree(args_copy);
  as_destroy(old_as);
  kfree(stack_args);

	/* Warp to user mode. */
  // Call enter_new_process with
  // – the address to the arguments on the stack,
  // – the stack pointer (from as_define_stack),
  // – and the program entry point (from vfs_open).
  vaddr_t new_stackptr = ROUNDUP(stackptr, 8);
	enter_new_process(args_count, 
                    (userptr_t) stackptr,
                    new_stackptr, 
                    entrypoint);

  // kprintf("hello~~~\n");
	
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

#endif
