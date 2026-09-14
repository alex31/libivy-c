# Optional C++23 wrapper; the default C build does not enter this directory.
CXX ?= c++
AR ?= ar
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
THREADLIB ?= -pthread
PREFIX ?= /usr/local
LIB ?= /lib
BUILD_DIR ?= ../build/cpp
MAJOR ?= $(shell sed -n 's/^\#define IVYMAJOR_VERSION //p' ../version.h)
MINOR ?= $(shell sed -n 's/^\#define IVYMINOR_VERSION //p' ../version.h)

ARCHIVE = $(BUILD_DIR)/libivy-cpp.a
SHARED = $(BUILD_DIR)/libivy-cpp.so.$(MAJOR).$(MINOR)
SONAME = libivy-cpp.so.$(MAJOR)
SHARED_LINKS = $(BUILD_DIR)/libivy-cpp.so $(BUILD_DIR)/$(SONAME)
CORE_SHARED = ../libivy.so.$(MAJOR).$(MINOR)
PKGCONF = $(BUILD_DIR)/ivy-cpp.pc

.PHONY: all static-libs shared-libs install clean FORCE
all: static-libs shared-libs $(PKGCONF)
static-libs: $(ARCHIVE)
shared-libs: $(SHARED) $(SHARED_LINKS)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/ivy.o: ivy.cpp ivy.hpp ../ivy.h ../timer.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) -I.. $(CXXFLAGS) -std=c++23 $(THREADLIB) -fPIC -MMD -MP -c $< -o $@

$(ARCHIVE): $(BUILD_DIR)/ivy.o
	$(AR) rcs $@ $^

$(SHARED): $(BUILD_DIR)/ivy.o $(CORE_SHARED)
	$(CXX) $(LDFLAGS) -shared -Wl,--no-undefined -Wl,-soname,$(SONAME) -o $@ $^ $(THREADLIB)

$(SHARED_LINKS): $(SHARED)
	ln -sf $(notdir $(SHARED)) $@

# Regenerate when PREFIX or LIB changes, as for the C pkg-config files.
$(PKGCONF): ivy-cpp.pc.in ../version.h FORCE | $(BUILD_DIR)
	sed -e 's,@PREFIX@,$(PREFIX),g; s,@LIB@,$(LIB),g; s,@MAJOR@,$(MAJOR),g; s,@MINOR@,$(MINOR),g; s,@THREADLIB@,$(THREADLIB),g' $< > $@

install: all
	install -d $(DESTDIR)$(PREFIX)$(LIB) $(DESTDIR)$(PREFIX)/include/Ivy $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m644 $(ARCHIVE) $(SHARED) $(DESTDIR)$(PREFIX)$(LIB)/
	ln -sf $(notdir $(SHARED)) $(DESTDIR)$(PREFIX)$(LIB)/libivy-cpp.so
	ln -sf $(notdir $(SHARED)) $(DESTDIR)$(PREFIX)$(LIB)/$(SONAME)
	install -m644 ivy.hpp $(DESTDIR)$(PREFIX)/include/Ivy/
	install -m644 $(PKGCONF) $(DESTDIR)$(PREFIX)/lib/pkgconfig/

clean:
	rm -f $(BUILD_DIR)/ivy.o $(BUILD_DIR)/ivy.d $(ARCHIVE) $(PKGCONF) $(BUILD_DIR)/libivy-cpp.so $(BUILD_DIR)/libivy-cpp.so.*

-include $(BUILD_DIR)/ivy.d
