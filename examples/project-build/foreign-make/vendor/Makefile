SOURCE_DIR ?= .
ARCHIVE ?= libforeign.a
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -ffreestanding -fno-stack-protector

.PHONY: archive
archive: $(ARCHIVE)

$(ARCHIVE): score.o
	ZERO_AR_DATE=1 "$(AR)" rcs "$@" score.o

score.o: $(MAKEFILE_LIST) $(SOURCE_DIR)/src/score.c \
         $(SOURCE_DIR)/include/score.h $(SOURCE_DIR)/include/score_config.h
	"$(CC)" $(CFLAGS) -I "$(SOURCE_DIR)/include" -c "$(SOURCE_DIR)/src/score.c" -o "$@"
