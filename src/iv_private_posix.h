/*
 * ivykis, an event handling library
 * Copyright (C) 2002, 2003, 2009, 2012 Lennert Buytenhek
 * Dedicated to Marija Kulikova.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License version 2.1 for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License version 2.1 along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <poll.h>
#include <pthread.h>

#define MASKIN		1
#define MASKOUT		2
#define MASKERR		4

#define MAX_POLL_FDS		7

struct iv_mt_group {
	struct iv_list_head	list_aux;
	struct iv_state		*st;
	int			upd_pipe[2];
	int			update_occured;
	int			num_fds;
	struct iv_fd_		*fds[MAX_POLL_FDS];
};

struct iv_state {
	/* iv_main_posix.c  */
	int			quit;
	int			numobjs;
	unsigned long		tid;

	/* iv_fd.c  */
	int			numfds;
	struct iv_fd_		*handled_fd;

	/* iv_task.c  */
	struct iv_list_head	tasks;

	/* iv_timer.c  */
	struct timespec		time;
	int			time_valid;
	int			num_timers;
	int			rat_depth;
	struct ratnode		*timer_root;

	/* poll methods  */
	union {
#ifdef HAVE_SYS_DEVPOLL_H
		struct {
			struct iv_avl_tree	fds;
			int			poll_fd;
			struct iv_list_head	notify;
		} dev_poll;
#endif

#ifdef HAVE_EPOLL_CREATE
		struct {
			int			epoll_fd;
			struct iv_list_head	notify;
		} epoll;
#endif

#ifdef HAVE_KQUEUE
		struct {
			int			kqueue_fd;
			struct iv_list_head	notify;
		} kqueue;
#endif

		struct {
			struct pollfd		*pfds;
			struct iv_fd_		**fds;
			int			num_regd_fds;
		} poll;

#ifdef ENABLE_POLL_MT
		struct {
			pthread_mutex_t		state_lock;
			pthread_cond_t		groups_aux_empty;
			struct iv_list_head	groups_aux;
			struct iv_mt_group	*notokay;
			pthread_cond_t		exec_lock_free;
			int			exec_lock;
			int			event_rx_active;
			struct iv_mt_group	group_main;
			struct pollfd		pfds[MAX_POLL_FDS + 1];
		} poll_mt;
#endif

#ifdef HAVE_PORT_CREATE
		struct {
			int			port_fd;
			struct iv_list_head	notify;
		} port;
#endif
	} u;
};

struct iv_fd_ {
	/*
	 * User data.
	 */
	int			fd;
	void			*cookie;
	void			(*handler_in)(void *);
	void			(*handler_out)(void *);
	void			(*handler_err)(void *);

	/*
	 * If this fd gathered any events during this polling round,
	 * fd->list_active will be on iv_main()'s active list, and
	 * fd->ready_bands will indicate which bands are currently
	 * active.
	 */
	struct iv_list_head	list_active;
	uint8_t			ready_bands;

	/*
	 * Reflects whether the fd has been registered with
	 * iv_fd_register().  Will be zero in ->notify_fd() if the
	 * fd is being unregistered.
	 */
	uint8_t			registered;

	/*
	 * ->wanted_bands is set by the ivykis core to indicate
	 * which bands currenty have handlers registered for them.
	 */
	uint8_t			wanted_bands;

	/*
	 * ->registered_bands is maintained by the poll method to
	 * indicate which bands are currently registered with the
	 * kernel, so that the ivykis core knows when to call
	 * the poll method's ->notify_fd() on an fd.
	 */
	uint8_t			registered_bands;

#if defined(HAVE_SYS_DEVPOLL_H) || defined(HAVE_EPOLL_CREATE) ||	\
    defined(HAVE_KQUEUE) || defined(HAVE_PORT_CREATE)
	/*
	 * ->list_notify is used by poll methods that defer updating
	 * kernel registrations to ->poll() time.
	 */
	struct iv_list_head	list_notify;
#endif

	/*
	 * This is for state internal to some of the poll methods.
	 */
	union {
#ifdef HAVE_SYS_DEVPOLL_H
		struct iv_avl_node	avl_node;
#endif
		int			index;
#ifdef ENABLE_POLL_MT
		struct {
			struct iv_mt_group	*grp;
			int			index;
		} poll_mt;
#endif
	} u;
};

struct iv_fd_poll_method {
	char	*name;
	int	(*init)(struct iv_state *st);
	void	(*poll)(struct iv_state *st,
			struct iv_list_head *active, struct timespec *abs);
	void	(*register_fd)(struct iv_state *st, struct iv_fd_ *fd);
	void	(*unregister_fd)(struct iv_state *st, struct iv_fd_ *fd);
	void	(*notify_fd)(struct iv_state *st, struct iv_fd_ *fd);
	int	(*notify_fd_sync)(struct iv_state *st, struct iv_fd_ *fd);
	void	(*deinit)(struct iv_state *st);
	int	(*event_rx_on)(struct iv_state *st);
	void	(*event_rx_off)(struct iv_state *st);
	void	(*event_send)(struct iv_state *dest);
};

extern int maxfd;
extern struct iv_fd_poll_method *method;

extern struct iv_fd_poll_method iv_fd_poll_method_dev_poll;
extern struct iv_fd_poll_method iv_fd_poll_method_epoll;
extern struct iv_fd_poll_method iv_fd_poll_method_kqueue;
extern struct iv_fd_poll_method iv_fd_poll_method_poll;
extern struct iv_fd_poll_method iv_fd_poll_method_poll_mt;
extern struct iv_fd_poll_method iv_fd_poll_method_port;
extern struct iv_fd_poll_method iv_fd_poll_method_ppoll;

/* iv_event_posix.c */
void iv_event_run_pending_events(void);

/* iv_fd.c */
void iv_fd_init(struct iv_state *st, unsigned int flags);
void iv_fd_deinit(struct iv_state *st);
void iv_fd_run_active_list(struct iv_state *st, struct iv_list_head *active);
void iv_fd_poll_and_run(struct iv_state *st, struct timespec *abs);
void iv_fd_make_ready(struct iv_list_head *active,
		      struct iv_fd_ *fd, int bands);
void iv_fd_set_cloexec(int fd);
void iv_fd_set_nonblock(int fd);


extern pthread_key_t iv_state_key;

#ifdef HAVE_THREAD
extern __thread struct iv_state *__st;

static inline struct iv_state *iv_get_state(void)
{
	return __st;
}
#else
static inline struct iv_state *iv_get_state(void)
{
	return pthread_getspecific(iv_state_key);
}
#endif
