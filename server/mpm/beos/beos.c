/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2001 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 *
 * Portions of this software are based upon public domain software
 * originally written at the National Center for Supercomputing Applications,
 * University of Illinois, Urbana-Champaign.
 */

/* The new BeOS MPM!
 *
 * This one basically is a single process multi threaded model, but 
 * I couldn't be bothered adding the spmt_ to the front of the name!
 * Anyway, this is still under development so it isn't yet the default
 * choice.
 */
 
#define CORE_PRIVATE 
 
#include "apr_strings.h"
#include "apr_portable.h"
#include "httpd.h" 
#include "http_main.h" 
#include "http_log.h" 
#include "http_config.h"	/* for read_config */ 
#include "http_core.h"		/* for get_remote_host */ 
#include "http_connection.h"
#include "ap_mpm.h"
#include "beosd.h"
#include "ap_listen.h"
#include "scoreboard.h" 
#include <kernel/OS.h>
#include "mpm_common.h"
#include "mpm.h"
#include <unistd.h>
#include <sys/socket.h>
#include <signal.h>

/*
 * Actual definitions of config globals
 */

int ap_threads_per_child=HARD_THREAD_LIMIT;         /* Worker threads per child */
int ap_max_requests_per_child=0;
static const char *ap_pid_fname=NULL;
static int ap_threads_to_start=0;
static int min_spare_threads=0;
static int max_spare_threads=0;
static int ap_thread_limit=0;
static int num_listening_sockets = 0; /* set by open_listeners in ap_mpm_run */
static apr_socket_t ** listening_sockets;
apr_lock_t *accept_mutex = NULL;

static apr_pool_t *pconf;		/* Pool for config stuff */
static apr_pool_t *pchild;		/* Pool for httpd child stuff */

static int server_pid; 

/* Keep track of the number of worker threads currently active */
static int worker_thread_count;
apr_lock_t *worker_thread_count_mutex;

/* The structure used to pass unique initialization info to each thread */
typedef struct {
    int slot;
    apr_pool_t *tpool;
} proc_info;

/*
 * The max child slot ever assigned, preserved across restarts.  Necessary
 * to deal with MaxClients changes across SIGWINCH restarts.  We use this
 * value to optimize routines that have to scan the entire scoreboard.
 */
int ap_max_child_assigned = -1;
int ap_max_threads_limit = -1;
char ap_coredump_dir[MAX_STRING_LEN];


static volatile bool flag_of_death;
static int udp_sock;
static struct sockaddr_in udpsi;

/* shared http_main globals... */

server_rec *ap_server_conf;

/* one_process */
static int one_process = 0;

#ifdef DEBUG_SIGSTOP
int raise_sigstop_flags;
#endif

AP_DECLARE(int) ap_get_max_daemons(void)
{
    return ap_max_child_assigned;
}

/* a clean exit from a child with proper cleanup 
   static void clean_child_exit(int code) __attribute__ ((noreturn)); */
static void clean_child_exit(int code)
{
    if (pchild)
        apr_pool_destroy(pchild);
    exit(code);
}

/* handle all varieties of core dumping signals */
static void sig_coredump(int sig)
{
    chdir(ap_coredump_dir);
    signal(sig, SIG_DFL);
    kill(server_pid, sig);
    /* At this point we've got sig blocked, because we're still inside
     * the signal handler.  When we leave the signal handler it will
     * be unblocked, and we'll take the signal... and coredump or whatever
     * is appropriate for this particular Unix.  In addition the parent
     * will see the real signal we received -- whereas if we called
     * abort() here, the parent would only see SIGABRT.
     */
}

/*****************************************************************
 * Connection structures and accounting...
 */

/* volatile just in case */
static int volatile shutdown_pending;
static int volatile restart_pending;
static int volatile is_graceful;
ap_generation_t volatile ap_my_generation = 0;

