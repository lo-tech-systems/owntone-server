/*
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
 *
 * Pieces from mt-daapd:
 * Copyright (C) 2003 Ron Pedde (ron@pedde.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

/* owntone_config.h provides platform detection fallbacks (HAVE_SIGNALFD etc.)
 * and must come before any conditional system includes below. */
#include "owntone_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <grp.h>
#include <stdint.h>

#ifdef HAVE_SIGNALFD
# include <sys/signalfd.h>
#elif defined(HAVE_KQUEUE)
# include <sys/time.h>
# include <sys/event.h>
#endif

#include <getopt.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <curl/curl.h>

#include <pthread.h>
#include <gcrypt.h>

#include "db.h"
#include "logger.h"
#include "misc.h"
#include "cache.h"
#include "httpd.h"
#include "mdns.h"
#include "player.h"
#include "worker.h"
#include "library.h"
#include "ptpd.h"

#define PIDFILE          STATEDIR "/run/owntone.pid"
#define SQLITE_EXT_PATH  PKGLIBDIR "/owntone-sqlext.so"

struct event_base *evbase_main;

static struct event *sig_event;
static int main_exit;

static void
version(void)
{
  fprintf(stdout, "%s %s\n", PACKAGE_NAME, PACKAGE_VERSION);
}

static void
usage(char *program)
{
  version();
  printf("\n");
  printf("Usage: %s [options]\n\n", program);
  printf("Options:\n");
  printf("  -d <number>     Log level (0-5)\n");
  printf("  -D <dom,dom..>  Log domains\n");
  printf("  -c <file>       Use <file> as the settings file (default: " SETTINGS_FILE ")\n");
  printf("  -P <file>       Write PID to specified file\n");
  printf("  -f              Run in foreground\n");
  printf("  -v              Display version information\n");
  printf("\n\n");
  printf("Available log domains:\n");
  logger_domains();
  printf("\n\n");
}

static int
daemonize(bool background, char *pidfile)
{
  FILE *fp;
  pid_t childpid;
  pid_t pid_ret;
  int fd;
  int ret;
  char *runas;

  if (background)
    {
      fp = fopen(pidfile, "w");
      if (!fp)
	{
	  DPRINTF(E_LOG, L_MAIN, "Error opening pidfile (%s): %s\n", pidfile, strerror(errno));

	  return -1;
	}

      fd = open("/dev/null", O_RDWR, 0);
      if (fd < 0)
	{
	  DPRINTF(E_LOG, L_MAIN, "Error opening /dev/null: %s\n", strerror(errno));

	  fclose(fp);
	  return -1;
	}

      signal(SIGTTOU, SIG_IGN);
      signal(SIGTTIN, SIG_IGN);
      signal(SIGTSTP, SIG_IGN);

      childpid = fork();

      if (childpid > 0)
	exit(EXIT_SUCCESS);
      else if (childpid < 0)
	{
	  DPRINTF(E_FATAL, L_MAIN, "Fork failed: %s\n", strerror(errno));

	  close(fd);
	  fclose(fp);
	  return -1;
	}

      pid_ret = setsid();
      if (pid_ret == (pid_t) -1)
	{
	  DPRINTF(E_FATAL, L_MAIN, "setsid() failed: %s\n", strerror(errno));

	  close(fd);
	  fclose(fp);
	  return -1;
	}

      logger_detach();

      dup2(fd, STDIN_FILENO);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);

      if (fd > 2)
	close(fd);

      ret = chdir("/");
      if (ret < 0)
        DPRINTF(E_WARN, L_MAIN, "chdir() failed: %s\n", strerror(errno));

      umask(0);

      fprintf(fp, "%d\n", getpid());
      fclose(fp);

      DPRINTF(E_DBG, L_MAIN, "PID: %d\n", getpid());
    }

  if (geteuid() == (uid_t) 0)
    {
      runas = (char *)config_get_str("uid", "nobody");

      ret = initgroups(runas, runas_gid);
      if (ret != 0)
	{
	  DPRINTF(E_FATAL, L_MAIN, "initgroups() failed: %s\n", strerror(errno));

	  return -1;
	}

      ret = setegid(runas_gid);
      if (ret != 0)
	{
	  DPRINTF(E_FATAL, L_MAIN, "setegid() failed: %s\n", strerror(errno));

	  return -1;
	}

      ret = seteuid(runas_uid);
      if (ret != 0)
	{
	  DPRINTF(E_FATAL, L_MAIN, "seteuid() failed: %s\n", strerror(errno));

	  return -1;
	}
    }

  return 0;
}


