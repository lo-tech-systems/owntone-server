/*
 * Copyright (C) 2009-2010 Julien BLACHE <jb@jblache.org>
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <inttypes.h>

#include <event2/event.h>

#include <regex.h>
#include <zlib.h>

#include "logger.h"
#include "owntone_config.h"
#include "misc.h"
#include "worker.h"
#include "evthr.h"
#include "httpd.h"
#include "httpd_internal.h"
#include "transcode.h"
#include "listener.h"
#include "player.h"
#define ERR_PAGE "<html>\n<head>\n" \
  "<title>%d %s</title>\n" \
  "</head>\n<body>\n" \
  "<h1>%s</h1>\n" \
  "</body>\n</html>\n"

extern struct httpd_module httpd_jsonapi;

static struct httpd_module *httpd_modules[] = {
    &httpd_jsonapi,
    NULL
};


static const char *httpd_allow_origin;
static int httpd_port;


// The server is designed around a single thread listening for requests. When
// received, the request is passed to a thread from the worker pool, where a
// handler will process it and prepare a response for the httpd thread to send
// back. The idea is that the httpd thread never blocks. The handler in the
// worker thread can block, but shouldn't hold the thread if it is a long-
// running request (e.g. a long poll), because then we can run out of worker
// threads. The handler should use events to avoid this. Handlers, that are non-
// blocking and where the response must not be delayed can use
// HTTPD_HANDLER_REALTIME, then the httpd thread calls it directly (sync)
// instead of the async worker. In short, you shouldn't need to increase the
// below.
#define THREADPOOL_NTHREADS 1

static struct evthr_pool *httpd_threadpool;


/* --------------------------- MODULES INTERFACE ---------------------------- */

static void
modules_handlers_unset(struct httpd_uri_map *uri_map)
{
  struct httpd_uri_map *uri;

  for (uri = uri_map; uri->preg; uri++)
    {
      regfree(uri->preg); // Frees allocation by regcomp
      free(uri->preg); // Frees our own calloc
    }
}

static int
modules_handlers_set(struct httpd_uri_map *uri_map)
{
  struct httpd_uri_map *uri;
  char buf[64];
  int ret;

  for (uri = uri_map; uri->handler; uri++)
    {
      uri->preg = calloc(1, sizeof(regex_t));
      if (!uri->preg)
	{
	  DPRINTF(E_LOG, L_HTTPD, "Error setting URI handler, out of memory");
	  goto error;
	}

      ret = regcomp(uri->preg, uri->regexp, REG_EXTENDED | REG_NOSUB);
      if (ret != 0)
	{
	  regerror(ret, uri->preg, buf, sizeof(buf));
	  DPRINTF(E_LOG, L_HTTPD, "Error setting URI handler, regexp error: %s\n", buf);
	  goto error;
	}
    }

  return 0;

 error:
  modules_handlers_unset(uri_map);
  return -1;
}

static int
modules_init(void)
{
  struct httpd_module **ptr;
  struct httpd_module *m;

  for (ptr = httpd_modules; *ptr; ptr++)
    {
      m = *ptr;
      m->initialized = (!m->init || m->init() == 0);
      if (!m->initialized)
	{
	  DPRINTF(E_FATAL, L_HTTPD, "%s init failed\n", m->name);
	  return -1;
	}

      if (modules_handlers_set(m->handlers) != 0)
	{
	  DPRINTF(E_FATAL, L_HTTPD, "%s handler configuration failed\n", m->name);
	  return -1;
	}
    }

  return 0;
}

static void
modules_deinit(void)
{
  struct httpd_module **ptr;
  struct httpd_module *m;

  for (ptr = httpd_modules; *ptr; ptr++)
    {
      m = *ptr;
      if (m->initialized && m->deinit)
	m->deinit();

      modules_handlers_unset(m->handlers);
    }
}

static struct httpd_module *
modules_search(const char *path)
{
  struct httpd_module **ptr;
  struct httpd_module *m;
  const char **test;
  bool is_found = false;

  for (ptr = httpd_modules; *ptr; ptr++)
    {
      m = *ptr;
      if (!m->request)
	continue;

      for (test = m->subpaths; *test && !is_found; test++)
	is_found = (strncmp(path, *test, strlen(*test)) == 0);

      for (test = m->fullpaths; *test && !is_found; test++)
	is_found = (strcmp(path, *test) == 0);

      if (is_found)
	return m;
    }

  return NULL;
}


