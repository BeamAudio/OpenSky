CXX      := g++
CC       := gcc
CMAKE    ?= cmake
CXXFLAGS := -std=c++20 -O3 -mtune=native -fno-math-errno -Wall -Wextra -Wno-unused-parameter -Ithird_party -I../engine/src -isystem ../engine/thirdparty/jsbsim/src -I../protocol
CFLAGS   := -O3 -mtune=native -fno-math-errno -Ithird_party
# The target Renoir CPU supports AVX2/FMA. Enable those lanes when the build host
# supports them; older x86 hosts keep the compiler's portable baseline path.
HOST_SIMD_FLAGS := $(shell uname -m 2>/dev/null | grep -Eq 'x86_64|amd64' && grep -qm1 '\<avx2\>' /proc/cpuinfo 2>/dev/null && echo '-mavx2 -mfma')
CXXFLAGS += $(HOST_SIMD_FLAGS) -ffp-contract=fast -fopenmp-simd
CFLAGS   += $(HOST_SIMD_FLAGS)
LDFLAGS  := -L../game/bin -Wl,-rpath,'$$ORIGIN/../game/bin' -lJSBSim -lX11 -ldl -lpthread -lm

JSBSIM_ROOT := ../engine/thirdparty/jsbsim
JSBSIM_BUILD := .build/jsbsim
JSBSIM_SONAME := ../game/bin/libJSBSim.so.1
JSBSIM_LIB := ../game/bin/libJSBSim.so
JSBSIM_INPUTS := $(shell find $(JSBSIM_ROOT)/src -type f) $(JSBSIM_ROOT)/CMakeLists.txt $(JSBSIM_ROOT)/JSBSimConfig.cmake.in

SRCS := \
	src/gl_loader.cpp \
	src/flight_profiler.cpp \
	src/platform_window.cpp \
	src/pack_reader.cpp \
	src/async_streamer.cpp \
	src/cdlod_terrain.cpp \
	src/volumetric_clouds.cpp \
	src/regional_atmosphere.cpp \
	src/weather_library.cpp \
	src/world_objects.cpp \
	src/living_traffic.cpp \
	src/airfield_vegetation.cpp \
	src/audio_engine.cpp \
	src/aircraft_hud.cpp \
	src/game_frontend.cpp \
	src/debug_overlays.cpp \
	src/main.cpp \
	../engine/src/c172_systems.cpp \
	../engine/src/flight_model_jsb.cpp \
	../engine/src/opentrack_udp.cpp

CSRCS := third_party/miniz_tinfl.c

THIRDPARTY_CPP := third_party/bc7enc.cpp
OBJS := $(SRCS:.cpp=.o) $(THIRDPARTY_CPP:.cpp=.o) $(CSRCS:.c=.o)
TARGET := nspain_sim

all: $(TARGET)

$(TARGET): $(OBJS) $(JSBSIM_SONAME) $(JSBSIM_LIB)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# Package the SONAME alongside the game so the executable loads the upstream ABI.
$(JSBSIM_SONAME): $(JSBSIM_INPUTS)
	$(CMAKE) -S $(JSBSIM_ROOT) -B $(JSBSIM_BUILD) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(JSBSIM_BUILD) --target libJSBSim --parallel 6
	mkdir -p ../game/bin
	cp -L $(JSBSIM_BUILD)/src/libJSBSim.so.1 $@.tmp
	mv $@.tmp $@

$(JSBSIM_LIB): $(JSBSIM_SONAME)
	ln -sfn $(notdir $(JSBSIM_SONAME)) $@

src/%.o: src/%.cpp $(wildcard src/*.hpp) $(wildcard src/*.h) ../engine/src/flight_model_jsb.hpp ../engine/src/c172_systems.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

third_party/%.o: third_party/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

../engine/src/%.o: ../engine/src/%.cpp $(wildcard ../engine/src/*.hpp)
	$(CXX) $(CXXFLAGS) -c $< -o $@

third_party/%.o: third_party/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(JSBSIM_LIB) $(JSBSIM_SONAME) src/*.o third_party/*.o ../engine/src/*.o

.build/test_flight_and_systems: ../engine/tests/test_flight_and_systems.cpp ../engine/src/flight_model_jsb.cpp ../engine/src/c172_systems.cpp $(JSBSIM_SONAME) $(JSBSIM_LIB)
	mkdir -p .build
	$(CXX) $(CXXFLAGS) ../engine/tests/test_flight_and_systems.cpp ../engine/src/flight_model_jsb.cpp ../engine/src/c172_systems.cpp \
		-o $@ -L../game/bin -Wl,-rpath,'$$ORIGIN/../../game/bin' -lJSBSim

flight-test: .build/test_flight_and_systems
	cd .. && LD_LIBRARY_PATH=game/bin ./sim_engine/.build/test_flight_and_systems

.PHONY: all clean flight-test