#ifdef HAVE_SIGNALFD
static void
signal_signalfd_cb(int fd, short event, void *arg)
{
  struct signalfd_siginfo info;
  int status;

  while (read(fd, &info, sizeof(struct signalfd_siginfo)) == sizeof(struct signalfd_siginfo))
    {
      switch (info.ssi_signo)
	{
	  case SIGCHLD:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGCHLD\n");

	    while (waitpid(-1, &status, WNOHANG) > 0)
	      /* Nothing. */ ;
	    break;

	  case SIGINT:
	  case SIGTERM:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGTERM or SIGINT\n");

	    main_exit = 1;
	    break;

	  case SIGHUP:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGHUP\n");

	    if (!main_exit)
	      logger_reinit();
	    break;
	}
    }

  if (main_exit)
    event_base_loopbreak(evbase_main);
  else
    event_add(sig_event, NULL);
}

#elif defined(HAVE_KQUEUE)

static void
signal_kqueue_cb(int fd, short event, void *arg)
{
  struct timespec ts;
  struct kevent ke;
  int status;

  ts.tv_sec = 0;
  ts.tv_nsec = 0;

  while (kevent(fd, NULL, 0, &ke, 1, &ts) > 0)
    {
      switch (ke.ident)
	{
	  case SIGCHLD:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGCHLD\n");

	    while (waitpid(-1, &status, WNOHANG) > 0)
	      /* Nothing. */ ;
	    break;

	  case SIGINT:
	  case SIGTERM:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGTERM or SIGINT\n");

	    main_exit = 1;
	    break;

	  case SIGHUP:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGHUP\n");

	    if (!main_exit)
	      logger_reinit();
	    break;
	}
    }

  if (main_exit)
    event_base_loopbreak(evbase_main);
  else
    event_add(sig_event, NULL);
}
#endif

#if (LIBAVCODEC_VERSION_MAJOR < 58) || ((LIBAVCODEC_VERSION_MAJOR == 58) && (LIBAVCODEC_VERSION_MINOR < 18))
static int
ffmpeg_lockmgr(void **pmutex, enum AVLockOp op)
{
  switch (op)
    {
      case AV_LOCK_CREATE:
	*pmutex = malloc(sizeof(pthread_mutex_t));
	if (!*pmutex)
	  return 1;
        CHECK_ERR(L_MAIN, mutex_init(*pmutex));
	return 0;

      case AV_LOCK_OBTAIN:
        CHECK_ERR(L_MAIN, pthread_mutex_lock(*pmutex));
	return 0;

      case AV_LOCK_RELEASE:
        CHECK_ERR(L_MAIN, pthread_mutex_unlock(*pmutex));
	return 0;

      case AV_LOCK_DESTROY:
	CHECK_ERR(L_MAIN, pthread_mutex_destroy(*pmutex));
	free(*pmutex);
        *pmutex = NULL;

	return 0;
    }

  return 1;
}
#endif

