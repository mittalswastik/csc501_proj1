//////////////////////////////////////////////////////////////////////
//                      North Carolina State University
//
//
//
//                             Copyright 2016
//
////////////////////////////////////////////////////////////////////////
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
// This program is distributed in the hope it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
//
////////////////////////////////////////////////////////////////////////
//
//   Author:  Hung-Wei Tseng, Yu-Chia Liu
//
//   Description:
//     Core of Kernel Module for Processor Container
//
////////////////////////////////////////////////////////////////////////

#include "processor_container.h"

#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/kthread.h>

// libraries added

#include <linux/list.h>
#include <linux/types.h>

/*

    Idea

    We need to implement a data structure to handle the contaianers and also the task within those containers

    We plan on creating a hash table with key entry as the container id and each entry having a list of tasks
    These task would be pointing to next task within the same contaianer and last one points to first(circular linked list)
    We create hash table for container because it has continuos id - 0,1,2... which are not random hence we do not need to 
    worry aboout creating a hash function. Whereas for task within a id we have pid(process id) which is of random form hence
    difficult to manipulate hash key also when hash table would be dynamically increasing and decreasing

    Create

    Each create would either add a task to the list or resize the table to add a container
    As hash map searches in O(1) and link list adds as well in O(1) hence new task create for existing
    container id is O(1). For new container id we first need to resize the hash table

    [cid1]  -> task1 -> task2 ->task3 -> task4 ->task1
    [cid2]
    [cid3]
    [cid4]    

    Here we are setting default containers to 10000, Will use krealloc to reallocate the array size to get new containers range

    Delete

    Every delete would remove element from the list which has to be tracked by reading the complete list for specific container id
    hence O(n) solution.


    Switch

    Switch is called every 5ms as user library uses SIG system call to ask kernel to switch

    We have a current task in each resource container running. So whenever a switch is called We schedule next task of the same resource container
    hence we first need to identify the container.

*/

struct task_info {
    long long int cid;
    pid_t tid;
    struct task_info *next;
    struct task_struct *taskinlist;         // pointing to its corresponding structure in kernel run queue
};

struct container_info {
    struct task_info *head;
    struct task_info *foot;
    struct task_info *cur;
};

struct container_info *containers[10000];

struct mutex lockproc; 

DEFINE_MUTEX(lockproc);

/**
 * Delete the task in the container.
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), 
 */

int processor_container_delete(struct processor_container_cmd __user *user_cmd)
{
    mutex_lock(&lockproc);

    struct processor_container_cmd *temp;

    temp = kmalloc(sizeof(struct processor_container_cmd), GFP_KERNEL);

    copy_from_user(temp,user_cmd, sizeof(struct processor_container_cmd));

    printk(KERN_INFO "to delete task with cid %llu", temp->cid);

    long long int x = temp->cid;

    struct task_struct *currenttask = current;

    struct task_info *del, *prev;

    if(containers[x]->head == NULL) {
        printk(KERN_INFO "no such task found");
        // it would never be here
    }

    else {

        if(containers[x]->head->next == containers[x]->head) {
            printk(KERN_INFO "last task of container %llu",x);
            // only one task left within the container
            del = containers[x]->head;
            kfree(del);
            containers[x]->head = NULL;
        }

        else {

            del = containers[x]->head->next;
            prev = containers[x]->head;

            if(prev->tid == currenttask->pid){

                // Deleting the 1st task requires to change head and also last task pointing to first

                printk(KERN_INFO "deleting task with tid %d from container %llu",prev->tid,x);

                containers[x]->head = prev->next;
                containers[x]->foot->next = containers[x]->head;
                containers[x]->cur = prev->next;
                kfree(prev);
            }

            while(del != containers[x]->head) {
                if(del->tid == currenttask->pid) {
                    printk(KERN_INFO "deleting task with tid %d from container %llu",del->tid,x);
                    prev->next = del->next;
                    kfree(del);
                    containers[x]->cur = prev->next;
                    break;
                }

                prev = prev->next;
                del = del->next;
            }

            printk(KERN_INFO "waking up %d after delete in container %llu",containers[x]->cur->tid,x);

            wake_up_process(containers[x]->cur->taskinlist);

        }

    }

    mutex_unlock(&lockproc);

    return 0;
}

