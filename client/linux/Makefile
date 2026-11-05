# 64-bit only: modern CS:S on Linux is x86_64.
# libstdc++/libgcc are linked statically for the Steam Runtime's older ABI. That makes hiding
# our symbols mandatory: an LD_PRELOAD object sits first in the global scope and would
# interpose its operator new/delete and __cxa_* on the whole game.
# glibc_compat.cpp keeps the .so loadable on glibc 2.31; check with: make check-glibc
# Release builds go through `make sniper`: a host toolchain built for x86-64-v4 (CachyOS) bakes
# AVX-512 into the static libstdc++ and the .so dies with SIGILL on CPUs without it.

CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CXXFLAGS += -std=c++14 -fno-strict-aliasing -ffp-contract=off -fno-math-errno

SO_NAME  := bhoptools.so
EXE_NAME := bhoptools

SO_FLAGS := -shared -fPIC -m64 \
            -fno-stack-protector \
            -fvisibility=hidden -fvisibility-inlines-hidden \
            -static-libstdc++ -static-libgcc \
            -Wl,--no-undefined \
            -Wl,--exclude-libs,ALL \
            -Wl,--version-script=preload.ver \
            -Wl,--no-as-needed -l:libdl.so.2 -l:libpthread.so.0

EXE_FLAGS := -m64 \
             -static-libstdc++ -static-libgcc

.PHONY: all clean check-glibc sniper

all: $(SO_NAME) $(EXE_NAME)

SRCS := preload.cpp glibc_compat.cpp chat_glyphs.cpp singleletter_fix.cpp \
        ../shared/teleport_predict.cpp ../shared/checkpoint_predict.cpp \
        ../shared/speedo.cpp ../shared/jumpstats.cpp ../shared/engine_trace.cpp ../shared/momsurffix_predict.cpp \
        ../shared/settings.cpp ../shared/options_tab.cpp ../shared/spectate.cpp ../shared/download_progress.cpp ../shared/fastdl.cpp \
        ../shared/release_sig.cpp ../shared/vendor/monocypher/monocypher.c ../shared/vendor/monocypher/monocypher-ed25519.c \
        ../shared/vendor/sha256/sha256.c

$(SO_NAME): $(SRCS) $(wildcard *.h ../shared/*.h) preload.ver
	$(CXX) $(CXXFLAGS) -I. -I../shared -fPIC $(SRCS) -o $@ $(SO_FLAGS)

$(EXE_NAME): launcher.cpp
	$(CXX) $(CXXFLAGS) launcher.cpp -o $@ $(EXE_FLAGS)

check-glibc: $(SO_NAME)
	@v=$$(objdump -T $(SO_NAME) | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1); \
	echo "highest glibc symbol version: $$v"; \
	case "$$v" in GLIBC_2.[0-9]|GLIBC_2.[0-9].*|GLIBC_2.[12][0-9]|GLIBC_2.[12][0-9].*|GLIBC_2.3[01]) ;; *) echo "too new for glibc 2.31"; exit 1;; esac
	@n=$$(objdump -d $(SO_NAME) | grep -c zmm); echo "AVX-512 instructions: $$n"; [ "$$n" = 0 ]

SNIPER_IMAGE := registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest

sniper:
	docker run --rm -u $$(id -u):$$(id -g) -v "$(CURDIR)/../..":/src -w /src/client/linux $(SNIPER_IMAGE) \
		sh -c '$(MAKE) clean && $(MAKE) CXX=g++-14 && $(MAKE) check-glibc'

clean:
	rm -f $(SO_NAME) $(EXE_NAME)

.PHONY: test
test:
	$(MAKE) -C ../tests test
