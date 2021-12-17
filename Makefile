EXTENSION    = pg_stat_errors
EXTVERSION   = $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")
TESTS        = $(wildcard test/sql/*.sql)
REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test


PG_CONFIG = pg_config

MODULE_big = $(EXTENSION)
OBJS = $(EXTENSION).o
prepare = ecodes.inc

EXTRA_CLEAN = $(prepare)

all: 

$(EXTENSION).o: $(prepare)

release-zip: all
	git archive --format zip --prefix=$(EXTENSION)-${EXTVERSION}/ --output ./$(EXTENSION)-${EXTVERSION}.zip HEAD
	unzip ./$(EXTENSION)-$(EXTVERSION).zip
	rm ./$(EXTENSION)-$(EXTVERSION).zip
	rm ./$(EXTENSION)-$(EXTVERSION)/.gitignore
	zip -r ./$(EXTENSION)-$(EXTVERSION).zip ./$(EXTENSION)-$(EXTVERSION)/
	rm ./$(EXTENSION)-$(EXTVERSION) -rf

DATA = $(wildcard *--*.sql)
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# generate ecodes.inc from errcodes.txt
ifeq ($(findstring $(MAJORVERSION), "9.4 9.5 9.6 10"), $(MAJORVERSION))
ecodes.inc: data/$(MAJORVERSION)/errcodes.txt scripts/gen-ecodes.pl
	$(PERL) $(srcdir)/scripts/gen-ecodes.pl $< > $@
else
ecodes.inc: $(top_srcdir)/../../share/errcodes.txt scripts/gen-ecodes.pl
	$(PERL) $(srcdir)/scripts/gen-ecodes.pl $< > $@
endif

