# Makefile for stablebox
#
# Copyright (C) 1999-2004 by Erik Andersen <andersen@codepoet.org>
#
# Licensed under GPLv2, see the file LICENSE in this tarball for details.
#

.PHONY: dummy subdirs release distclean clean config oldconfig menuconfig \
        tags check test depend dep buildtree hosttools _all checkhelp \
        sizes bloatcheck baseline objsizes

noconfig_targets := menuconfig config oldconfig randconfig hosttools \
	defconfig allyesconfig allnoconfig allbareconfig \
	clean distclean help \
	release tags

nocheck_targets := clean distclean help release tags

# the toplevel sourcedir
ifndef top_srcdir
top_srcdir=$(CURDIR)
endif
# toplevel directory of the object-tree
ifndef top_builddir
top_builddir=$(CURDIR)
endif

export srctree=$(top_srcdir)
vpath %/Config.in $(srctree)

DIRS:=applets archival archival/libunarchive coreutils console-tools \
	debianutils editors findutils init miscutils modutils networking \
	networking/libiproute networking/udhcp procps loginutils shell \
	sysklogd util-linux libpwdgrp coreutils/libcoreutils libbb

SRC_DIRS:=$(patsubst %,$(top_srcdir)/%,$(DIRS))

# That's our default target when none is given on the command line
_all:

CONFIG_CONFIG_IN = $(top_srcdir)/Config.in

ifeq ($(BUILD_SRC),)
ifdef O
  ifeq ("$(origin O)", "command line")
    BUILD_OUTPUT := $(O)
    top_builddir := $(O)
  endif
else
# If no alternate output-dir was specified, we build in cwd
# We are using BUILD_OUTPUT nevertheless to make sure that we create
# Rules.mak and the toplevel Makefile, in case they don't exist.
  BUILD_OUTPUT := $(top_builddir)
endif

# see if we are in verbose mode
BUILD_VERBOSE :=
ifdef V
  ifeq ("$(origin V)", "command line")
    BUILD_VERBOSE := $(V)
  endif
endif
ifdef VERBOSE
  ifeq ("$(origin VERBOSE)", "command line")
    BUILD_VERBOSE := $(VERBOSE)
  endif
endif

ifneq ($(strip $(BUILD_VERBOSE)),)
  export BUILD_VERBOSE
  CHECK_VERBOSE := -v
# ARFLAGS+=v
endif

ifneq ($(strip $(HAVE_DOT_CONFIG)),y)
# pull in settings early
-include $(top_srcdir)/Rules.mak
endif

# All object directories.
OBJ_DIRS := $(DIRS)
all_tree := $(patsubst %,$(top_builddir)/%,$(OBJ_DIRS) scripts scripts/config include)
all_tree: $(all_tree)
$(all_tree):
	@mkdir -p "$@"

ifneq ($(BUILD_OUTPUT),)
# Invoke a second make in the output directory, passing relevant variables
# Check that the output directory actually exists
saved-output := $(BUILD_OUTPUT)
BUILD_OUTPUT := $(shell cd $(BUILD_OUTPUT) && /bin/pwd)
$(if $(wildcard $(BUILD_OUTPUT)),, \
     $(error output directory "$(saved-output)" does not exist))

.PHONY: $(MAKECMDGOALS)

$(filter-out _all,$(MAKECMDGOALS)) _all: $(BUILD_OUTPUT)/Rules.mak $(BUILD_OUTPUT)/Makefile all_tree
	$(Q)$(MAKE) -C $(BUILD_OUTPUT) \
	top_srcdir=$(top_srcdir) \
	top_builddir=$(top_builddir) \
	BUILD_SRC=$(top_srcdir) \
	-f $(CURDIR)/Makefile $@

$(BUILD_OUTPUT)/Rules.mak:
	@echo > $@
	@echo top_srcdir=$(top_srcdir) >> $@
	@echo top_builddir=$(BUILD_OUTPUT) >> $@
	@echo include $(top_srcdir)/Rules.mak >> $@

$(BUILD_OUTPUT)/Makefile:
	@echo > $@
	@echo top_srcdir=$(top_srcdir) >> $@
	@echo top_builddir=$(BUILD_OUTPUT) >> $@
	@echo BUILD_SRC='$$(top_srcdir)' >> $@
	@echo include '$$(BUILD_SRC)'/Makefile >> $@

