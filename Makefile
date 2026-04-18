# Thin CMake wrapper — preserves make / make test / make clean / CC=... / NO_SANITIZER=1
BUILD  := build
NPROC  := $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

CMAKE_FLAGS :=
ifdef NO_SANITIZER
CMAKE_FLAGS += -DNO_SANITIZER=ON
endif
ifdef CC
CMAKE_FLAGS += -DCMAKE_C_COMPILER=$(CC)
endif

.PHONY: all webtransportd test clean install

all webtransportd:
	cmake -B $(BUILD) $(CMAKE_FLAGS)
	cmake --build $(BUILD) --target webtransportd -j$(NPROC)

test:
	cmake -B $(BUILD) $(CMAKE_FLAGS)
	cmake --build $(BUILD) -j$(NPROC)
	ctest --test-dir $(BUILD) --output-on-failure

clean:
	rm -rf $(BUILD)
	rm -f webtransportd webtransportd.exe
	rm -f examples/echo examples/echo.exe
	rm -f examples/frame_hi examples/frame_hi.exe

install: all
	cmake --install $(BUILD)
