
#ifndef __LOGGER_H__
#define __LOGGER_H__

#include <stdarg.h>

/* Log domains */
#define L_CONF        0
#define L_HTTPD       3
#define L_MAIN        5
#define L_MDNS        6
#define L_MISC        7
#define L_XCODE       10
/* libevent logging */
#define L_EVENT       11
#define L_FFMPEG      14
#define L_PLAYER      16
#define L_RAOP        17
#define L_WEB         29
#define L_AIRPLAY     30

#define N_LOGDOMAINS  32

/* Severities */
#define E_FATAL   0
#define E_LOG     1
#define E_WARN    2
#define E_INFO    3
#define E_DBG     4
#define E_SPAM    5



void
DPRINTF(int severity, int domain, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

void
DVPRINTF(int severity, int domain, const char *fmt, va_list ap);

void
DHEXDUMP(int severity, int domain, const unsigned char *data, int data_len, const char *heading);

void
logger_ffmpeg(void *ptr, int level, const char *fmt, va_list ap);

void
logger_libevent(int severity, const char *msg);

void
logger_reinit(void);

int
logger_severity(void);

void
logger_domains(void);

void
logger_detach(void);

int
logger_init(char *file, char *domains, int severity, char *logformat);

void
logger_deinit(void);


#endif /* !__LOGGER_H__ */