# Leave processing to above invocation of make
skip-makefile := 1
endif # ifneq ($(BUILD_OUTPUT),)
endif # ifeq ($(BUILD_SRC),)

ifeq ($(skip-makefile),)

# We only need a copy of the Makefile for the config targets and reuse
# the rest from the source directory, i.e. we do not cp ALL_MAKEFILES.
scripts/config/Makefile: $(top_srcdir)/scripts/config/Makefile
	cp $< $@

_all: all

help:
	@echo 'Cleaning:'
	@echo '  clean			- delete temporary files created by build'
	@echo '  distclean		- delete all non-source files (including .config)'
	@echo
	@echo 'Build:'
	@echo '  all			- Executable and documentation'
	@echo '  stablebox		- the swiss-army executable'
	@echo
	@echo 'Configuration:'
	@echo '  allnoconfig		- disable all symbols in .config'
	@echo '  allyesconfig		- enable all symbols in .config (see defconfig)'
	@echo '  allbareconfig		- enable all applets without any sub-features'
	@echo '  config		- text based configurator (of last resort)'
	@echo '  defconfig		- set .config to largest generic configuration'
	@echo '  menuconfig		- interactive curses-based configurator'
	@echo '  oldconfig		- resolve any unresolved symbols in .config'
	@echo '  hosttools  		- build sed for the host.'
	@echo '  			  You can use these commands if the commands on the host'
	@echo '  			  is unusable. Afterwards use it like:'
	@echo '			  make SED="$(top_builddir)/sed"'
	@echo
	@echo 'Installation:'
	@echo '  install		- install stablebox into $(PREFIX)'
	@echo '  uninstall'
	@echo
	@echo 'Development:'
	@echo '  baseline		- create stablebox_old for bloatcheck.'
	@echo '  bloatcheck		- show size difference between old and new versions'
	@echo '  check			- run the test suite for all applets'
	@echo '  checkhelp		- check for missing help-entries in Config.in'
	@echo '  randconfig		- generate a random configuration'
	@echo '  release		- create a distribution tarball'
	@echo '  sizes			- show size of all enabled stablebox symbols'
	@echo '  objsizes		- show size of each .o object built'
	@echo


include $(top_srcdir)/Rules.mak

ifneq ($(strip $(HAVE_DOT_CONFIG)),y)

# Default target if none was requested explicitly
all: menuconfig

# warn if no configuration exists and we are asked to build a non-config target
.config:
	@echo ""
	@echo "No $(top_builddir)/$@ found!"
	@echo "Please refer to 'make  help', section Configuration."
	@echo ""
	@exit 1

# configuration
# ---------------------------------------------------------------------------

scripts/config/conf: scripts/config/Makefile
	$(Q)$(MAKE) -C scripts/config conf
	-@if [ ! -f .config ] ; then \
		touch .config; \
	fi

scripts/config/mconf: scripts/config/Makefile
	$(Q)$(MAKE) -C scripts/config ncurses conf mconf
	-@if [ ! -f .config ] ; then \
		touch .config; \
	fi

menuconfig: scripts/config/mconf
	@[ -f .config ] || $(MAKE) $(MAKEFLAGS) defconfig
	@./scripts/config/mconf $(CONFIG_CONFIG_IN)

config: scripts/config/conf
	@./scripts/config/conf $(CONFIG_CONFIG_IN)

oldconfig: scripts/config/conf
	@./scripts/config/conf -o $(CONFIG_CONFIG_IN)

randconfig: scripts/config/conf
	@./scripts/config/conf -r $(CONFIG_CONFIG_IN)

allyesconfig: scripts/config/conf
	@./scripts/config/conf -y $(CONFIG_CONFIG_IN) > /dev/null
	@$(SED) -i -r -e "s/^(USING_CROSS_COMPILER)=.*/# \1 is not set/" .config
	@./scripts/config/conf -o $(CONFIG_CONFIG_IN) > /dev/null

allnoconfig: scripts/config/conf
	@./scripts/config/conf -n $(CONFIG_CONFIG_IN) > /dev/null

# defconfig is allyesconfig minus any features that are specialized enough
# or cause enough behavior change that the user really should switch them on
# manually if that's what they want.  Sort of "maximum sane config".

