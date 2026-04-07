
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


LEXER_SRC  = parsers/smartpl_lexer.l
PARSER_SRC = parsers/smartpl_parser.y

# This flag is given to Bison and tells it to produce headers. Note that
# automake recognizes this flag too, and has special logic around it, so don't
# change it to compound arguments (so for instance no "-dv"). I'm also not sure
# --defines will work instead of -d.
AM_YFLAGS = -d

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
	db.c db.h \
	db_init.c db_init.h \
	db_upgrade.c db_upgrade.h \
	logger.c logger.h \
	conffile.c conffile.h \
	cache.c cache.h \
	library.c library.h \
	$(MDNS_SRC) mdns.h \
	httpd_libevhttp.c \
	httpd.c httpd.h httpd_internal.h \
	httpd_jsonapi.c \
	httpd_artworkapi.c \
	http.c http.h \
	transcode.c transcode.h \
	artwork.c artwork.h \
	misc.c misc.h \
	misc_json.c misc_json.h \
	misc_xml.c misc_xml.h \
	rng.c rng.h \
	smartpl_query.c smartpl_query.h \
	player.c player.h \
	worker.c worker.h \
	settings.c settings.h \
	input.h input.c \
	inputs/pipe.c \
	outputs.h outputs.c \
	outputs/rtp_common.h outputs/rtp_common.c \
	outputs/raop.c outputs/airplay.c $(PAIR_AP_SRC) \
	outputs/airplay_events.c outputs/airplay_events.h \
	outputs/dummy.c outputs/fifo.c \
	evrtsp/rtsp.c evrtsp/evrtsp.h evrtsp/rtsp-internal.h evrtsp/log.h \
	evthr.c evthr.h \
	listener.c listener.h \
	commands.c commands.h \
	ptpd.c ptpd.h \
	outputs/plist_wrap.h \
	$(LEXER_SRC) $(PARSER_SRC)

# This should ensure the headers are built first. automake knows how to make
# parser headers, but doesn't know how to do that for flex. So instead we set
# the C files as target, as the AM_LFLAGS will make sure headers are produced.
BUILT_SOURCES = \
	$(LEXER_SRC:.l=.h) $(PARSER_SRC:.y=.h)

# automake doesn't know how to make lexer headers, nor does it automatically
# include them, so need to specify them as EXTRA_DIST.
EXTRA_DIST = \
	$(LEXER_SRC:.l=.h)

SUBDIRS = libairptp

# Rule for generating lexer headers. $@ is an automatic variable that is equal
# to the particular target name, so a header file name.
$(LEXER_SRC:.l=.h): $(LEXER_SRC)
	$(LEX) --header-file=$@ --stdout $(@:.h=.l) > /dev/null

# Anything built by make should be cleaned by make clean, but when it comes to
# flex/bison automake's support leaves something to be desired
clean-local:
	rm -f $(LEXER_SRC:.l=.[ch]) $(PARSER_SRC:.y=.[ch])