/* --------------------------- REQUEST HELPERS ------------------------------ */

static void
cors_headers_add(struct httpd_request *hreq, const char *allow_origin)
{
  if (allow_origin)
    httpd_header_add(hreq->out_headers, "Access-Control-Allow-Origin", httpd_allow_origin);

  httpd_header_add(hreq->out_headers, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  httpd_header_add(hreq->out_headers, "Access-Control-Allow-Headers", "authorization");
}

static bool
is_cors_preflight(struct httpd_request *hreq, const char *allow_origin)
{
  return ( hreq->method == HTTPD_METHOD_OPTIONS && hreq->in_headers && allow_origin &&
           httpd_header_find(hreq->in_headers, "Origin") &&
           httpd_header_find(hreq->in_headers, "Access-Control-Request-Method") );
}

void
httpd_request_handler_set(struct httpd_request *hreq)
{
  struct httpd_uri_map *map;
  int ret;

  // Path with e.g. /api -> JSON module
  hreq->module = modules_search(hreq->path);
  if (!hreq->module)
    {
      return;
    }

  for (map = hreq->module->handlers; map->handler; map++)
    {
      // Check if handler supports the current http request method
      if (map->method && hreq->method && !(map->method & hreq->method))
	continue;

      ret = regexec(map->preg, hreq->path, 0, NULL, 0);
      if (ret != 0)
	continue;

      hreq->handler = map->handler;
      hreq->is_async = !(map->flags & HTTPD_HANDLER_REALTIME);
      break;
    }
}

void
httpd_redirect_to(struct httpd_request *hreq, const char *path)
{
  httpd_header_add(hreq->out_headers, "Location", path);

  httpd_send_reply(hreq, HTTP_MOVETEMP, "Moved", HTTPD_SEND_NO_GZIP);
}

/*
 * Checks if the given ETag matches the "If-None-Match" request header
 *
 * If the request does not contains a "If-None-Match" header value or the value
 * does not match the given ETag, it returns false (modified) and adds the
 * "Cache-Control" and "ETag" headers to the response header.
 *
 * @param req The request with request and response headers
 * @param etag The valid ETag for the requested resource
 * @return True if the given ETag matches the request-header-value "If-None-Match", otherwise false
 */
bool
httpd_request_etag_matches(struct httpd_request *hreq, const char *etag)
{
  const char *none_match;

  none_match = httpd_header_find(hreq->in_headers, "If-None-Match");

  // Return not modified, if given timestamp matches "If-Modified-Since" request header
  if (none_match && (strcasecmp(etag, none_match) == 0))
    return true;

  // Add cache headers to allow client side caching
  httpd_header_add(hreq->out_headers, "Cache-Control", "private,no-cache,max-age=0");
  httpd_header_add(hreq->out_headers, "ETag", etag);

  return false;
}

/*
 * Checks if the given timestamp matches the "If-Modified-Since" request header
 *
 * If the request does not contains a "If-Modified-Since" header value or the value
 * does not match the given timestamp, it returns false (modified) and adds the
 * "Cache-Control" and "Last-Modified" headers to the response header.
 *
 * @param req The request with request and response headers
 * @param mtime The last modified timestamp for the requested resource
 * @return True if the given timestamp matches the request-header-value "If-Modified-Since", otherwise false
 */
bool
httpd_request_not_modified_since(struct httpd_request *hreq, time_t mtime)
{
  char last_modified[1000];
  const char *modified_since;
  struct tm timebuf;

  modified_since = httpd_header_find(hreq->in_headers, "If-Modified-Since");

  strftime(last_modified, sizeof(last_modified), "%a, %d %b %Y %H:%M:%S %Z", gmtime_r(&mtime, &timebuf));

  // Return not modified, if given timestamp matches "If-Modified-Since" request header
  if (modified_since && (strcasecmp(last_modified, modified_since) == 0))
    return true;

  // Add cache headers to allow client side caching
  httpd_header_add(hreq->out_headers, "Cache-Control", "private,no-cache,max-age=0");
  httpd_header_add(hreq->out_headers, "Last-Modified", last_modified);

  return false;
}

void
httpd_response_not_cachable(struct httpd_request *hreq)
{
  // Remove potentially set cache control headers
  httpd_header_remove(hreq->out_headers, "Cache-Control");
  httpd_header_remove(hreq->out_headers, "Last-Modified");
  httpd_header_remove(hreq->out_headers, "ETag");

  // Tell clients that they are not allowed to cache this response
  httpd_header_add(hreq->out_headers, "Cache-Control", "no-store");
}

/* -------------------------- SPEAKER HANDLING ------------------------------- */

// Thread: player (must not block)
static void
httpd_speaker_update_handler(short event_mask, void *ctx)
{
  /* No-op: transcoding cache has been removed */
  (void)ctx;
}


/* ---------------------------- REQUEST CALLBACKS --------------------------- */

// Worker thread, invoked by request_cb() below
static void
request_async_cb(void *arg)
{
  struct httpd_request *hreq = *(struct httpd_request **)arg;

  DPRINTF(E_DBG, hreq->module->logdomain, "%s request '%s'\n", hreq->module->name, hreq->uri);

  // Some handlers require an evbase to schedule events
  hreq->evbase = worker_evbase_get();
  hreq->module->request(hreq);
}

// httpd thread
static void
request_cb(struct httpd_request *hreq, void *arg)
{
  if (is_cors_preflight(hreq, httpd_allow_origin))
    {
      httpd_send_reply(hreq, HTTP_OK, "OK", HTTPD_SEND_NO_GZIP);
      return;
    }
  else if (!hreq->uri || !hreq->uri_parsed)
    {
      DPRINTF(E_WARN, L_HTTPD, "Invalid URI in request: '%s'\n", hreq->uri);
      httpd_redirect_to(hreq, "/");
      return;
    }
  else if (!hreq->path)
    {
      DPRINTF(E_WARN, L_HTTPD, "Invalid path in request: '%s'\n", hreq->uri);
      httpd_redirect_to(hreq, "/");
      return;
    }

  httpd_request_handler_set(hreq);
  if (hreq->module && hreq->is_async)
    {
      worker_execute(request_async_cb, &hreq, sizeof(struct httpd_request *), 0);
    }
  else if (hreq->module)
    {
      DPRINTF(E_DBG, hreq->module->logdomain, "%s request: '%s'\n", hreq->module->name, hreq->uri);
      hreq->evbase = httpd_backend_evbase_get(hreq->backend);
      hreq->module->request(hreq);
    }
  else
    {
      DPRINTF(E_DBG, L_HTTPD, "Unhandled HTTP request: '%s'\n", hreq->uri);
      httpd_send_error(hreq, HTTP_NOTFOUND, "Not Found");
    }

  // Don't touch hreq here, if async it has been passed to a worker thread
}


struct evbuffer *
httpd_gzip_deflate(struct evbuffer *in)
{
  struct evbuffer *out;
  struct evbuffer_iovec iovec[1];
  z_stream strm;
  int ret;

  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;

  // Just to keep Coverity from complaining about uninitialized values
  strm.total_in = 0;
  strm.total_out = 0;

  // Set up a gzip stream (the "+ 16" in 15 + 16), instead of a zlib stream (default)
  ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
  if (ret != Z_OK)
    {
      DPRINTF(E_LOG, L_HTTPD, "zlib setup failed: %s\n", zError(ret));
      return NULL;
    }

  strm.next_in = evbuffer_pullup(in, -1);
  strm.avail_in = evbuffer_get_length(in);

  out = evbuffer_new();
  if (!out)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not allocate evbuffer for gzipped reply\n");
      goto out_deflate_end;
    }

  // We use this to avoid a memcpy. The 512 is an arbitrary padding to make sure
  // there is enough space, even if the compressed output should be slightly
  // larger than input (could happen with small inputs).
  ret = evbuffer_reserve_space(out, strm.avail_in + 512, iovec, 1);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not reserve memory for gzipped reply\n");
      goto out_evbuf_free;
    }

  strm.next_out = iovec[0].iov_base;
  strm.avail_out = iovec[0].iov_len;

  ret = deflate(&strm, Z_FINISH);
  if (ret != Z_STREAM_END)
    goto out_evbuf_free;

  iovec[0].iov_len -= strm.avail_out;

  evbuffer_commit_space(out, iovec, 1);
  deflateEnd(&strm);

  return out;

 out_evbuf_free:
  evbuffer_free(out);

 out_deflate_end:
  deflateEnd(&strm);

  return NULL;
}