defconfig: scripts/config/conf
	@./scripts/config/conf -y $(CONFIG_CONFIG_IN) > /dev/null
	@$(SED) -i -r -e "s/^(USING_CROSS_COMPILER|CONFIG_(DEBUG.*|STATIC|PAM|BUILD_(AT_ONCE|LIBBUSYBOX)|FEATURE_(FULL_LIBBUSYBOX|SHARED_BUSYBOX|MTAB_SUPPORT|CLEAN_UP|UDHCP_DEBUG)|INSTALL_NO_USR))=.*/# \1 is not set/" .config
	@./scripts/config/conf -o $(CONFIG_CONFIG_IN) > /dev/null


allbareconfig: scripts/config/conf
	@./scripts/config/conf -y $(CONFIG_CONFIG_IN) > /dev/null
	@$(SED) -i -r -e "s/^(USING_CROSS_COMPILER|CONFIG_(DEBUG|STATIC|NC_GAPING_SECURITY_HOLE|BUILD_AT_ONCE)).*/# \1 is not set/" .config
	@$(SED) -i -e "/FEATURE/s/=.*//;/^[^#]/s/.*FEATURE.*/# \0 is not set/;" .config
	@echo "CONFIG_FEATURE_BUFFERS_GO_ON_STACK=y" >> .config
	@yes n | ./scripts/config/conf -o $(CONFIG_CONFIG_IN) > /dev/null

hosttools:
	$(Q)cp .config .config.bak || noold=yea
	$(Q)$(MAKE) CC="$(HOSTCC)" CFLAGS="$(HOSTCFLAGS) $(INCS)" allnoconfig
	$(Q)mv .config .config.in
	$(Q)(grep -v CONFIG_SED .config.in ; \
	 echo "CONFIG_SED=y" ; ) > .config
	$(Q)$(MAKE) CC="$(HOSTCC)" CFLAGS="$(HOSTCFLAGS) $(INCS)" oldconfig include/bb_config.h
	$(Q)$(MAKE) CC="$(HOSTCC)" CFLAGS="$(HOSTCFLAGS) $(INCS)" stablebox 
	$(Q)[ -f .config.bak ] && mv .config.bak .config || rm .config
	mv stablebox sed
	@echo "Now do: $(MAKE) SED=$(top_builddir)/sed <target>"

else # ifneq ($(strip $(HAVE_DOT_CONFIG)),y)

all: stablebox stablebox.links

# In this section, we need .config
-include $(top_builddir)/.config.cmd
include $(patsubst %,%/Makefile.in, $(SRC_DIRS))

endif # ifneq ($(strip $(HAVE_DOT_CONFIG)),y)

-include $(top_builddir)/.config
-include $(top_builddir)/.depend

stablebox_unstripped: .depend $(BUSYBOX_SRC) $(APPLET_SRC) $(libraries-y)
	$(do_link)

stablebox: stablebox_unstripped
	$(Q)cp stablebox_unstripped stablebox
	$(do_strip)

%.bflt: %_unstripped
	$(do_elf2flt)

stablebox.links: $(top_srcdir)/applets/busybox.mkll include/bb_config.h $(top_srcdir)/include/applets.h
	$(Q)-$(SHELL) $^ >$@

install: $(top_srcdir)/applets/install.sh stablebox stablebox.links
	$(Q)DO_INSTALL_LIBS="$(strip $(DO_INSTALL_LIBS))" \
		$(SHELL) $< $(PREFIX) $(INSTALL_OPTS)
ifeq ($(strip $(CONFIG_FEATURE_SUID)),y)
	@echo
	@echo
	@echo --------------------------------------------------
	@echo You will probably need to make your stablebox binary
	@echo setuid root to ensure all configured applets will
	@echo work properly.
	@echo --------------------------------------------------
	@echo
endif

check test: stablebox
	bindir=$(top_builddir) srcdir=$(top_srcdir)/testsuite SED="$(SED)" \
	$(SHELL) $(top_srcdir)/testsuite/runtest $(CHECK_VERBOSE)

checkhelp:
	$(Q)$(top_srcdir)/scripts/checkhelp.awk \
		$(wildcard $(patsubst %,%/Config.in,$(SRC_DIRS) ./))

sizes: stablebox_unstripped
	$(NM) --size-sort $(<)

bloatcheck: stablebox_old stablebox_unstripped
	@$(top_srcdir)/scripts/bloat-o-meter stablebox_old stablebox_unstripped

baseline: stablebox_unstripped
	@mv stablebox_unstripped stablebox_old

