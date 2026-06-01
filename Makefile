CC = gcc
CFLAGS = -O2 -Wall -D_GNU_SOURCE -DCONFIG_VERSION=\"0.1.0\"
INCLUDES = -I.
QUICKJS_SRC = quickjs/quickjs.c quickjs/libregexp.c quickjs/libunicode.c quickjs/dtoa.c

all: jscgi

jscgi: jscgi.c $(QUICKJS_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -rdynamic -o $@ jscgi.c $(QUICKJS_SRC) -lm -ldl

clean:
	rm -f jscgi

.PHONY: all clean
