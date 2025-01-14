/*
 * cpacfstats - display and maintain CPACF perf counters
 *
 * cpacfstatsd daemon implementation
 *
 * Copyright IBM Corp. 2015, 2022
 *
 * s390-tools is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "lib/zt_common.h"
#include "cpacfstats.h"

static volatile int stopsig;

static const char *const name = "cpacfstatsd";

static const char *const usage =
	"Usage: %s [OPTIONS]\n"
	"\n"
	"Daemon to provide access to CPACF perf counters\n"
	"Use OPTIONS described below:\n"
	"\n"
	"\t-h, --help          Print this help, then exit\n"
	"\t-v, --version       Print version information, then exit\n"
	"\t-f, --foreground    Run in foreground, do not detach\n";

static int daemonized;

static int recv_query(int s, enum ctr_e *ctr, enum cmd_e *cmd)
{
	struct msg m;
	int rc;

	rc = recv_msg(s, &m, DEFAULT_RECV_TIMEOUT);
	if (rc == 0) {
		if (m.head.m_ver != VERSION) {
			eprint("Received msg with wrong version %d != %d\n",
			       m.head.m_ver, VERSION);
			return -1;
		}
		if (m.head.m_type != QUERY) {
			eprint("Received msg with wrong type %d != %d\n",
			       m.head.m_type, QUERY);
			return -1;
		}
		*ctr = m.query.m_ctr;
		*cmd = m.query.m_cmd;
	}

	return rc;
}


static int send_answer(int s, int ctr, int state, uint64_t value)
{
	struct msg m;

	memset(&m, 0, sizeof(m));

	m.head.m_ver = VERSION;
	m.head.m_type = ANSWER;
	m.answer.m_ctr = ctr;
	m.answer.m_state = state;
	m.answer.m_value = value;

	return send_msg(s, &m, DEFAULT_SEND_TIMEOUT);
}


/*
 * Print according to protocol for PAI:
 * - first the state and the number of PAI counters that follow
 * - if state is ENABLED:
 *   - for each PAI counter the value with state ENABLED
 * Note that the PAI counters are 0-based, not 1 based as in PoP!
 * Sending ends with the first error.
 */
static int do_send_pai(int s, int user)
{
	int ctr, state, i, maxctr, rc = 0;
	uint64_t value;
	
	if (user) {
		ctr = PAI_USER;
		maxctr = NUM_PAI_USER;
	} else {
		ctr = PAI_KERNEL;
		maxctr = NUM_PAI_KERNEL;
	}
	state = perf_ctr_state(ctr);
	if (state != ENABLED)
		return rc;
	for (i = 0; i < maxctr; ++i) {
		rc = perf_read_pai_ctr(i, user, &value);
		if (rc != 0) {
			send_answer(s, i, rc, 0);
			break;
		}
		send_answer(s, i, state, value);
	}
	return rc;
}


static int do_enable(int s, enum ctr_e ctr)
{
	uint64_t value = 0;
	int i, rc = 0;
	int state;

	for (i = 0; i < NUM_COUNTER; i++) {
		if (i == ALL_COUNTER)
			continue;
		if (i == (int) ctr || ctr == ALL_COUNTER) {
			state = perf_ctr_state(i);
			if (state == DISABLED) {
				rc = perf_enable_ctr(i);
				if (rc != 0) {
					send_answer(s, i, rc, 0);
					break;
				}
				state = ENABLED;
			}
			if (state != UNSUPPORTED) {
				rc = perf_read_ctr(i, &value);
				if (rc != 0) {
					send_answer(s, i, rc, 0);
					break;
				}
			}
			send_answer(s, i, state, value);
			if (i == PAI_USER)
				rc = do_send_pai(s, 1);
			if (i == PAI_KERNEL)
				rc = do_send_pai(s, 0);
		}
	}
	if (rc == 0) {
		rc = perf_read_ctr(HOTPLUG_DETECTED, &value);
		send_answer(s, HOTPLUG_DETECTED, rc, value);
	}
	return rc;
}


static int do_disable(int s, enum ctr_e ctr)
{
	int i, rc = 0;
	uint64_t value;

	for (i = 0; i < NUM_COUNTER; i++) {
		if (i == ALL_COUNTER)
			continue;
		if (i == (int) ctr || ctr == ALL_COUNTER) {
			if (perf_ctr_state(i) == ENABLED) {
				rc = perf_disable_ctr(i);
				if (rc != 0) {
					send_answer(s, i, rc, 0);
					break;
				}
			}
			send_answer(s, i, perf_ctr_state(i), 0);
		}
	}
	if (rc == 0) {
		rc = perf_read_ctr(HOTPLUG_DETECTED, &value);
		send_answer(s, HOTPLUG_DETECTED, rc, value);
	}
	return rc;
}


