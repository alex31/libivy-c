CXX ?= c++
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra -Wpedantic

PKG_CONFIG ?= pkg-config

# The ivy-cpp pkg-config module is provided by the ivy-c-dev Debian package.
IVY_CXXFLAGS ?= $(shell $(PKG_CONFIG) --cflags ivy-cpp 2>/dev/null)
IVY_LIBS ?= $(shell $(PKG_CONFIG) --libs ivy-cpp 2>/dev/null)

TARGETS = ivytranslater lifecycle callbacks inspection

all: $(TARGETS)

check-pkg:
	@$(PKG_CONFIG) --print-errors --exists ivy-cpp || { \
		echo "Error: pkg-config cannot resolve ivy-cpp and its dependencies." >&2; \
		echo "Install ivy-c-dev (Debian/Ubuntu) or set PKG_CONFIG_PATH to a matching Ivy installation." >&2; \
		exit 1; \
	}

%: %.cpp | check-pkg
	$(CXX) $(CXXFLAGS) $(IVY_CXXFLAGS) $< $(IVY_LIBS) -o $@

clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean check-pkg