/*
 * ap_start_shutdown() and ap_start_restart(), below, are a first stab at
 * functions to initiate shutdown or restart without relying on signals. 
 * Previously this was initiated in sig_term() and restart() signal handlers, 
 * but we want to be able to start a shutdown/restart from other sources --
 * e.g. on Win32, from the service manager. Now the service manager can
 * call ap_start_shutdown() or ap_start_restart() as appropiate.  Note that
 * these functions can also be called by the child processes, since global
 * variables are no longer used to pass on the required action to the parent.
 *
 * These should only be called from the parent process itself, since the
 * parent process will use the shutdown_pending and restart_pending variables
 * to determine whether to shutdown or restart. The child process should
 * call signal_parent() directly to tell the parent to die -- this will
 * cause neither of those variable to be set, which the parent will
 * assume means something serious is wrong (which it will be, for the
 * child to force an exit) and so do an exit anyway.
 */

static void ap_start_shutdown(void)
{
    if (shutdown_pending == 1) {
	/* Um, is this _probably_ not an error, if the user has
	 * tried to do a shutdown twice quickly, so we won't
	 * worry about reporting it.
	 */
	return;
    }
    shutdown_pending = 1;
}

/* do a graceful restart if graceful == 1 */
static void ap_start_restart(int graceful)
{

    if (restart_pending == 1) {
        /* Probably not an error - don't bother reporting it */
        return;
    }
    restart_pending = 1;
    is_graceful = graceful;
    if (is_graceful){
        apr_pool_cleanup_kill(pconf, NULL, ap_cleanup_scoreboard);
    }
}

static void sig_term(int sig)
{
    ap_start_shutdown();
}

static void restart(int sig)
{
    ap_start_restart(sig == SIGWINCH);
}

static void tell_workers_to_exit(void)
{
    flag_of_death = true;
    sendto(udp_sock, "die!", 4, 0, (struct sockaddr*)&udpsi, sizeof(udpsi));
}

static void set_signals(void)
{
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (!one_process) {
	sa.sa_handler = sig_coredump;

	if (sigaction(SIGSEGV, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGSEGV)");
	if (sigaction(SIGBUS, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGBUS)");
	if (sigaction(SIGABRT, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGABRT)");
	if (sigaction(SIGILL, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGILL)");
	sa.sa_flags = 0;
    }
    sa.sa_handler = sig_term;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGTERM)");
    if (sigaction(SIGINT, &sa, NULL) < 0)
        ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGINT)");
    
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0)
    	ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGPIPE)");

    /* we want to ignore HUPs and WINCH while we're busy processing one */
    sigaddset(&sa.sa_mask, SIGHUP);
    sigaddset(&sa.sa_mask, SIGWINCH);
    sa.sa_handler = restart;
    if (sigaction(SIGHUP, &sa, NULL) < 0)
    	ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGHUP)");
    if (sigaction(SIGWINCH, &sa, NULL) < 0)
	    ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf, "sigaction(SIGWINCH)");
}

/*****************************************************************
 * Here follows a long bunch of generic server bookkeeping stuff...
 */

int ap_graceful_stop_signalled(void)
{
    /* XXX - Does this really work? - Manoj */
    return is_graceful;
}

/*****************************************************************
 * Child process main loop.
 */

static void process_socket(apr_pool_t *p, apr_socket_t *sock, int my_child_num)
{
    conn_rec *current_conn;
    long conn_id = my_child_num;
    int csd;

    (void)apr_os_sock_get(&csd, sock);
    
    if (csd >= FD_SETSIZE) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, NULL,
            "filedescriptor (%u) larger than FD_SETSIZE (%u) "
            "found, you probably need to rebuild Apache with a "
            "larger FD_SETSIZE", csd, FD_SETSIZE);
        apr_socket_close(sock);
	    return;
    }

    current_conn = ap_new_connection(p, ap_server_conf, sock, conn_id);

    if (current_conn) {
        ap_process_connection(current_conn);
        ap_lingering_close(current_conn);
    }
}

