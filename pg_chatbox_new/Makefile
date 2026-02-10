EXTENSION = pg_chatbox
MODULES = pg_chatbox
DATA = pg_chatbox--1.0.sql

PG_CONFIG = /usr/pgsql-17/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Disable LLVM / bitcode
PG_USE_LLVM_LTO = 0

override CFLAGS += -O2 -fPIC

# 🔴 Force curl to be linked
override LDFLAGS += -Wl,--whole-archive -lcurl -Wl,--no-whole-archive