static int do_reset(int s, enum ctr_e ctr)
{
	int i, rc = 0, state;
	uint64_t value;

	for (i = 0; i < NUM_COUNTER; i++) {
		if (i == ALL_COUNTER)
			continue;
		if (i == (int) ctr || ctr == ALL_COUNTER) {
			state = perf_ctr_state(i);
			if (state == ENABLED) {
				rc = perf_reset_ctr(i, &value);
				if (rc != 0) {
					send_answer(s, i, rc, 0);
					break;
				}
			}
			send_answer(s, i, state, value);
			if (i == PAI_USER)
				rc = do_send_pai(s, 1);
			if (i == PAI_KERNEL)
				rc = do_send_pai(s, 0);
		}
	}
	if (rc == 0) {
		rc = perf_read_ctr(HOTPLUG_DETECTED, &value);
		send_answer(s, HOTPLUG_DETECTED, rc, value);
	}
	return rc;
}


static int do_print(int s, enum ctr_e ctr)
{
	int i, rc = 0, state;
	uint64_t value = 0;

	for (i = 0; i < NUM_COUNTER; i++) {
		if (i == ALL_COUNTER)
			continue;
		if (i == (int) ctr || ctr == ALL_COUNTER) {
			state = perf_ctr_state(i);
			if (state == ENABLED) {
				rc = perf_read_ctr(i, &value);
				if (rc != 0) {
					send_answer(s, i, rc, 0);
					break;
				}
			}
			send_answer(s, i, state, value);
			if (i == PAI_USER)
				rc = do_send_pai(s, 1);
			if (i == PAI_KERNEL)
				rc = do_send_pai(s, 0);
		}
	}
	if (rc == 0) {
		rc = perf_read_ctr(HOTPLUG_DETECTED, &value);
		send_answer(s, HOTPLUG_DETECTED, rc, value);
	}
	return rc;
}


static int become_daemon(void)
{
	FILE *f;
	int fd;

	/* syslog */
	openlog("cpacfstatsd", 0, LOG_DAEMON);

	/*
	 * fork and terminate parent
	 * Reasons:
	 * - opens new command line prompt
	 * - the child process is guaranteed not to be the process group leader
	 *   nessecarry for setsid.
	 */

	switch (fork()) {
	case -1:  /* error */
		eprint("Fork() failed, errno=%d [%s]\n",
		       errno, strerror(errno));
		return -1;
	case 0:   /* child */
		break;
	default:  /* parent */
		_exit(0);
	}

	if (chdir("/") != 0) {
		eprint("Chdir('/') failed, errno=%d [%s]\n",
		       errno, strerror(errno));
		return -1;
	}

	/* start new session */
	if (setsid() == -1) {
		eprint("Setsid() failed, errno=%d [%s]\n",
		       errno, strerror(errno));
		return -1;
	}

	/* clear umask so that socket has right default permission */
	umask(0007);

	/* make stdin, stdout and stderr use /dev/null */
	fd = open("/dev/null", O_RDWR);
	if (fd < 0) {
		eprint("Could not open /dev/null, errno=%d [%s]\n",
		       errno, strerror(errno));
		return -1;
	}
	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	close(fd);

	daemonized = 1;

	/* make pid file, fails if the file exists */
	f = fopen(PID_FILE, "w+x");
	if (!f) {
		eprint("Couldn't create pid file '%s', errno=%d [%s]\n",
		       PID_FILE, errno, strerror(errno));
		return -1;
	}
	fprintf(f, "%lu", (unsigned long)getpid());
	fflush(f);
	fclose(f);
	chmod(PID_FILE, 0644);

	return 0;
}


static void remove_sock(void)
{
	remove(SOCKET_FILE);
}

static int check_pidfile(void)
{
	unsigned long pid;
	FILE *f;

	f = fopen(PID_FILE, "r");
	if (!f) {
		if (errno == ENOENT) {
			/* pid file does not exit, pid file check is ok */
			return 0;
		}
		/* unknown errno, pid file check is not ok */
		eprint("Unknown error on pid file check '%s', errno=%d [%s]\n",
		       PID_FILE, errno, strerror(errno));
		return -1;
	}

	/* pid file could be opened, scan pid in there */
	if (fscanf(f, "%lu", &pid) != 1) {
		/*
		 * invalid, maybe a leftover from a previous run
		 * remove and return pid file check ok
		 */
		fclose(f);
		remove(PID_FILE);
		return 0;
	}
	fclose(f);

	/* check if this process is still running */
	if (kill(pid, 0) != 0) {
		/*
		 * failure, assume this means there is no such pid running
		 * remove pid file and return pid file check ok
		 */
		remove(PID_FILE);
		return 0;
	}

	/*
	 * looks like there is another cpacfstatsd running
	 * return with pid file check failure
	 */
	eprint("Looks like there is another cpacfstatsd (pid=%lu) running\n",
	       pid);
	eprint("Please check and maybe remove stale pid file '%s'\n",
	       PID_FILE);

	return -1;
}