static int32 worker_thread(void * dummy)
{
    proc_info * ti = dummy;
    int child_slot = ti->slot;
    apr_pool_t *tpool = ti->tpool;
    apr_socket_t *csd = NULL;
    apr_pool_t *ptrans;		/* Pool for per-transaction stuff */
    apr_socket_t *sd = NULL;
    apr_status_t rv = APR_EINIT;
    int srv , n;
    int curr_pollfd, last_pollfd = 0;
    sigset_t sig_mask;
    int requests_this_child = ap_max_requests_per_child;
    apr_pollfd_t *pollset;
    /* each worker thread is in control of it's own destiny...*/
    int this_worker_should_exit = 0; 
    free(ti);

    /* block the signals for this thread */
    sigfillset(&sig_mask);
    sigprocmask(SIG_BLOCK, &sig_mask, NULL);

    apr_pool_create(&ptrans, tpool);

    apr_lock_acquire(worker_thread_count_mutex);
    worker_thread_count++;
    apr_lock_release(worker_thread_count_mutex);

    (void) ap_update_child_status(0, child_slot, SERVER_STARTING,
                                  (request_rec*)NULL);
                                  
    apr_poll_setup(&pollset, num_listening_sockets, tpool);
    for(n=0 ; n <= num_listening_sockets ; ++n)
	    apr_poll_socket_add(pollset, listening_sockets[n], APR_POLLIN);

    while (!this_worker_should_exit) {
        this_worker_should_exit |= (ap_max_requests_per_child != 0) && (requests_this_child <= 0);
        
        if (this_worker_should_exit) break;

        (void) ap_update_child_status(0, child_slot, SERVER_READY,
                                      (request_rec*)NULL);

        apr_lock_acquire(accept_mutex);
        while (!this_worker_should_exit) {
            apr_int16_t event;
            apr_status_t ret = apr_poll(pollset, &srv, -1);
            
            if (flag_of_death)
                this_worker_should_exit = 1;

            apr_poll_revents_get(&event, listening_sockets[0], pollset);
            if (event & APR_POLLIN)
                this_worker_should_exit = 1;
            
            if (ret != APR_SUCCESS) {
                if (errno == EINTR) {
                    continue;
                }

                /* poll() will only return errors in catastrophic
                 * circumstances. Let's try exiting gracefully, for now. */
                ap_log_error(APLOG_MARK, APLOG_ERR, ret, (const server_rec *)
                             ap_server_conf, "apr_poll: (listen)");
                this_worker_should_exit = 1;
            }

            if (this_worker_should_exit) break;

            if (num_listening_sockets == 1) {
                sd = ap_listeners->sd;
                goto got_fd;
            }
            else {
                /* find a listener */
                curr_pollfd = last_pollfd;
                do {
                    curr_pollfd++;
                    if (curr_pollfd > num_listening_sockets) {
                        curr_pollfd = 1;
                    }
                    /* Get the revent... */
                    apr_poll_revents_get(&event, listening_sockets[curr_pollfd], pollset);
                    
                    if (event & APR_POLLIN) {
                        last_pollfd = curr_pollfd;
                        sd = listening_sockets[curr_pollfd];
                        goto got_fd;
                    }
                } while (curr_pollfd != last_pollfd);
            }
        }
    got_fd:
        if (!this_worker_should_exit) {
            rv = apr_accept(&csd, sd, ptrans);
            apr_lock_release(accept_mutex);
            if (rv != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, rv, ap_server_conf,
                  "apr_accept");
            } else {
                process_socket(ptrans, csd, child_slot);
                requests_this_child--;
            }
        }
        else {
            apr_lock_release(accept_mutex);
            break;
        }
        apr_pool_clear(ptrans);
    }

    ap_update_child_status(0, child_slot, SERVER_DEAD, (request_rec*)NULL);
    
    apr_lock_acquire(worker_thread_count_mutex);
    worker_thread_count--;
    apr_lock_release(worker_thread_count_mutex);

    return (0);
}