/**
 * Create a task in the corresponding container.
 * external functions needed:
 * copy_from_user(), mutex_lock(), mutex_unlock(), set_current_state(), schedule()
 * 
 * external variables needed:
 * struct task_struct* current  
 */

int processor_container_create(struct processor_container_cmd __user *user_cmd)
{

    mutex_lock(&lockproc);

    struct processor_container_cmd *temp;    

    temp = kmalloc(sizeof(struct processor_container_cmd), GFP_KERNEL);

    copy_from_user(temp,user_cmd, sizeof(struct processor_container_cmd));

    printk(KERN_INFO "to add task with cid %llu", temp->cid);

    struct task_info *task = kmalloc(sizeof(struct task_info), GFP_KERNEL);

    struct task_struct *currenttask = current;     // can directly use current->pid though.

    long long int x = temp->cid;    // check if _u64 = long long int

    if(containers[x] == NULL) {

            printk(KERN_INFO "new container");
            printk(KERN_INFO "creating task with id %d", currenttask->pid);
            task->cid = x;
            task->tid = currenttask->pid;
            task->taskinlist = currenttask;

            struct container_info *container = kmalloc(sizeof(struct container_info), GFP_KERNEL);

            containers[x] = container;

            containers[x]->head = task;
            containers[x]->foot = task;
            containers[x]->foot->next = containers[x]->head;
            containers[x]->cur = containers[x]->head;

            mutex_unlock(&lockproc);
        }

    else {

        printk(KERN_INFO "container already here");
        printk(KERN_INFO "creating task with id %d", currenttask->pid);
        task->cid = x;
        task->tid = currenttask->pid;
        task->taskinlist = currenttask;

        containers[x]->foot->next = task;
        containers[x]->foot = task;
        containers[x]->foot->next = containers[x]->head;

        mutex_unlock(&lockproc);
        set_current_state(TASK_INTERRUPTIBLE);
        schedule();
    }
    
    return 0;
}

/**
 * switch to the next task in the next container
 * 
 * external functions needed:
 * mutex_lock(), mutex_unlock(), wake_up_process(), set_current_state(), schedule()
 */
int processor_container_switch(struct processor_container_cmd __user *user_cmd)
{

    mutex_lock(&lockproc);

    struct processor_container_cmd *temp;   

    temp = kmalloc(sizeof(struct processor_container_cmd), GFP_KERNEL);

    copy_from_user(temp,user_cmd, sizeof(struct processor_container_cmd));

    long long int x = temp->cid;
    long long int counter;
    long long int i;

    for(i = 0 ; i < 10000 ; i++) {
        if(containers[i] != NULL) {

            if(containers[i]->cur->tid == current->pid) {
                counter = i;
                break;
            }
        }
    }

    printk(KERN_INFO "task provided by container %llu ",counter);

    if(containers[counter]->cur == containers[counter]->cur->next){
        printk(KERN_INFO "scheduling %d and not putting process id %d in container %llu to sleep",containers[counter]->cur->next->tid,containers[counter]->cur->tid,counter);
        mutex_unlock(&lockproc); 
    }

    else {

        printk(KERN_INFO "scheduling %d and putting process id %d in container %llu to sleep",containers[counter]->cur->next->tid,containers[counter]->cur->tid,counter);
        
        containers[counter]->cur = containers[counter]->cur->next;
    
        wake_up_process(containers[counter]->cur->taskinlist);

        mutex_unlock(&lockproc);

        set_current_state(TASK_INTERRUPTIBLE);

        schedule();
    }

    return 0;
}

/**
 * control function that receive the command in user space and pass arguments to
 * corresponding functions.
 */
int processor_container_ioctl(struct file *filp, unsigned int cmd,
                              unsigned long arg)
{
    switch (cmd)
    {
    case PCONTAINER_IOCTL_CSWITCH:
        return processor_container_switch((void __user *)arg);
    case PCONTAINER_IOCTL_CREATE:
        return processor_container_create((void __user *)arg);
    case PCONTAINER_IOCTL_DELETE:
        return processor_container_delete((void __user *)arg);
    default:
        return -ENOTTY;
    }
}


// realloc

// (void*) tasks = krealloc((void *) tasks, (sizeof(tasks)+sizeof(struct task_info)), GFP_KERNEL);