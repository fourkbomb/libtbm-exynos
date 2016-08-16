#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  on the rights to use, copy, modify, merge, publish, distribute, sub
#  license, and/or sell copies of the Software, and to permit persons to whom
#  the Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
#  ADAM JACKSON BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
#  IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
#  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

AC_PREREQ(2.69)
AC_INIT(libtbm-android, 0.9.0)
AC_CONFIG_SRCDIR([Makefile.am])
AM_INIT_AUTOMAKE([dist-bzip2])

AM_CONFIG_HEADER([config.h])

AC_PROG_LIBTOOL
AC_PROG_CC

AC_HEADER_STDC

# android-hw-libs is temporary unless we have the hybris and have to use the android tool-chain
PKG_CHECK_MODULES(TBM_BACKEND_ANDROID, libtbm android-headers android-hw-libs dlog)

# set the dir for the tbm module
DEFAULT_TBM_MODULE_PATH="${libdir}/bufmgr"
AC_ARG_WITH(tbm-module-path, AS_HELP_STRING([--with-tbm-module-path=PATH], [tbm module dir]),
				[ TBM_MODULE_PATH="$withval" ],
				[ TBM_MODULE_PATH="${DEFAULT_TBM_MODULE_PATH}" ])
AC_SUBST(TBM_MODULE_PATH)

AC_OUTPUT([
	Makefile
	src/Makefile])

echo ""
echo "CFLAGS  : $CFLAGS"
echo "LDFLAGS : $LDFLAGS"
echo "TBM_BACKEND_ANDROID_CFLAGS : $TBM_BACKEND_ANDROID_CFLAGS"
echo "TBM_BACKEND_ANDROID_LIBS   : $TBM_BACKEND_ANDROID_LIBS"
echo "bufmgr_dir : $TBM_MODULE_PATH"
echo ""