static int make_worker(server_rec *s, int slot)
{
    thread_id tid;
    proc_info *my_info = (proc_info *)malloc(sizeof(proc_info)); /* freed by thread... */

    if (my_info == NULL) {
        ap_log_error(APLOG_MARK, APLOG_ALERT, errno, ap_server_conf,
            "malloc: out of memory");
        clean_child_exit(APEXIT_CHILDFATAL);
    }
    
    my_info->slot = slot;
    apr_pool_create(&my_info->tpool, pchild);
    
    if (slot + 1 > ap_max_child_assigned)
	    ap_max_child_assigned = slot + 1;

    if (one_process) {
    	set_signals();
        ap_scoreboard_image->parent[0].pid = getpid();
        return 0;
    }

    (void) ap_update_child_status(0, slot, SERVER_STARTING, (request_rec*)NULL);
    tid = spawn_thread(worker_thread, "apache_worker", B_NORMAL_PRIORITY,
        my_info);
    if (tid < B_NO_ERROR) {
        ap_log_error(APLOG_MARK, APLOG_ERR, errno, s, 
            "spawn_thread: Unable to start a new thread");
	    /* In case system resources are maxxed out, we don't want
	     * Apache running away with the CPU trying to fork over and
	     * over and over again. */
        (void) ap_update_child_status(0, slot, SERVER_DEAD, 
                                      (request_rec*)NULL);
        
    	sleep(10);
        free(my_info);
        
    	return -1;
    }
    resume_thread(tid);

    ap_scoreboard_image->servers[0][slot].tid = tid;
    return 0;
}

/* start up a bunch of children */
static void startup_threads(int number_to_start)
{
    int i;

   ap_log_error(APLOG_MARK, APLOG_ERR, 0, NULL, 
                "starting %d threads", number_to_start);

    for (i = 0; number_to_start && i < ap_thread_limit; ++i) {
	if (ap_scoreboard_image->servers[0][i].tid) {
	    continue;
	}
	if (make_worker(ap_server_conf, i) < 0) {
	    break;
	}
	--number_to_start;
    }
}


/*
 * spawn_rate is the number of children that will be spawned on the
 * next maintenance cycle if there aren't enough idle servers.  It is
 * doubled up to MAX_SPAWN_RATE, and reset only when a cycle goes by
 * without the need to spawn.
 */
static int spawn_rate = 1;
#ifndef MAX_SPAWN_RATE
#define MAX_SPAWN_RATE	(32)
#endif
static int hold_off_on_exponential_spawning;

static void perform_idle_server_maintenance(void)
{
    int i;
    int free_length;
    int free_slots[MAX_SPAWN_RATE];
    int last_non_dead;

    /* initialize the free_list */
    free_length = 0;

    for (i = 0; i < ap_thread_limit; ++i) {
        if (ap_scoreboard_image->servers[0][i].tid == 0) {
            if (free_length < spawn_rate) {
                free_slots[free_length] = i;
                ++free_length;
            }
        }
        else {
            last_non_dead = i;
        }

    	if (i >= ap_max_child_assigned && free_length >= spawn_rate) {
	         break;
	    }
    }
    ap_max_child_assigned = last_non_dead + 1;

    if (free_length > 0) {
    	for (i = 0; i < free_length; ++i) {
	        make_worker(ap_server_conf, free_slots[i]);
	    }
	    /* the next time around we want to spawn twice as many if this
	     * wasn't good enough, but not if we've just done a graceful
	     */
	    if (hold_off_on_exponential_spawning) {
	        --hold_off_on_exponential_spawning;
	    } else if (spawn_rate < MAX_SPAWN_RATE) {
	        spawn_rate *= 2;
	    }
    } else {
        spawn_rate = 1;
    }
}

