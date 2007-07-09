SUBDIRS = src po 

fdidir=$(datadir)/hal/fdi/policy/10osvendor
fdi_DATA=10-cups-autoconfig.fdi

sysconfigdir = $(sysconfdir)/sysconfig
sysconfig_DATA = printerconfig

WORKDIR := $(shell pwd)

rpm: dist
	mkdir -p $(WORKDIR)/rpm/SOURCES $(WORKDIR)/rpm/BUILD \
	$(WORKDIR)/rpm/RPMS $(WORKDIR)/rpm/SRPMS; \
	cp $(PACKAGE)-$(VERSION)*tar.gz $(WORKDIR)/rpm/SOURCES; \
	rpmbuild --define "_sourcedir $(WORKDIR)/rpm/SOURCES" \
	--define "_builddir $(WORKDIR)/rpm/BUILD" \
	--define "_srcrpmdir $(WORKDIR)/rpm/SRPMS" \
	--define "_rpmdir $(WORKDIR)/rpm/RPMS" -ba cups-autoconfig.spec

EXTRA_DIST = intltool-extract.in intltool-merge.in intltool-update.in 10-cups-autoconfig.fdi printerconfig as-ac-expand.m4
CLEANFILES = intltool-extract intltool-merge intltool-update