// The httpd_send functions below can be called from a worker thread (with
// hreq->is_async) or directly from the httpd thread. In the former case, they
// will command sending from the httpd thread, since it is not safe to access
// the backend (evhttp) from a worker thread. hreq will be freed (again,
// possibly async) if the type is either _COMPLETE or _END.

void
httpd_send_reply(struct httpd_request *hreq, int code, const char *reason, enum httpd_send_flags flags)
{
  struct evbuffer *gzbuf;
  struct evbuffer *save;
  const char *param;
  int do_gzip;

  if (!hreq->backend)
    return;

  do_gzip = ( (!(flags & HTTPD_SEND_NO_GZIP)) &&
              (evbuffer_get_length(hreq->out_body) > 512) &&
              (param = httpd_header_find(hreq->in_headers, "Accept-Encoding")) &&
              (strstr(param, "gzip") || strstr(param, "*"))
            );

  cors_headers_add(hreq, httpd_allow_origin);

  if (do_gzip && (gzbuf = httpd_gzip_deflate(hreq->out_body)))
    {
      DPRINTF(E_DBG, L_HTTPD, "Gzipping response\n");

      httpd_header_add(hreq->out_headers, "Content-Encoding", "gzip");
      save = hreq->out_body;
      hreq->out_body = gzbuf;
      evbuffer_free(save);
    }

  httpd_send(hreq, HTTPD_REPLY_COMPLETE, code, reason, NULL, NULL);
}

