AM_CFLAGS = \
	@TBM_BACKEND_ANDROID_CFLAGS@ \
	-I$(top_srcdir) \
	-I$(top_srcdir)/src

libtbm_android_la_LTLIBRARIES = libtbm_android.la
libtbm_android_ladir = @TBM_MODULE_PATH@
libtbm_android_la_LIBADD = @TBM_BACKEND_ANDROID_LIBS@

libtbm_android_la_SOURCES = \
	tbm_bufmgr_android.c
