CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-function
CXXFLAGS += -I../shared -std=c++14 -fno-strict-aliasing -ffp-contract=off -fno-math-errno

HEADERS := $(wildcard ../shared/*.h)
TESTS   := rngfix_test vscript_test trigger_test movement_test momsurffix_test style_keys_test restart_test autosync_test hudkeys_test mapdiff_test zones_test jumpstats_test release_sig_test

.PHONY: all test clean

all: $(TESTS)

$(filter-out jumpstats_test release_sig_test,$(TESTS)): %: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $< -o $@

# jumpstats.cpp takes CoreLog and PatchPointer from the platform's utils.h.
jumpstats_test: jumpstats_test.cpp ../shared/jumpstats.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -I../linux $< -o $@

RELEASE_SIG_SRCS := ../shared/release_sig.cpp ../shared/vendor/monocypher/monocypher.c \
                    ../shared/vendor/monocypher/monocypher-ed25519.c ../shared/vendor/sha256/sha256.c

# g++ builds the vendored .c files as C++, the same as the client builds do.
release_sig_test: release_sig_test.cpp $(RELEASE_SIG_SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $< $(RELEASE_SIG_SRCS) -o $@

# Not a test: the workflows check a release with it before publishing.
check_sums: check_sums.cpp $(RELEASE_SIG_SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) $< $(RELEASE_SIG_SRCS) -o $@

test: $(TESTS)
	for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(TESTS) check_sums
