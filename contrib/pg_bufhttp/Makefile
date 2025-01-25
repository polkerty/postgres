# contrib/pg_bufhttp/Makefile

MODULE_big = pg_bufhttp
OBJS = \
	pg_bufhttp.o

EXTENSION = pg_bufhttp
PGFILEDESC = "pg_bufhttp - view contents of buffer manager over the network"

TAP_TESTS = 0

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_bufhttp
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
