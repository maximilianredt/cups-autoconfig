WARNING_FLAGS= -Wall -Wshadow -Wmissing-declarations -Wmissing-prototypes \
		       -Wdeclaration-after-statement -Wstrict-prototypes \
		       -Wfloat-equal -Wno-unused-parameter -Wno-sign-compare

PROG_CFLAGS = -DLIBDIR="\"@LIBDIR@\"" -DSYSCONFDIR="\"@SYSCONFDIR@\"" -DLOCALSTATEDIR="\"@LOCALSTATEDIR@\""

calibdir = $(libdir)/cups-autoconfig
calib_PROGRAMS = cups-autoconfig 
cups_autoconfig_SOURCES = cups-autoconfig.c
cups_autoconfig_LDFLAGS = $(GLIB_LIBS) $(DBUS_LIBS) $(HAL_LIBS)-lcups
cups_autoconfig_CFLAGS = $(AM_CFLAGS) $(WARNING_FLAGS) $(PROG_CFLAGS) $(GLIB_CFLAGS) $(DBUS_CFLAGS) $(HAL_CFLAGS)

CLEANFILES = $(sbin_PROGRAMS)
