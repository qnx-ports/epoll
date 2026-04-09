CC        = gcc
CFLAGS    = -fPIC -fvisibility=hidden
LDFLAGS   = -shared -Wl,--exclude-libs,ALL -Wl,--unresolved-symbols=report-all
INCLUDES  = -I. -Ipublic

BUILD_DIR = build
PREFIX    ?= /usr
LIB_NAME  = libepoll.so
TARGET    = $(BUILD_DIR)/$(LIB_NAME)

SRCS      = epoll.c epoll-mgr.c
OBJS      = $(patsubst %.c, $(BUILD_DIR)/%.o, $(SRCS))

all: $(TARGET)
	rm -f $(OBJS)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/lib/$(LIB_NAME)

	install -D -m 644 epoll-iomsg.h $(DESTDIR)$(PREFIX)/include/epoll-iomsg.h
	mkdir -p $(DESTDIR)/$(PREFIX)/include/sys
	install -D -m 644 public/sys/epoll.h $(DESTDIR)$(PREFIX)/include/sys/epoll.h

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all install clean
