CXX ?= g++
CC ?= cc
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -Wpedantic
CFLAGS ?= -O2 -DNDEBUG -std=c11
NODE_ROOT ?= /usr
CPPFLAGS += $(shell pkg-config --cflags x11 xft fontconfig libcurl libjpeg libuv) -isystem $(NODE_ROOT)/include/node
LDLIBS += $(shell pkg-config --libs x11 xft fontconfig libcurl libjpeg libuv) -L$(NODE_ROOT)/lib -lnode -pthread -lm -ldl
CORE = src/video_core.cpp src/diagnostics.cpp src/player.cpp src/media_process.cpp
HEADERS = $(wildcard src/*.hpp)
JS_OBJECTS = build/acorn_data.o

all: cryget-desktop

test: cryget-desktop cryget-core-test cryget-player-test cryget-mux-test
	./cryget-desktop --self-test
	./cryget-core-test
	./cryget-player-test
	./cryget-mux-test

build/acorn_data.o: third_party/acorn/acorn_data.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@
cryget-core-test: src/core_test.cpp $(CORE) $(HEADERS) $(JS_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/core_test.cpp $(CORE) $(JS_OBJECTS) -o $@ $(LDLIBS)
cryget-player-test: src/player_test.cpp $(CORE) $(HEADERS) $(JS_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/player_test.cpp $(CORE) $(JS_OBJECTS) -o $@ $(LDLIBS)
cryget-mux-test: src/mux_test.cpp $(CORE) $(HEADERS) $(JS_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/mux_test.cpp $(CORE) $(JS_OBJECTS) -o $@ $(LDLIBS)
cryget-desktop: src/main.cpp $(CORE) $(HEADERS) $(JS_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/main.cpp $(CORE) $(JS_OBJECTS) -o $@ $(LDLIBS)

clean:
	rm -f cryget-desktop cryget-core-test cryget-player-test cryget-mux-test $(JS_OBJECTS)
.PHONY: all clean test
