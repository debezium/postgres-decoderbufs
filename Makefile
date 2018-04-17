MODULE_big = decoderbufs
EXTENSION = decoderbufs

PROTOBUF_C_CFLAGS = $(shell pkg-config --cflags 'libprotobuf-c >= 1.0.0')
PROTOBUF_C_LDFLAGS = $(shell pkg-config --libs 'libprotobuf-c >= 1.0.0')

ifneq ($(USE_POSTGIS),false)
C_PARAMS = -DUSE_POSTGIS
POSTGIS_C_LDFLAGS = -L/usr/local/lib -llwgeom
endif

PG_CPPFLAGS += -std=c11 $(PROTOBUF_C_CFLAGS) -I/usr/local/include $(C_PARAMS)
SHLIB_LINK  += $(PROTOBUF_C_LDFLAGS) $(POSTGIS_C_LDFLAGS)

OBJS = src/decoderbufs.o src/proto/pg_logicaldec.pb-c.o

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