static void server_main_loop(int remaining_threads_to_start)
{
    int child_slot;
    apr_wait_t status;
    apr_proc_t pid;
    int i;

    while (!restart_pending && !shutdown_pending) {
        ap_wait_or_timeout(&status, &pid, pconf);
         
        if (pid.pid >= 0) {
            ap_process_child_status(&pid, status);
            /* non-fatal death... note that it's gone in the scoreboard. */
            child_slot = -1;
            for (i = 0; i < ap_max_child_assigned; ++i) {
        	if (ap_scoreboard_image->servers[0][i].tid == pid.pid) {
                    child_slot = i;
                    break;
                }
            }
            if (child_slot >= 0) {
                ap_scoreboard_image->servers[0][child_slot].tid = 0;
                (void) ap_update_child_status(0, child_slot, SERVER_DEAD, (request_rec*)NULL);
                
                if (remaining_threads_to_start
		            && child_slot < ap_thread_limit) {
                    /* we're still doing a 1-for-1 replacement of dead
                     * children with new children
                     */
                    make_worker(ap_server_conf, child_slot);
                    --remaining_threads_to_start;
		        }
#if APR_HAS_OTHER_CHILD
            }
            else if (apr_proc_other_child_read(&pid, status) == 0) {
    		/* handled */
#endif
            }
            else if (is_graceful) {
                /* Great, we've probably just lost a slot in the
                 * scoreboard.  Somehow we don't know about this
                 * child.
                 */
                 ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, ap_server_conf,
			                  "long lost child came home! (pid %ld)", pid.pid);
            }
	    
            /* Don't perform idle maintenance when a child dies,
             * only do it when there's a timeout.  Remember only a
             * finite number of children can die, and it's pretty
             * pathological for a lot to die suddenly.
             */
             continue;
         }
	     else if (remaining_threads_to_start) {
             /* we hit a 1 second timeout in which none of the previous
              * generation of children needed to be reaped... so assume
              * they're all done, and pick up the slack if any is left.
              */
              startup_threads(remaining_threads_to_start);
              remaining_threads_to_start = 0;
              /* In any event we really shouldn't do the code below because
               * few of the servers we just started are in the IDLE state
               * yet, so we'd mistakenly create an extra server.
               */
              continue;
         }
         perform_idle_server_maintenance();
    }
}

int ap_mpm_run(apr_pool_t *_pconf, apr_pool_t *plog, server_rec *s)
{
    int remaining_threads_to_start, i,j;
    apr_status_t rv;
    ap_listen_rec *lr;    
    pconf = _pconf;
    ap_server_conf = s;

    ap_scoreboard_fname = DEFAULT_SCOREBOARD;

    if ((udp_sock = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP)) < 0){
        ap_log_error(APLOG_MARK, APLOG_ALERT, errno, s,
            "couldn't create control socket, shutting down");
        return 1;
    }

    /* now build the udp sockaddr_in structure... */
#ifdef BEOS_BONE
    udpsi.sin_len = sizeof(struct sockaddr_in);