static void remove_pidfile(void)
{
	remove(PID_FILE);
}


void signalhandler(int sig)
{
	perf_stop();
	stopsig = sig;
}


int eprint(const char *format, ...)
{
	char buf[512];
	va_list vargs;
	int i, n;

	i = snprintf(buf, sizeof(buf), "%s: ", name);
	va_start(vargs, format);
	n = vsnprintf(buf+i, sizeof(buf)-i, format, vargs);
	va_end(vargs);

	if (n > 0) {
		if (daemonized)
			syslog(LOG_WARNING, "%s", buf);
		else
			fputs(buf, stderr);
	}

	return n;
}


int main(int argc, char *argv[])
{
	int rc, sfd, foreground = 0;
	struct sigaction act;

	if (argc > 1) {
		int opt, idx = 0;
		const struct option long_opts[] = {
			{ "help", 0, NULL, 'h' },
			{ "foreground", 0, NULL, 'f' },
			{ "version", 0, NULL, 'v' },
			{ NULL, 0, NULL, 0 } };
		while (1) {
			opt = getopt_long(argc, argv,
					  "hfv", long_opts, &idx);
			if (opt == -1)
				break; /* no more arguments */
			switch (opt) {
			case 'h':
				printf(usage, name);
				return 0;
			case 'f':
				foreground = 1;
				break;
			case 'v':
				printf("%s: Linux on System z CPACF Crypto Activity Counters Daemon\n"
				       "Version %s\n%s\n",
				       name, RELEASE_STRING, COPYRIGHT);
				return 0;
			default:
				printf("%s: Invalid argument, try -h or --help for more information\n",
					name);
				return EXIT_FAILURE;
			}
		}
	}

	if (check_pidfile() != 0) {
		eprint("Stalled pid file or daemon allready running, terminating\n");
		return EXIT_FAILURE;
	}

	if (!foreground) {
		if (become_daemon() != 0) {
			eprint("Couldn't daemonize\n");
			return EXIT_FAILURE;
		}
	}

	if (perf_init() != 0) {
		eprint("Couldn't initialize perf lib\n");
		return EXIT_FAILURE;
	}
	atexit(perf_close);

	sfd = open_socket(SERVER);
	if (sfd < 0) {
		eprint("Couldn't initialize server socket\n");
		return EXIT_FAILURE;
	}
	atexit(remove_sock);

	memset(&act, 0, sizeof(act));
	act.sa_handler = signalhandler;
	act.sa_flags = 0;
	if (sigaction(SIGINT, &act, 0) != 0) {
		eprint("Couldn't establish signal handler for SIGINT, errno=%d [%s]\n",
		       errno, strerror(errno));
		return EXIT_FAILURE;
	}
	if (sigaction(SIGTERM, &act, 0) != 0) {
		eprint("Couldn't establish signal handler for SIGTERM, errno=%d [%s]\n",
		       errno, strerror(errno));
		return EXIT_FAILURE;
	}
	/* Ignore SIGPIPE such that we see EPIPE as return from write. */
	signal(SIGPIPE, SIG_IGN);

	eprint("Running\n");

	while (!stopsig) {
		enum ctr_e ctr;
		enum cmd_e cmd;
		int s;

		s = accept(sfd, NULL, NULL);
		if (s < 0) {
			if (errno == EINTR)
				continue;
			eprint("Accept() failure, errno=%d [%s]\n",
			       errno, strerror(errno));
			return EXIT_FAILURE;
		}

		rc = recv_query(s, &ctr, &cmd);
		if (rc != 0) {
			eprint("Recv_query() failed, ignoring\n");
			goto cleanup;
		}

		if (cmd == ENABLE)
			rc = do_enable(s, ctr);
		else if (cmd == DISABLE)
			rc = do_disable(s, ctr);
		else if (cmd == RESET)
			rc = do_reset(s, ctr);
		else if (cmd == PRINT)
			rc = do_print(s, ctr);
		else {
			eprint("Received unknown command %d, ignoring\n",
			       (int) cmd);
			goto cleanup;
		}

cleanup:
		close(s);
	}

	if (stopsig == SIGTERM)
		eprint("Caught signal SIGTERM, terminating...\n");
	else if (stopsig == SIGINT)
		eprint("Caught signal SIGINT, terminating...\n");
	else
		eprint("Caught signal %d, terminating...\n", stopsig);
	remove_pidfile();

	return 0;
}
