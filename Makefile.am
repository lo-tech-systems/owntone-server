ACLOCAL_AMFLAGS = -I m4

RPM_SPEC_FILE = owntone.spec
CONF_FILE = owntone-settings.json
SYSTEMD_SERVICE_FILE = owntone.service
LOGROTATE_CONFIG_FILE = owntone

if COND_INSTALL_SYSTEMD
systemddir = $(SYSTEMD_DIR)
systemd_DATA = $(SYSTEMD_SERVICE_FILE)
endif

logrotatedir = $(sysconfdir)/logrotate.d
dist_logrotate_DATA = $(LOGROTATE_CONFIG_FILE)

BUILT_SOURCES = $(SYSTEMD_SERVICE_FILE) $(LOGROTATE_CONFIG_FILE)

SUBDIRS = src

dist_man_MANS = owntone.8

nobase_dist_doc_DATA = \
	UPGRADING \
	README.md \
	docs/configuration.md \
	docs/changelog.md \
	docs/control-clients/cli-api.md \
	docs/audio-outputs/airplay.md \
	docs/audio-outputs/chromecast.md \
	docs/artwork.md \
	docs/json-api.md \
	scripts/pairinghelper.sh

EXTRA_DIST = \
	$(CONF_FILE) \
	$(LOGROTATE_CONFIG_FILE).in \
	$(SYSTEMD_SERVICE_FILE).in \
	$(RPM_SPEC_FILE)

install-data-hook:
	$(MKDIR_P) "$(DESTDIR)$(localstatedir)/log"
	$(MKDIR_P) "$(DESTDIR)$(localstatedir)/run"
	$(MKDIR_P) "$(DESTDIR)$(localstatedir)/cache/$(PACKAGE)"
	( $(GETENT) group $(OWNTONE_GROUP) &> /dev/null || $(GROUPADD) --system $(OWNTONE_GROUP) )
	( $(GETENT) passwd $(OWNTONE_USER) &> /dev/null || $(USERADD) --system --no-create-home --gid $(OWNTONE_GROUP) --groups audio --shell /usr/sbin/nologin $(OWNTONE_USER) )
	( ! $(GETENT) group pulse-access &> /dev/null || $(USERMOD) --append --groups pulse-access $(OWNTONE_USER) )
	$(CHOWN) $(OWNTONE_USER):$(OWNTONE_GROUP) "$(DESTDIR)$(localstatedir)/cache/$(PACKAGE)"
# The settings file is now created and self-healed by the daemon at runtime.
# Keep the repository template for reference/documentation, but do not seed
# $(sysconfdir) during install.

uninstall-hook:
	( cd "$(DESTDIR)$(localstatedir)/cache" && rm -rf $(PACKAGE) )
	( cd "$(DESTDIR)$(localstatedir)/log" && rm -f "$(PACKAGE).log*" )
	( cd "$(DESTDIR)$(localstatedir)/run" && rm -f "$(PACKAGE).pid" )
	( ! $(GETENT) passwd $(OWNTONE_USER) &> /dev/null || $(USERDEL) $(OWNTONE_USER) )

CLEANFILES = $(BUILT_SOURCES)

do_subst = $(SED) -e 's|@sbindir[@]|$(sbindir)|g' \
             -e 's|@localstatedir[@]|$(localstatedir)|g' \
             -e 's|@PACKAGE[@]|$(PACKAGE)|g' \
             -e 's|@OWNTONE_USER[@]|$(OWNTONE_USER)|g'

# these files use $prefix, which is determined at build (not configure) time
$(SYSTEMD_SERVICE_FILE): Makefile
	$(AM_V_at)rm -f $@ $@-t
	$(AM_V_GEN)$(do_subst) "$(srcdir)/$@.in" > $@-t
	$(AM_V_at)mv $@-t $@

$(SYSTEMD_SERVICE_FILE): $(srcdir)/$(SYSTEMD_SERVICE_FILE).in

$(LOGROTATE_CONFIG_FILE): Makefile
	$(AM_V_at)rm -f $@ $@-t
	$(AM_V_GEN)$(do_subst) "$(srcdir)/$@.in" > $@-t
	$(AM_V_at)mv $@-t $@

$(LOGROTATE_CONFIG_FILE): $(srcdir)/$(LOGROTATE_CONFIG_FILE).in
