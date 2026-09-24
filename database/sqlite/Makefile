############################################################################
# apps/database/sqlite/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

include $(APPDIR)/Make.defs

SQLITEDIR=$(APPDIR)/database/sqlite
BUILDDIR=$(SQLITEDIR)/build

CSRCS += ${BUILDDIR}/sqlite3.c

CFLAGS += ${INCDIR_PREFIX}$(SQLITEDIR)
CFLAGS += ${DEFINE_PREFIX}_HAVE_SQLITE_CONFIG_H
CFLAGS += ${DEFINE_PREFIX}NDEBUG
CFLAGS += -Wno-unused-variable -Wno-undef -Wno-unused-function -Wno-shadow

# Leave out what a small system does not use: extensions, the deprecated
# and shared-cache interfaces, JSON, WAL (it needs a shared memory map of
# a file) and memory-mapped I/O; temporary tables stay in memory

ifneq ($(CONFIG_LIB_SQLITE_SMALL),)
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_LOAD_EXTENSION
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_DEPRECATED
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_SHARED_CACHE
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_PROGRESS_CALLBACK
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_JSON
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_WAL
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_DECLTYPE
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_TRACE
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_UTF16
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_COMPLETE
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_GET_TABLE
CFLAGS += ${DEFINE_PREFIX}SQLITE_OMIT_TCL_VARIABLE
CFLAGS += ${DEFINE_PREFIX}SQLITE_DEFAULT_MEMSTATUS=0
CFLAGS += ${DEFINE_PREFIX}SQLITE_MAX_MMAP_SIZE=0
CFLAGS += ${DEFINE_PREFIX}SQLITE_TEMP_STORE=3
CFLAGS += ${DEFINE_PREFIX}SQLITE_DQS=0
CFLAGS += ${DEFINE_PREFIX}SQLITE_LIKE_DOESNT_MATCH_BLOBS
endif

ifneq ($(CONFIG_UTILS_SQLITE),)
PROGNAME  = sqlite3
PRIORITY  = 100
STACKSIZE = ${CONFIG_UTILS_SQLITE_STACKSIZE}
MODULE = $(CONFIG_UTILS_SQLITE)
MAINSRC = ${BUILDDIR}/shell.c
endif


# The amalgamation: generated from a git checkout in sqlite/ (which needs
# tclsh), or else sqlite.org's own, which needs nothing

SQLITE_VERSION = $(patsubst "%",%,$(CONFIG_LIB_SQLITE_VERSION))
SQLITE_YEAR    = $(patsubst "%",%,$(CONFIG_LIB_SQLITE_YEAR))
SQLITE_NUMBER  = $(shell echo $(SQLITE_VERSION) | \
                   awk -F. '{ printf "%d%02d%02d00", $$1, $$2, $$3 }')
SQLITE_AMALG   = sqlite-amalgamation-$(SQLITE_NUMBER)

${BUILDDIR}/sqlite3.c:
ifneq ($(wildcard sqlite/.git),)
	$(Q) mkdir -p ${BUILDDIR} && \
	cd ${BUILDDIR} && \
	echo "Generate SQLite amalgamation version" && \
	${SQLITEDIR}/sqlite/configure 1> /dev/null && \
	make sqlite3.c -j 1> /dev/null
else
	$(Q) curl -L -f https://www.sqlite.org/$(SQLITE_YEAR)/$(SQLITE_AMALG).zip \
	  -o $(SQLITE_AMALG).zip
	$(Q) unzip -q -o $(SQLITE_AMALG).zip
	$(Q) mkdir -p ${BUILDDIR}
	$(Q) mv $(SQLITE_AMALG)/* ${BUILDDIR}/
	$(call DELDIR, $(SQLITE_AMALG))
	$(call DELFILE, $(SQLITE_AMALG).zip)
endif

context:: ${BUILDDIR}/sqlite3.c

clean::
	$(call DELFILE, ${BUILDDIR}/*.o)

distclean::
	$(call DELDIR, ${BUILDDIR})
ifeq ($(wildcard sqlite/.git),)
	$(call DELDIR, sqlite)
endif

include $(APPDIR)/Application.mk
