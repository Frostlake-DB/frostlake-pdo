# Builds the pdo_frostlake extension against the PHP that php-config describes.
#
#   make                       build/pdo_frostlake.so
#   make test                  run the suite against a throwaway Frostlake server
#   make PHP_CONFIG=php-config8.4
#   make PHP_INCLUDE_DIR=/path/to/include/php/20240924   (headers without php-config)

PHP_CONFIG ?= php-config
PHP ?= php
PHP_INCLUDE_DIR ?= $(shell $(PHP_CONFIG) --include-dir 2>/dev/null)

CC ?= cc
CFLAGS ?= -O2 -g
# What the extension needs whatever CFLAGS or LDFLAGS a caller passes.
EXT_CFLAGS := -std=gnu11 -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter \
              -D_GNU_SOURCE -DCOMPILE_DL_PDO_FROSTLAKE
PHP_INCLUDES = -I$(PHP_INCLUDE_DIR) -I$(PHP_INCLUDE_DIR)/main -I$(PHP_INCLUDE_DIR)/TSRM \
               -I$(PHP_INCLUDE_DIR)/Zend -I$(PHP_INCLUDE_DIR)/ext -I$(PHP_INCLUDE_DIR)/ext/date/lib

SRC := src/pdo_frostlake.c src/frostlake_driver.c src/frostlake_statement.c \
       src/frostlake_sql.c src/frostlake_wire.c src/json.c src/http.c
OBJ := $(SRC:src/%.c=build/%.o)
HEADERS := $(wildcard src/*.h)

.PHONY: all test clean check-headers

all: build/pdo_frostlake.so

check-headers:
	@test -n "$(PHP_INCLUDE_DIR)" && test -f "$(PHP_INCLUDE_DIR)/ext/pdo/php_pdo_driver.h" || { \
	  echo "PHP headers not found: install the php-dev package or pass PHP_INCLUDE_DIR=..." >&2; exit 1; }

build/%.o: src/%.c $(HEADERS) | check-headers
	@mkdir -p build
	$(CC) $(PHP_INCLUDES) $(CPPFLAGS) $(EXT_CFLAGS) $(CFLAGS) -c $< -o $@

build/pdo_frostlake.so: $(OBJ)
	$(CC) -shared $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ)

test: build/pdo_frostlake.so
	PHP="$(PHP)" tests/run.sh

clean:
	rm -rf build