int
main(int argc, char **argv)
{
  int option;
  const char *settings_file = SETTINGS_FILE;
  bool background = true;
  bool testrun = false;
  int loglevel = -1;
  char *logdomains = NULL;
  const char *logfile = NULL;
  const char *logformat = NULL;
  char *pidfile = PIDFILE;
  char **buildopts;
  const char *av_version;
  const char *gcry_version;
  sigset_t sigs;
  int sigfd;
#ifdef HAVE_KQUEUE
  struct kevent ke_sigs[4];
#endif
  int i;
  int ret;

  struct option option_map[] = {
    { "debug",         1, NULL, 'd' },
    { "logdomains",    1, NULL, 'D' },
    { "foreground",    0, NULL, 'f' },
    { "config",        1, NULL, 'c' },
    { "pidfile",       1, NULL, 'P' },
    { "version",       0, NULL, 'v' },
    { "testrun",       0, NULL, 't' }, // Used for CI, not documented to user
    { "logformat",     1, NULL, 516 },

    { NULL,            0, NULL, 0   }
  };

  while ((option = getopt_long(argc, argv, "D:d:c:P:ftv", option_map, NULL)) != -1)
    {
      switch (option)
	{
	  case 516:
	    logformat = optarg;
	    break;

	  case 't':
	    testrun = true;
	    break;

	  case 'd':
	    ret = safe_atoi32(optarg, &option);
	    if (ret < 0)
	      fprintf(stderr, "Error: loglevel must be an integer in '-d %s'\n", optarg);
	    else
	      loglevel = option;
	    break;

	  case 'D':
	    logdomains = optarg;
	    break;

	  case 'f':
	    background = false;
	    break;

	  case 'c':
	    settings_file = optarg;
	    break;

	  case 'P':
	    pidfile = optarg;
	    break;

	  case 'v':
	    version();
	    return EXIT_SUCCESS;
	    break;

	  default:
	    usage(argv[0]);
	    return EXIT_FAILURE;
	    break;
	}
    }

  ret = logger_init(NULL, NULL, (loglevel < 0) ? E_LOG : loglevel, NULL);
  if (ret != 0)
    {
      fprintf(stderr, "Could not initialize log facility\n");

      return EXIT_FAILURE;
    }

  ret = config_load(settings_file);
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Settings file errors; please fix %s\n", settings_file);

      logger_deinit();
      return EXIT_FAILURE;
    }

  logger_deinit();

  /* Reinit log facility with settings values */
  if (loglevel < 0)
    loglevel = config_get_int("loglevel", E_LOG);
  if (!logformat)
    logformat = config_get_str("logformat", "default");
  logfile = config_get_str("logfile", NULL);

  ret = logger_init((char *)logfile, logdomains, loglevel, (char *)logformat);
  if (ret != 0)
    {
      fprintf(stderr, "Could not reinitialize log facility with settings file values\n");

      config_unload();
      return EXIT_FAILURE;
    }

  /* Set up libevent logging callback */
  event_set_log_callback(logger_libevent);

  DPRINTF(E_LOG, L_MAIN, "OwnTone version %s taking off\n", VERSION);

  DPRINTF(E_LOG, L_MAIN, "Built with:\n");
  buildopts = buildopts_get();
  for (i = 0; buildopts[i]; i++)
    {
      DPRINTF(E_LOG, L_MAIN, "- %s\n", buildopts[i]);
    }

#if HAVE_DECL_AV_VERSION_INFO
  av_version = av_version_info();
#else
  av_version = "(unknown version)";
#endif

#ifdef HAVE_FFMPEG
  DPRINTF(E_INFO, L_MAIN, "Initialized with ffmpeg %s\n", av_version);
#else
  DPRINTF(E_INFO, L_MAIN, "Initialized with libav %s\n", av_version);
#endif

// The following was deprecated with ffmpeg 4.0 = avcodec 58.18, avformat 58.12, avfilter 7.16
#if (LIBAVCODEC_VERSION_MAJOR < 58) || ((LIBAVCODEC_VERSION_MAJOR == 58) && (LIBAVCODEC_VERSION_MINOR < 18))
  ret = av_lockmgr_register(ffmpeg_lockmgr);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not register ffmpeg lock manager callback\n");

      ret = EXIT_FAILURE;
      goto ffmpeg_init_fail;
    }
