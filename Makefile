PREFIX=/usr
CFLAGS=-Werror=return-type -Werror=format -fno-strict-aliasing -O2

ifeq ($(shell pkg-config --errors-to-stdout --print-errors xcb-randr),)
	XCB_TARGET=libxcb-randr.so.0
endif

all: libXrandr.so.2 libXinerama.so.1 $(XCB_TARGET) xcbtest

config.h: configure
	./configure

skeleton-xrandr.h: make_skeleton.py
	./make_skeleton.py X11/extensions/Xrandr.h XRR libXrandr.c RRCrtc,RROutput > $@ || { rm -f $@; exit 1; }

skeleton-xcb.h: make_skeleton.py
	./make_skeleton.py xcb/randr.h xcb_randr_ libxcb-randr.cpp xcb_randr_output_t,xcb_randr_crtc_t > $@ || { rm -f $@; exit 1; }

libXrandr.so: libXrandr.c config.h skeleton-xrandr.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $< -ldl

libxcb-randr.so: libxcb-randr.cpp config.h skeleton-xcb.h
	@# NOTE: not $(CXX), to avoid silent linking to libstdc++. We want to keep this C++ code as if it were "enhanced C",
	@# without heavy features and libraries.
	$(CC) -fno-exceptions $(CFLAGS) -fPIC -shared -o $@ $< -ldl

libXinerama.so.1 libXrandr.so.2: libXrandr.so
	[ -e $@ ] || ln -s $< $@

libxcb-randr.so.0: libxcb-randr.so
	[ -e $@ ] || ln -s $< $@


xcbtest: xcbtest.c
	$(CC) $(CFLAG) -o $@ $< -lX11 -lXrandr -lxcb -lxcb-randr


install: libXrandr.so libxcb-randr.so
	TARGET_DIR=`sed -nre 's/#define FAKEXRANDR_INSTALL_DIR "([^"]+)"/\1/p' config.h`; \
	[ -d $$TARGET_DIR ] || exit 1; \
	install libXrandr.so $$TARGET_DIR; \
	install libxcb-randr.so $$TARGET_DIR; \
	ln -sf libxcb-randr.so $$TARGET_DIR/libxcb-randr.so.0; \
	ln -sf libXrandr.so $$TARGET_DIR/libXrandr.so.2; \
	ln -sf libXrandr.so $$TARGET_DIR/libXinerama.so.1; \
	ldconfig
	install fakexrandr-manage.py $(PREFIX)/bin/fakexrandr-manage

uninstall: config.h
	TARGET_DIR=`sed -nre 's/#define FAKEXRANDR_INSTALL_DIR "([^"]+)"/\1/p' config.h`; \
	[ -d $$TARGET_DIR ] || exit 1; \
	strings $$TARGET_DIR/libXrandr.so | grep -q _is_fake_xrandr || exit 1; \
	rm -f $$TARGET_DIR/libXrandr.so $$TARGET_DIR/libXrandr.so.2 $$TARGET_DIR/libXinerama.so.1 $(PREFIX)/bin/fakexrandr-manage; \
	ldconfig

clean:
	rm -f libXrandr.so libxcb-randr.so libXrandr.so.2 libXinerama.so.1 $(XCB_TARGET) config.h skeleton-xcb.h skeleton-xrandr.h xcbtest
