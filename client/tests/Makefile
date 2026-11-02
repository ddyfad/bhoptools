CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-function
CXXFLAGS += -I../shared -std=c++14 -fno-strict-aliasing -ffp-contract=off -fno-math-errno

HEADERS := $(wildcard ../shared/*.h)
TESTS   := rngfix_test vscript_test trigger_test movement_test momsurffix_test style_keys_test restart_test autosync_test hudkeys_test mapdiff_test zones_test jumpstats_test

.PHONY: all test clean

all: $(TESTS)

$(filter-out jumpstats_test,$(TESTS)): %: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $< -o $@

# jumpstats.cpp takes CoreLog and PatchPointer from the platform's utils.h.
jumpstats_test: jumpstats_test.cpp ../shared/jumpstats.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -I../linux $< -o $@

test: $(TESTS)
	for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(TESTS)
