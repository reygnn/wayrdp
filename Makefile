# A thin wrapper around meson and ninja; the build itself is meson.build.
#
#   make                     configure BUILDDIR if needed, then build
#   make test                unit tests (meson test)
#   make e2e MWC=…           every end-to-end script in test/, one after another
#   make e2e-mwc-rdp MWC=…   one of them (e2e-<script name without .sh>)
#   make install             meson install; DESTDIR is honoured
#   make uninstall           remove what make install put in place (needs BUILDDIR)
#   make reconfigure         meson setup --reconfigure with the variables below
#   make clean               ninja clean
#   make distclean           remove BUILDDIR
#
# BUILDDIR    build directory (default: build)
# BUILDTYPE   --buildtype for a new BUILDDIR (default: release; debug keeps asserts)
# SYSCONFDIR  --sysconfdir for a new BUILDDIR (default: /etc, where PAM looks for
#             pam.d/wayrdp; meson's own would be /usr/local/etc)
# MESONFLAGS  further options for meson setup, e.g. "-Dpam=disabled"
#
# BUILDTYPE, SYSCONFDIR and MESONFLAGS take effect when BUILDDIR is set up; for
# an existing one use make reconfigure.
#
# The variables of the test scripts (MWC, PAM_PASSWORD, CLIENT, EXPECT_CODEC,
# WAYRDP_WRAP, …) go through as they are, on the command line or from the
# environment. A script that skips (exit 77, e.g. pam-rdp without PAM_PASSWORD)
# does not fail make.

BUILDDIR   ?= build
BUILDTYPE  ?= release
SYSCONFDIR ?= /etc
MESONFLAGS ?=

MESON ?= meson
NINJA ?= ninja

SETUPFLAGS = --buildtype=$(BUILDTYPE) --sysconfdir=$(SYSCONFDIR) $(MESONFLAGS)

E2E = mwc-rdp pam-rdp pam-hangup mwc-probe clipboard-shutdown stuck-client unplug-output

.PHONY: all setup reconfigure test install uninstall clean distclean e2e $(addprefix e2e-,$(E2E))

all: setup
	$(NINJA) -C $(BUILDDIR)

setup: $(BUILDDIR)/build.ninja

# Only for a new BUILDDIR; ninja itself reruns meson when a meson.build changes.
$(BUILDDIR)/build.ninja:
	$(MESON) setup $(BUILDDIR) $(SETUPFLAGS)

reconfigure:
	$(MESON) setup --reconfigure $(BUILDDIR) $(SETUPFLAGS)

test: setup
	$(MESON) test -C $(BUILDDIR)

install: all
	$(MESON) install -C $(BUILDDIR) --no-rebuild

# meson's uninstall target works from BUILDDIR's install log, so it has to
# run before make distclean.
uninstall:
	@[ -f $(BUILDDIR)/meson-logs/install-log.txt ] || { echo "make: nothing installed from $(BUILDDIR)" >&2; exit 1; }
	$(NINJA) -C $(BUILDDIR) uninstall

clean:
	-[ ! -f $(BUILDDIR)/build.ninja ] || $(NINJA) -C $(BUILDDIR) clean

distclean:
	rm -rf $(BUILDDIR)

# Runs test/<name>.sh for each name in $(1) against BUILDDIR's wayrdp, one
# after another in one shell, so they don't run in parallel even under
# make -j. Exit 77 counts as skipped; the first failure stops.
run_e2e = for t in $(1); do \
	echo "== test/$$t.sh"; \
	rc=0; test/$$t.sh $(BUILDDIR)/wayrdp || rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "== $$t: skipped"; \
	elif [ $$rc -ne 0 ]; then echo "== $$t: FAILED ($$rc)"; exit $$rc; \
	else echo "== $$t: ok"; fi; \
	done

check-mwc:
	@[ -n "$(MWC)" ] || { echo "make: set MWC=/path/to/mwc for the end-to-end tests" >&2; exit 1; }

.PHONY: check-mwc

e2e: all check-mwc
	@$(call run_e2e,$(E2E))

$(addprefix e2e-,$(E2E)): e2e-%: all check-mwc
	@$(call run_e2e,$*)