void
httpd_send_reply_start(struct httpd_request *hreq, int code, const char *reason)
{
  cors_headers_add(hreq, httpd_allow_origin);

  httpd_send(hreq, HTTPD_REPLY_START, code, reason, NULL, NULL);
}

void
httpd_send_reply_chunk(struct httpd_request *hreq, httpd_connection_chunkcb cb, void *arg)
{
  httpd_send(hreq, HTTPD_REPLY_CHUNK, 0, NULL, cb, arg);
}

void
httpd_send_reply_end(struct httpd_request *hreq)
{
  httpd_send(hreq, HTTPD_REPLY_END, 0, NULL, NULL, NULL);
}

// This is a modified version of evhttp_send_error (credit libevent)
void
httpd_send_error(struct httpd_request *hreq, int error, const char *reason)
{
  evbuffer_drain(hreq->out_body, -1);
  httpd_headers_clear(hreq->out_headers);

  cors_headers_add(hreq, httpd_allow_origin);

  httpd_header_add(hreq->out_headers, "Content-Type", "text/html");
  httpd_header_add(hreq->out_headers, "Connection", "close");

  evbuffer_add_printf(hreq->out_body, ERR_PAGE, error, reason, reason);

  httpd_send(hreq, HTTPD_REPLY_COMPLETE, error, reason, NULL, NULL);
}

bool
httpd_request_is_trusted(struct httpd_request *hreq)
{
  return httpd_backend_peer_is_trusted(hreq->backend);
}

bool
httpd_request_is_authorized(struct httpd_request *hreq)
{
  const char *passwd;
  int ret;

  if (httpd_request_is_trusted(hreq))
    return true;

  passwd = config_get_str("admin_password", NULL);
  if (!passwd)
    {
      DPRINTF(E_LOG, L_HTTPD, "Web interface request to '%s' denied: No password set in the config\n", hreq->uri);

      httpd_send_error(hreq, HTTP_FORBIDDEN, "Forbidden");
      return false;
    }

  ret = httpd_basic_auth(hreq, "admin", passwd, PACKAGE " web interface");
  if (ret != 0)
    {
      // httpd_basic_auth has sent a reply (and logged an error, if relevant)
      return false;
    }

  return true;
}

