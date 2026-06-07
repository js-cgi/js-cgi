CC = gcc
CFLAGS = -O2 -Wall -Wno-unused-result -D_GNU_SOURCE
INCLUDES = -I.
QUICKJS_SRC = quickjs/quickjs.c quickjs/libregexp.c quickjs/libunicode.c quickjs/dtoa.c

all: js-cgi

js-cgi: js-cgi.c $(QUICKJS_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -rdynamic -o $@ js-cgi.c $(QUICKJS_SRC) -lm -ldl

clean:
	rm -f js-cgi

.PHONY: all clean