objsizes: stablebox_unstripped
	$(SHELL) $(top_srcdir)/scripts/objsizes

# The nifty new dependency stuff
scripts/bb_mkdep: $(top_srcdir)/scripts/bb_mkdep.c
	$(do_link.h)

DEP_INCLUDES := include/bb_config.h

ifeq ($(strip $(CONFIG_BBCONFIG)),y)
DEP_INCLUDES += include/bbconfigopts.h

include/bbconfigopts.h: .config $(top_srcdir)/scripts/config/mkconfigs
	$(disp_gen)
	$(Q)$(top_srcdir)/scripts/config/mkconfigs > $@
endif

ifeq ($(strip $(CONFIG_FEATURE_COMPRESS_USAGE)),y)
USAGE_BIN:=scripts/usage
$(USAGE_BIN): $(top_srcdir)/scripts/usage.c .config \
		$(top_srcdir)/include/usage.h
	$(do_link.h)

DEP_INCLUDES += include/usage_compressed.h

include/usage_compressed.h: .config $(USAGE_BIN) \
		$(top_srcdir)/scripts/usage_compressed
	$(Q)SED="$(SED)" $(SHELL) $(top_srcdir)/scripts/usage_compressed \
	"$(top_builddir)/scripts" > $@
endif # CONFIG_FEATURE_COMPRESS_USAGE

# workaround alleged bug in make-3.80, make-3.81
.NOTPARALLEL: .depend

depend dep: .depend
.depend: scripts/bb_mkdep $(USAGE_BIN) $(DEP_INCLUDES)
	$(disp_gen)
	$(Q)rm -f .depend
	$(Q)mkdir -p include/config
	$(Q)scripts/bb_mkdep -I $(top_srcdir)/include $(top_srcdir) > $@.tmp
	$(Q)mv $@.tmp $@

include/bb_config.h: .config
	@if [ ! -x $(top_builddir)/scripts/config/conf ] ; then \
	    $(MAKE) -C scripts/config conf; \
	fi;
	@$(top_builddir)/scripts/config/conf -o $(CONFIG_CONFIG_IN)

clean:
	- $(MAKE) -C scripts/config $@
	- rm -f pod2htm* *.gdb *.elf *~ core .*config.log \
	    stablebox.links \
	    .config.old stablebox stablebox_unstripped \
	    include/usage_compressed.h scripts/usage
	- rm -r -f _install testsuite/links
	- find . -name .\*.flags -o -name \*.o  -o -name \*.om -o -name \*.syn \
	    -o -name \*.os -o -name \*.osm -o -name \*.a | xargs rm -f

distclean: clean
	rm -f scripts/bb_mkdep scripts/usage
	rm -r -f include/config include/config.h $(DEP_INCLUDES)
	find . -name .depend'*' -print0 | xargs -0 rm -f
	rm -f .hdepend
	rm -f .config .config.old .config.cmd

release: distclean
	cd ..; \
	rm -r -f $(PROG)-$(VERSION); \
	cp -a stablebox $(PROG)-$(VERSION); \
	\
	find $(PROG)-$(VERSION)/ -type d \
		-name .svn \
		-print \
		-exec rm -r -f {} \; ; \
	\
	find $(PROG)-$(VERSION)/ -type f \
		-name .\#* \
		-print \
		-exec rm -f {} \; ; \
	\
	tar -cvjf $(PROG)-$(VERSION).tar.bz2 $(PROG)-$(VERSION)/;

tags:
	ctags -R .

# documentation, cross-reference
# Modern distributions already ship synopsis packages (e.g. debian)
# If you have an old distribution go to http://synopsis.fresco.org/
syn_tgt := $(wildcard $(patsubst %,%/*.c,$(SRC_DIRS)))
syn     := $(patsubst %.c, %.syn, $(syn_tgt))

%.syn: %.c
	synopsis -p C -l Comments.SSDFilter,Comments.Previous $(INCS) -Wp,verbose,debug,preprocess,cppflags="'$(CFLAGS) $(EXTRA_CFLAGS) $(LDFLAGS) $(PROG_CFLAGS) $(PROG_LDFLAGS) $(CFLAGS_COMBINE) $(APPLETS_DEFINE) $(BUSYBOX_DEFINE)'" -o $@ $<
html: $(syn)
	synopsis -f HTML -Wf,title="'BusyBox Documentation'" -o $@ $^


endif # ifeq ($(skip-makefile),)