#endif
    udpsi.sin_family = AF_INET;
    udpsi.sin_port = htons(7777);
    udpsi.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(udp_sock, (struct sockaddr*)&udpsi, sizeof(udpsi)) < 0){
        ap_log_error(APLOG_MARK, APLOG_ALERT, errno, s,
            "couldn't bind the control socket, shutting down");
        return 1;
    }

    if ((num_listening_sockets = ap_setup_listeners(ap_server_conf)) < 1) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_ALERT, 0, s,
            "no listening sockets available, shutting down");
        return 1;
    }

    ap_log_pid(pconf, ap_pid_fname);
    flag_of_death = false;
        
    /*
     * Create our locks... 
     */
    
    /* accept_mutex
     * used to lock around select so we only have one thread
     * in select at a time
     */
    if ((rv = apr_lock_create(&accept_mutex, APR_MUTEX, APR_CROSS_PROCESS,
        NULL, pconf)) != APR_SUCCESS) {
        /* tsch tsch, can't have more than one thread in the accept loop
           at a time so we need to fall on our sword... */
        ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s,
                     "Couldn't create accept lock");
        return 1;
    }

    /* worker_thread_count_mutex
     * locks the worker_thread_count so we have ana ccurate count...
     */
    if ((rv = apr_lock_create(&worker_thread_count_mutex, APR_MUTEX, APR_CROSS_PROCESS,
        NULL, pconf)) != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_EMERG, rv, s,
                     "Couldn't create worker thread count lock");
        return 1;
    }

    /*
     * Startup/shutdown... 
     */
    
    if (!is_graceful)
        ap_create_scoreboard(pconf, SB_SHARED);

    if (!is_graceful) {
        for (i = 0; i < HARD_SERVER_LIMIT; i++) {
            ap_scoreboard_image->parent[i].pid = 0;
            for (j = 0;j < HARD_THREAD_LIMIT; j++)
                ap_scoreboard_image->servers[i][j].tid = 0;
        }
    }
    if (HARD_SERVER_LIMIT == 1)
        ap_scoreboard_image->parent[0].pid = getpid();

    set_signals();

    /* Sanity checks to avoid thrashing... */
    if (max_spare_threads < min_spare_threads )
        max_spare_threads = min_spare_threads;

    /* If we're doing a graceful_restart then we're going to see a lot
     * of threads exiting immediately when we get into the main loop
     * below (because we just sent them SIGWINCH).  This happens pretty
     * rapidly... and for each one that exits we'll start a new one until
     * we reach at least threads_min_free.  But we may be permitted to
     * start more than that, so we'll just keep track of how many we're
     * supposed to start up without the 1 second penalty between each fork.
     */
    remaining_threads_to_start = ap_threads_to_start;
    /* sanity check on the number to start... */
    if (remaining_threads_to_start > ap_thread_limit) {
	    remaining_threads_to_start = ap_thread_limit;
    }

    /* setup the child pool to use for the workers.  Each worker creates
     * a seperate pool of it's own to use.
     */
    apr_pool_create(&pchild, pconf);
    ap_run_child_init(pchild, ap_server_conf);

    /* Now that we have the child pool (pchild) we can allocate
     * the listenfds and creat the pollset...
     */
    listening_sockets = apr_palloc(pchild,
       sizeof(*listening_sockets) * (num_listening_sockets + 1));

    apr_os_sock_put(&listening_sockets[0], &udp_sock, pchild);

    for (lr = ap_listeners, i = 1; i <= num_listening_sockets; lr = lr->next, ++i)
	    listening_sockets[i]=lr->sd;

    /* we assume all goes OK...hmm might want to check that! */
    if (!is_graceful) {
	    startup_threads(remaining_threads_to_start);
	    remaining_threads_to_start = 0;
    }
    else {
	    /* give the system some time to recover before kicking into
	     * exponential mode */
        hold_off_on_exponential_spawning = 10;
    }

    /*
     * record that we've entered the world !
     */
    ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, ap_server_conf,
		"%s configured -- resuming normal operations",
		ap_get_server_version());

    ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_INFO, 0, ap_server_conf,
		"Server built: %s", ap_get_server_built());

    restart_pending = shutdown_pending = 0;

    /*
     * main_loop until it's all over
     */
    server_main_loop(remaining_threads_to_start);
    
    tell_workers_to_exit(); /* if we get here we're exiting... */
    sleep(1); /* give them a brief chance to exit */

    /* close the UDP socket we've been using... */
    apr_socket_close(listening_sockets[0]);
        
    /*
     * If we get here we're shutting down...
     */
    if (shutdown_pending) {
        /* Time to gracefully shut down:
         * Kill child processes, tell them to call child_exit, etc...
         */
        if (beosd_killpg(getpgrp(), SIGTERM) < 0)
            ap_log_error(APLOG_MARK, APLOG_WARNING, errno, ap_server_conf,
             "killpg SIGTERM");

        /* cleanup pid file on normal shutdown */
        {
            const char *pidfile = NULL;
            pidfile = ap_server_root_relative (pconf, ap_pid_fname);
            if ( pidfile != NULL && unlink(pidfile) == 0)
                ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_INFO,
            		 0, ap_server_conf,
            		 "removed PID file %s (pid=%ld)",
            		 pidfile, (long)getpid());
        }
        
        /* use ap_reclaim_child_processes starting with SIGTERM */
        ap_reclaim_child_processes(1);

        /* record the shutdown in the log */
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, ap_server_conf,
            "caught SIGTERM, shutting down");
    
        return 1;
    }

    /* we've been told to restart */
    signal(SIGHUP, SIG_IGN);

    if (one_process) {
        return 1;
    }

    if (is_graceful) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, ap_server_conf,
		    "SIGWINCH received.  Doing graceful restart");
    }
    else {
        /* Kill 'em all.  Since the child acts the same on the parents SIGTERM 
         * and a SIGHUP, we may as well use the same signal, because some user
         * pthreads are stealing signals from us left and right.
         */
	    
        ap_reclaim_child_processes(1);		/* Start with SIGTERM */
	    ap_log_error(APLOG_MARK, APLOG_NOERRNO|APLOG_NOTICE, 0, ap_server_conf,
		    "SIGHUP received.  Attempting to restart");
    }
    
    /* just before we go, tidy up the locks we've created to prevent a 
     * potential leak of semaphores... */
    apr_lock_destroy(worker_thread_count_mutex);
    apr_lock_destroy(accept_mutex);
    
    return 0;
}

