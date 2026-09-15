# Optional C++23 wrapper; the default C build does not enter this directory.
CXX ?= c++
AR ?= ar
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
THREADLIB ?= -pthread
PREFIX ?= /usr/local
LIB ?= /lib
BACKEND ?= select
ifeq ($(BACKEND),select)
LIBNAME = ivy-cpp
CORE_NAME = ivy
CORE_PACKAGE = ivy-c
BUILD_DIR ?= ../build/cpp
else ifeq ($(BACKEND),glib)
LIBNAME = ivy-cpp-glib
CORE_NAME = glibivy
CORE_PACKAGE = ivy-glib
BUILD_DIR ?= ../build/cpp-glib
BACKEND_CFLAGS = $(shell pkg-config --cflags glib-2.0)
BACKEND_LIBS = $(shell pkg-config --libs glib-2.0)
else
$(error Unsupported BACKEND '$(BACKEND)'; choose select or glib)
endif
MAJOR ?= $(shell sed -n 's/^\#define IVYMAJOR_VERSION //p' ../version.h)
MINOR ?= $(shell sed -n 's/^\#define IVYMINOR_VERSION //p' ../version.h)

ARCHIVE = $(BUILD_DIR)/lib$(LIBNAME).a
SHARED = $(BUILD_DIR)/lib$(LIBNAME).so.$(MAJOR).$(MINOR)
SONAME = lib$(LIBNAME).so.$(MAJOR)
SHARED_LINKS = $(BUILD_DIR)/lib$(LIBNAME).so $(BUILD_DIR)/$(SONAME)
CORE_SHARED = ../lib$(CORE_NAME).so.$(MAJOR).$(MINOR)
PKGCONF = $(BUILD_DIR)/$(LIBNAME).pc
SOURCES = ivy.cpp ivy_subscription.cpp ivy_events.cpp ivy_timer.cpp ivy_send.cpp ivy_filters.cpp ivy_application.cpp ivy_mainloop.cpp ivy_thread.cpp
ifeq ($(BACKEND),glib)
SOURCES += ivy_glib.cpp
endif
OBJECTS = $(SOURCES:%.cpp=$(BUILD_DIR)/%.o)
HEADERS = ivy.hpp ivy_detail.hpp ivy_bus_private.hpp ivy_glib.hpp ivy_thread.hpp
API_HEADERS = $(wildcard api/*.hpp)

.PHONY: all static-libs shared-libs install clean FORCE
all: static-libs shared-libs $(PKGCONF)
static-libs: $(ARCHIVE)
shared-libs: $(SHARED) $(SHARED_LINKS)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: %.cpp $(HEADERS) $(API_HEADERS) ivy_internal.hpp ../ivy.h ../timer.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -I.. $(CXXFLAGS) $(BACKEND_CFLAGS) -std=c++23 $(THREADLIB) -fPIC -MMD -MP -c $< -o $@

$(ARCHIVE): $(OBJECTS)
	$(AR) rcs $@ $^

$(SHARED): $(OBJECTS) $(CORE_SHARED)
	$(CXX) $(LDFLAGS) -shared -Wl,--no-undefined -Wl,-soname,$(SONAME) -o $@ $^ $(THREADLIB) $(BACKEND_LIBS)

$(SHARED_LINKS): $(SHARED)
	ln -sf $(notdir $(SHARED)) $@

# Regenerate when PREFIX or LIB changes, as for the C pkg-config files.
$(PKGCONF): ivy-cpp.pc.in ../version.h FORCE | $(BUILD_DIR)
	sed -e 's,@PREFIX@,$(PREFIX),g; s,@LIB@,$(LIB),g; s,@MAJOR@,$(MAJOR),g; s,@MINOR@,$(MINOR),g; s,@THREADLIB@,$(THREADLIB),g; s,@LIBNAME@,$(LIBNAME),g; s,@CORE_PACKAGE@,$(CORE_PACKAGE),g' $< > $@

install: all
	install -d $(DESTDIR)$(PREFIX)$(LIB) $(DESTDIR)$(PREFIX)/include/Ivy/api $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m644 $(ARCHIVE) $(SHARED) $(DESTDIR)$(PREFIX)$(LIB)/
	ln -sf $(notdir $(SHARED)) $(DESTDIR)$(PREFIX)$(LIB)/lib$(LIBNAME).so
	ln -sf $(notdir $(SHARED)) $(DESTDIR)$(PREFIX)$(LIB)/$(SONAME)
	install -m644 $(HEADERS) $(DESTDIR)$(PREFIX)/include/Ivy/
	install -m644 $(API_HEADERS) $(DESTDIR)$(PREFIX)/include/Ivy/api/
	install -m644 $(PKGCONF) $(DESTDIR)$(PREFIX)/lib/pkgconfig/

clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) $(ARCHIVE) $(PKGCONF) $(BUILD_DIR)/lib$(LIBNAME).so $(BUILD_DIR)/lib$(LIBNAME).so.*

-include $(OBJECTS:.o=.d)
