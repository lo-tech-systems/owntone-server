
sbin_PROGRAMS = owntone



PAIR_AP_SRC = \
	pair_ap/pair_fruit.c pair_ap/pair_homekit.c pair_ap/pair-tlv.c \
	pair_ap/pair.c pair_ap/pair-tlv.h pair_ap/pair-internal.h pair_ap/pair.h
AM_CPPFLAGS += -DCONFIG_GCRYPT


if COND_AVAHI
MDNS_SRC=mdns_avahi.c
else
MDNS_SRC=mdns_dnssd.c
endif


AM_CPPFLAGS += \
	$(OWNTONE_CPPFLAGS) \
	$(OWNTONE_OPTS_CPPFLAGS) \
	$(COMMON_CPPFLAGS) \
	\
	-D_GNU_SOURCE \
	-DDATADIR=\"$(pkgdatadir)\" \
	-DCONFDIR=\"$(sysconfdir)\" \
	-DSTATEDIR=\"$(localstatedir)\" \
	-DPKGLIBDIR=\"$(pkglibdir)\"

owntone_LDADD = \
	$(OWNTONE_LIBS) \
	$(OWNTONE_OPTS_LIBS) \
	$(COMMON_LIBS)

owntone_SOURCES = main.c \
	queue.c queue.h \
	db.h \
	logger.c logger.h \
	owntone_config.c owntone_config.h \
	$(MDNS_SRC) mdns.h \
	httpd_libevhttp.c \
	httpd.c httpd.h httpd_internal.h \
	httpd_jsonapi.c \
	transcode.c transcode.h \
	misc.c misc.h \
	misc_json.c misc_json.h \
	player.c player.h \
	worker.c worker.h \
	settings.c settings.h \
	input.h input.c \
	inputs/pipe.c \
	outputs.h outputs.c \
	outputs/rtp_common.h outputs/rtp_common.c \
	outputs/raop.c outputs/airplay.c $(PAIR_AP_SRC) \
	outputs/airplay_events.c outputs/airplay_events.h \
	evrtsp/rtsp.c evrtsp/evrtsp.h evrtsp/rtsp-internal.h evrtsp/log.h \
	evthr.c evthr.h \
	listener.c listener.h \
	commands.c commands.h \
	ptpd.c ptpd.h \
	outputs/plist_wrap.h

SUBDIRS = libairptp