#endif
#if (LIBAVFORMAT_VERSION_MAJOR < 58) || ((LIBAVFORMAT_VERSION_MAJOR == 58) && (LIBAVFORMAT_VERSION_MINOR < 12))
  av_register_all();
#endif
#if (LIBAVFILTER_VERSION_MAJOR < 7) || ((LIBAVFILTER_VERSION_MAJOR == 7) && (LIBAVFILTER_VERSION_MINOR < 16))
  avfilter_register_all();
#endif

#if HAVE_DECL_AVFORMAT_NETWORK_INIT
  avformat_network_init();
#endif
  av_log_set_callback(logger_ffmpeg);

  /* Initialize libcurl */
  curl_global_init(CURL_GLOBAL_DEFAULT);

  gcry_version = gcry_check_version(GCRYPT_VERSION);
  if (!gcry_version)
    {
      DPRINTF(E_FATAL, L_MAIN, "libgcrypt version mismatch\n");

      ret = EXIT_FAILURE;
      goto gcrypt_init_fail;
    }

  /* We aren't handling anything sensitive, so give up on secure
   * memory, which is a scarce system resource.
   */
  gcry_control(GCRYCTL_DISABLE_SECMEM, 0);

  gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

  DPRINTF(E_DBG, L_MAIN, "Initialized with gcrypt %s\n", gcry_version);

  /* ptpd binds to priviliged ports 319 and 320 (if they are available) */
  ptpd_find_or_bind();

  /* Block signals for all threads except the main one */
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGHUP);
  sigaddset(&sigs, SIGCHLD);
  sigaddset(&sigs, SIGTERM);
  sigaddset(&sigs, SIGPIPE);
  ret = pthread_sigmask(SIG_BLOCK, &sigs, NULL);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_MAIN, "Error setting signal set\n");

      ret = EXIT_FAILURE;
      goto signal_block_fail;
    }

  /* Daemonize and drop privileges */
  ret = daemonize(background, pidfile);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MAIN, "Could not initialize server\n");

      ret = EXIT_FAILURE;
      goto daemon_fail;
    }

  /* Initialize event base (after forking) */
  CHECK_NULL(L_MAIN, evbase_main = event_base_new());

  CHECK_ERR(L_MAIN, evthread_use_pthreads());

  DPRINTF(E_LOG, L_MAIN, "mDNS init\n");
  ret = mdns_init();
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "mDNS init failed\n");

      ret = EXIT_FAILURE;
      goto mdns_fail;
    }

  /* Initialize the database before starting */
  DPRINTF(E_INFO, L_MAIN, "Initializing database\n");
  ret = db_init(SQLITE_EXT_PATH);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Database init failed\n");

      ret = EXIT_FAILURE;
      goto db_fail;
    }

  /* Open a DB connection for the main thread */
  ret = db_perthread_init();
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not perform perthread DB init for main\n");

      ret = EXIT_FAILURE;
      goto db_fail;
    }

  /* Spawn worker thread */
  ret = worker_init();
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Worker thread failed to start\n");

      ret = EXIT_FAILURE;
      goto worker_fail;
    }

  /* Spawn cache thread */
  ret = cache_init();
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Cache thread failed to start\n");

      ret = EXIT_FAILURE;
      goto cache_fail;
    }

  /* Spawn library scan thread */
  ret = library_init();
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Library thread failed to start\n");

      ret = EXIT_FAILURE;
      goto library_fail;
    }

  /* Spawn player thread */
  ret = player_init();
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Player thread failed to start\n");

      ret = EXIT_FAILURE;
      goto player_fail;
    }

  /* Spawn HTTPd thread */
  ret = httpd_init();
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "HTTPd thread failed to start\n");

      ret = EXIT_FAILURE;
      goto httpd_fail;
    }