static void beos_pre_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp)
{
    static int restart_num = 0;
    int no_detach = 0;

    one_process = !!ap_exists_config_define("ONE_PROCESS");
    no_detach = !!ap_exists_config_define("NO_DETACH");

    /* sigh, want this only the second time around */
    if (restart_num++ == 1) {
        is_graceful = 0;
        
        if (!one_process && !no_detach)
	        apr_proc_detach();

        server_pid = getpid();
    }

    beosd_pre_config();
    ap_listen_pre_config();
    ap_threads_to_start = DEFAULT_START_THREADS;
    min_spare_threads = DEFAULT_MIN_FREE_THREADS;
    max_spare_threads = DEFAULT_MAX_FREE_THREADS;
    ap_thread_limit = HARD_THREAD_LIMIT;
    ap_threads_per_child = DEFAULT_THREADS_PER_CHILD;
    ap_pid_fname = DEFAULT_PIDLOG;
    ap_scoreboard_fname = DEFAULT_SCOREBOARD;
    ap_max_requests_per_child = DEFAULT_MAX_REQUESTS_PER_CHILD;

    apr_cpystrn(ap_coredump_dir, ap_server_root, sizeof(ap_coredump_dir));
}

static void beos_hooks(apr_pool_t *p)
{
    one_process = 0;
    
    ap_hook_pre_config(beos_pre_config, NULL, NULL, APR_HOOK_MIDDLE); 
}


static const char *set_pidfile(cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    if (cmd->server->is_virtual) {
	return "PidFile directive not allowed in <VirtualHost>";
    }
    ap_pid_fname = arg;
    return NULL;
}

static const char *set_scoreboard(cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    ap_scoreboard_fname = arg;
    return NULL;
}

static const char *set_daemons_to_start(cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    ap_threads_to_start = atoi(arg);
    return NULL;
}

static const char *set_min_spare_threads(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    min_spare_threads = atoi(arg);
    if (min_spare_threads <= 0) {
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "WARNING: detected MinSpareThreads set to non-positive.");
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL,
                    "Resetting to 1 to avoid almost certain Apache failure.");
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "Please read the documentation.");
       min_spare_threads = 1;
    }
       
    return NULL;
}

static const char *set_max_spare_threads(cmd_parms *cmd, void *dummy, const char *arg)
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    max_spare_threads = atoi(arg);
    return NULL;
}

static const char *set_server_limit (cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    ap_thread_limit = atoi(arg);
    if (ap_thread_limit > HARD_SERVER_LIMIT) {
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    "WARNING: MaxClients of %d exceeds compile time limit "
                    "of %d servers,", ap_thread_limit, HARD_SERVER_LIMIT);
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    " lowering MaxClients to %d.  To increase, please "
                    "see the", HARD_SERVER_LIMIT);
       ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                    " HARD_SERVER_LIMIT define in src/include/httpd.h.");
       ap_thread_limit = HARD_SERVER_LIMIT;
    } 
    else if (ap_thread_limit < 1) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                     "WARNING: Require MaxClients > 0, setting to 1");
        ap_thread_limit = 1;
    }
    return NULL;
}