int
httpd_basic_auth(struct httpd_request *hreq, const char *user, const char *passwd, const char *realm)
{
  char header[256];
  const char *auth;
  char *decoded_auth;
  char *delim;
  bool is_authorized;
  int ret;

  auth = httpd_header_find(hreq->in_headers, "Authorization");
  if (!auth)
    {
      DPRINTF(E_DBG, L_HTTPD, "No Authorization header\n");
      goto need_auth;
    }

  if (strncmp(auth, "Basic ", strlen("Basic ")) != 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Bad Authentication header in authorization attempt from %s\n", hreq->peer_address);
      goto need_auth;
    }

  auth += strlen("Basic ");

  decoded_auth = (char *)b64_decode(NULL, auth);
  if (!decoded_auth)
    {
      DPRINTF(E_LOG, L_HTTPD, "Bad Authentication header in authorization attempt from %s\n", hreq->peer_address);
      goto need_auth;
    }

  // Apple Music sends "iTunes_Music/1.4 ... (dt:1):password", which we need to
  // support even if it isn't according to the basic auth RFC that says the
  // username cannot include a colon. In addition, the password could have
  // colons that are not escaped. So delimiting user and password isn't
  // straightforward. Also, we don't want to make assumptions about Apple's
  // future changes to username (say they drop "(dt:1)" again).
  is_authorized = false;
  delim = strchr(decoded_auth, ':');
  while (!is_authorized && delim)
    {
      *delim = '\0';

      if (user)
	is_authorized = (strcmp(decoded_auth, user) == 0) && (constant_time_strcmp(delim + 1, passwd) == 0);
      else
	is_authorized = (constant_time_strcmp(delim + 1, passwd) == 0);

      *delim = ':';
      delim = strchr(delim + 1, ':');
    }

  free(decoded_auth);

  if (!is_authorized)
    {
      DPRINTF(E_LOG, L_HTTPD, "Bad username or password in authorization attempt from %s\n", hreq->peer_address);
      goto need_auth;
    }

  return 0;

 need_auth:
  ret = snprintf(header, sizeof(header), "Basic realm=\"%s\"", realm);
  if ((ret < 0) || (ret >= sizeof(header)))
    {
      httpd_send_error(hreq, HTTP_SERVUNAVAIL, "Internal Server Error");
      return -1;
    }

  httpd_header_add(hreq->out_headers, "WWW-Authenticate", header);
  evbuffer_add_printf(hreq->out_body, ERR_PAGE, HTTP_UNAUTHORIZED, "Unauthorized", "Authorization required");
  httpd_send_reply(hreq, HTTP_UNAUTHORIZED, "Unauthorized", HTTPD_SEND_NO_GZIP);
  return -1;
}

static int
bind_test(unsigned short port)
{
  struct net_socket socket = NET_SOCKET_INIT;
  int ret;

  ret = net_bind(&socket, &port, SOCK_STREAM, "httpd init");
  if (ret < 0)
    return -1;

  net_socket_close(&socket);
  return 0;
}

static void
thread_init_cb(struct evthr *thr, void *shared)
{
  struct event_base *evbase;
  httpd_server *server;

  thread_setname("httpd");

  CHECK_NULL(L_HTTPD, evbase = evthr_get_base(thr));
  CHECK_NULL(L_HTTPD, server = httpd_server_new(evbase, httpd_port, request_cb, NULL));

  // For CORS headers
  httpd_server_allow_origin_set(server, httpd_allow_origin);

  evthr_set_aux(thr, server);
}

static void
thread_exit_cb(struct evthr *thr, void *shared)
{
  httpd_server *server;

  server = evthr_get_aux(thr);
  httpd_server_free(server);
}

/* Thread: main */
int
httpd_init(void)
{
  int ret;

  // Read config
  httpd_port = config_get_int("port", 3689);
  httpd_allow_origin = config_get_str("allow_origin", "*");
  if (strlen(httpd_allow_origin) == 0)
    httpd_allow_origin = NULL;

  // Test that the port is free. We do it here because we can make a nicer exit
  // than we can in thread_init_cb(), where the actual binding takes place.
  ret = bind_test(httpd_port);
  if (ret < 0)
   {
      DPRINTF(E_FATAL, L_HTTPD, "Could not create HTTP server on port %d (server already running?)\n", httpd_port);
      return -1;
   }

  // Prepare modules, e.g. httpd_daap
  ret = modules_init();
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_HTTPD, "Modules init failed\n");
      goto error;
    }

  httpd_threadpool = evthr_pool_wexit_new(THREADPOOL_NTHREADS, thread_init_cb, thread_exit_cb, NULL);
  if (!httpd_threadpool)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not create httpd thread pool\n");
      goto error;
    }

  ret = evthr_pool_start(httpd_threadpool);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_HTTPD, "Could not spawn worker threads\n");
      goto error;
    }

  // We need to know about speaker format changes so we can ask the cache to
  // start preparing headers for mp4/alac if selected
  listener_add(httpd_speaker_update_handler, LISTENER_SPEAKER, NULL);

  return 0;

 error:
  httpd_deinit();
  return -1;
}

/* Thread: main */
void
httpd_deinit(void)
{
  listener_remove(httpd_speaker_update_handler);

  // Give modules a chance to hang up connections nicely
  modules_deinit();

  evthr_pool_stop(httpd_threadpool);
  evthr_pool_free(httpd_threadpool);
}
