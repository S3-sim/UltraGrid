#! /bin/sh
set -e

[ -d m4 ] || mkdir m4

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

ORIGDIR=`pwd`

cd $srcdir
aclocal
autoheader
autoconf
# install config.guess config.sub install-sh missing
automake --add-missing -c 2>&1 || true # actual call will fail - we do not have Makefile.am

CONFIGURE_OPTS=

if [ -n "$DEBUG" ]; then
        CONFIGURE_OPTS="$CONFIGURE_OPTS --enable-debug"
fi

cd $ORIGDIR

$srcdir/configure $CONFIGURE_OPTS $@

cd $ORIGDIR