static const char *set_threads_per_child (cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    ap_threads_per_child = atoi(arg);
    if (ap_threads_per_child > HARD_THREAD_LIMIT) {
        ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                     "WARNING: ThreadsPerChild of %d exceeds compile time"
                     "limit of %d threads,", ap_threads_per_child,
                     HARD_THREAD_LIMIT);
        ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                     " lowering ThreadsPerChild to %d. To increase, please"
                     "see the", HARD_THREAD_LIMIT);
        ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                     " HARD_THREAD_LIMIT define in %s", AP_MPM_HARD_LIMITS_FILE);
    }
    else if (ap_threads_per_child < 1) {
	ap_log_error(APLOG_MARK, APLOG_STARTUP | APLOG_NOERRNO, 0, NULL, 
                     "WARNING: Require ThreadsPerChild > 0, setting to 1");
	ap_threads_per_child = 1;
    }
    return NULL;
}

static const char *set_max_requests(cmd_parms *cmd, void *dummy, const char *arg) 
{
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    ap_max_requests_per_child = atoi(arg);

    return NULL;
}

static const char *set_coredumpdir (cmd_parms *cmd, void *dummy, const char *arg) 
{
    apr_finfo_t finfo;
    const char *fname;
    const char *err = ap_check_cmd_context(cmd, GLOBAL_ONLY);
    if (err != NULL) {
        return err;
    }

    fname = ap_server_root_relative(cmd->pool, arg);
    if ((apr_stat(&finfo, fname, APR_FINFO_TYPE, cmd->pool) != APR_SUCCESS) 
        || (finfo.filetype != APR_DIR)) {
	return apr_pstrcat(cmd->pool, "CoreDumpDirectory ", fname, 
			  " does not exist or is not a directory", NULL);
    }
    apr_cpystrn(ap_coredump_dir, fname, sizeof(ap_coredump_dir));
    return NULL;
}

static const command_rec beos_cmds[] = {
LISTEN_COMMANDS
AP_INIT_TAKE1( "PidFile", set_pidfile, NULL, RSRC_CONF,
    "A file for logging the server process ID"),
AP_INIT_TAKE1( "ScoreBoardFile", set_scoreboard, NULL, RSRC_CONF,
    "A file for Apache to maintain runtime process management information"),
AP_INIT_TAKE1( "StartServers", set_daemons_to_start, NULL, RSRC_CONF,
  "Number of child processes launched at server startup"),
AP_INIT_TAKE1( "MinSpareThreads", set_min_spare_threads, NULL, RSRC_CONF,
  "Minimum number of idle children, to handle request spikes"),
AP_INIT_TAKE1( "MaxSpareThreads", set_max_spare_threads, NULL, RSRC_CONF,
  "Maximum number of idle children" ),
AP_INIT_TAKE1( "MaxClients", set_server_limit, NULL, RSRC_CONF, 
  "Maximum number of children alive at the same time" ),
AP_INIT_TAKE1( "ThreadsPerChild", set_threads_per_child, NULL, RSRC_CONF, 
  "Number of threads each child creates" ),
AP_INIT_TAKE1( "MaxRequestsPerChild", set_max_requests, NULL, RSRC_CONF,
  "Maximum number of requests a particular child serves before dying." ),
AP_INIT_TAKE1( "CoreDumpDirectory", set_coredumpdir, NULL, RSRC_CONF, 
  "The location of the directory Apache changes to before dumping core" ),
{ NULL }
};

module AP_MODULE_DECLARE_DATA mpm_beos_module = {
    MPM20_MODULE_STUFF,
    NULL,                       /* hook to run before apache parses args */
    NULL,			/* create per-directory config structure */
    NULL,			/* merge per-directory config structures */
    NULL,			/* create per-server config structure */
    NULL,			/* merge per-server config structures */
    beos_cmds,		/* command apr_table_t */
    beos_hooks		/* register_hooks */
};

