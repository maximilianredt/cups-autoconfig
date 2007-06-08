WARNING_FLAGS= -Wall -Wshadow -Wmissing-declarations -Wmissing-prototypes \
		       -Wdeclaration-after-statement -Wstrict-prototypes \
		       -Wfloat-equal -Wno-unused-parameter -Wno-sign-compare

PROG_CFLAGS = -DLIBDIR="\"@LIBDIR@\""

calloutdir = /usr/lib/hal/scripts
callout_SCRIPTS = hal-cups-autoconfig

calibdir = $(libdir)/cups-autoconfig
calib_PROGRAMS = cups-autoconfig 
cups_autoconfig_SOURCES = cups-autoconfig.c
cups_autoconfig_LDFLAGS = $(GLIB_LIBS) $(DBUS_LIBS) -lcups
cups_autoconfig_CFLAGS = $(AM_CFLAGS) $(WARNING_FLAGS) $(PROG_CFLAGS) $(GLIB_CFLAGS) $(DBUS_CFLAGS)

CLEANFILES = $(sbin_PROGRAMS)