#ifdef HAVE_SIGNALFD
  /* Set up signal fd */
  sigfd = signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC);
  if (sigfd < 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not setup signalfd: %s\n", strerror(errno));

      ret = EXIT_FAILURE;
      goto signalfd_fail;
    }

  sig_event = event_new(evbase_main, sigfd, EV_READ, signal_signalfd_cb, NULL);
#elif defined(HAVE_KQUEUE)
  sigfd = kqueue();
  if (sigfd < 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not setup kqueue: %s\n", strerror(errno));

      ret = EXIT_FAILURE;
      goto signalfd_fail;
    }

  EV_SET(&ke_sigs[0], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
  EV_SET(&ke_sigs[1], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
  EV_SET(&ke_sigs[2], SIGHUP, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
  EV_SET(&ke_sigs[3], SIGCHLD, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);

  ret = kevent(sigfd, ke_sigs, 4, NULL, 0, NULL);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not register signal events: %s\n", strerror(errno));

      ret = EXIT_FAILURE;
      goto signalfd_fail;
    }

  sig_event = event_new(evbase_main, sigfd, EV_READ, signal_kqueue_cb, NULL);
#else
# error "Need either signalfd (Linux) or kqueue (BSD/macOS) for signal handling"
#endif
  if (!sig_event)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not create signal event\n");

      ret = EXIT_FAILURE;
      goto sig_event_fail;
    }

  event_add(sig_event, NULL);

  /* Run the loop */
  if (!testrun)
    event_base_dispatch(evbase_main);

  DPRINTF(E_LOG, L_MAIN, "Stopping gracefully\n");
  ret = EXIT_SUCCESS;

  event_free(sig_event);

  /*
   * On a clean shutdown, bring mDNS down first to give a chance
   * to the clients to perform a clean shutdown on their end
   */
  DPRINTF(E_LOG, L_MAIN, "mDNS deinit\n");
  mdns_deinit();

 sig_event_fail:
 signalfd_fail:
  DPRINTF(E_LOG, L_MAIN, "HTTPd deinit\n");
  httpd_deinit();

 httpd_fail:
  DPRINTF(E_LOG, L_MAIN, "Player deinit\n");
  player_deinit();

 player_fail:
  DPRINTF(E_LOG, L_MAIN, "Library scanner deinit\n");
  library_deinit();

 library_fail:
  DPRINTF(E_LOG, L_MAIN, "Cache deinit\n");
  cache_deinit();

 cache_fail:
  DPRINTF(E_LOG, L_MAIN, "Worker deinit\n");
  worker_deinit();

 worker_fail:
  DPRINTF(E_LOG, L_MAIN, "Database deinit\n");
  db_perthread_deinit();
  db_deinit();

 db_fail:
  if (ret == EXIT_FAILURE)
    {
      DPRINTF(E_LOG, L_MAIN, "mDNS deinit\n");
      mdns_deinit();
    }

 mdns_fail:
  event_base_free(evbase_main);

 daemon_fail:
  if (background)
    {
      ret = seteuid(0);
      if (ret < 0)
	DPRINTF(E_LOG, L_MAIN, "seteuid() failed: %s\n", strerror(errno));
      else
	{
	  ret = unlink(pidfile);
	  if (ret < 0)
	    DPRINTF(E_LOG, L_MAIN, "Could not unlink PID file %s: %s\n", pidfile, strerror(errno));
	}
    }

 signal_block_fail:
 gcrypt_init_fail:
  curl_global_cleanup();
#if HAVE_DECL_AVFORMAT_NETWORK_INIT
  avformat_network_deinit();
#endif

#if (LIBAVCODEC_VERSION_MAJOR < 58) || ((LIBAVCODEC_VERSION_MAJOR == 58) && (LIBAVCODEC_VERSION_MINOR < 18))
  av_lockmgr_register(NULL);
 ffmpeg_init_fail:
#endif

  DPRINTF(E_LOG, L_MAIN, "Exiting.\n");
  config_unload();
  logger_deinit();

  return ret;
}
