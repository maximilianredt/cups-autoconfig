#!/bin/sh
# Run this to generate all the initial makefiles, etc.

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

ORIGDIR=`pwd`
cd $srcdir
PROJECT=cups-autoconfig
TEST_TYPE=-f
CONFIGURE=configure.in
FILE=src/cups-autoconfig.c
aclocalinclude="-I . $ACLOCAL_FLAGS"
DIE=0

have_libtool=false
if libtoolize --version < /dev/null > /dev/null 2>&1 ; then
	libtool_version=`libtoolize --version | sed 's/^[^0-9]*\([0-9.][0-9.]*\).*/\1/'`
	case $libtool_version in
	    1.4*|1.5*)
		have_libtool=true
		;;
	esac
fi
if $have_libtool ; then : ; else
	echo
	echo "You must have libtool 1.4 installed to compile $PROJECT."
	echo "Install the appropriate package for your distribution,"
	echo "or get the source tarball at http://ftp.gnu.org/gnu/libtool/"
	DIE=1
fi

(intltoolize --version) < /dev/null > /dev/null 2>&1 || {
    echo "You must have intltool installed."
    DIE=1
}

(autoconf --version) < /dev/null > /dev/null 2>&1 || {
	echo
	echo "You must have autoconf installed to compile $PROJECT."
	echo "Install the appropriate package for your distribution,"
	echo "or get the source tarball at http://ftp.gnu.org/gnu/autoconf/"
	DIE=1
}

(automake --version) < /dev/null > /dev/null 2>&1 || {
    echo
    echo "You must have automake installed to compile $PROJECT."
    echo "Get ftp://sourceware.cygnus.com/pub/automake/automake-1.4.tar.gz"
    echo "(or a newer version if it is available)"
    DIE=1
}

if test "$DIE" -eq 1; then
	exit 1
fi

test $TEST_TYPE $FILE || {
	echo "You must run this script in the top-level $PROJECT directory"
	exit 1
}

if test -z "$AUTOGEN_SUBDIR_MODE"; then
    if test -z "$*"; then
        echo "I am going to run ./configure with no arguments - if you wish "
        echo "to pass any to it, please specify them on the $0 command line."
    fi
fi

rm -rf autom4te.cache

case $CC in
*xlc | *xlc\ * | *lcc | *lcc\ *) am_opt=--include-deps;;
esac

echo "Running intltoolize..."
intltoolize --copy --force --automake

echo "Running libtoolize ..."
libtoolize --force || exit $?

echo "Running aclocal ..." 
aclocal $aclocalinclude || exit $?

echo "Running autoheader ..."
autoheader || exit $?

echo "Running automake ..."
automake --add-missing --gnu $am_opt || exit $?

echo "Running autoconf ..."
autoconf || exit $?

cd $ORIGDIR || exit $?

if test -z "$AUTOGEN_SUBDIR_MODE"; then
    $srcdir/configure --enable-maintainer-mode $AUTOGEN_CONFIGURE_ARGS "$@" || exit $?

    echo 
    echo "Now type 'make' to compile $PROJECT."
fi
