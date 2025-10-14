# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../..

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS +=

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
LDFLAGS +=

FLAGS += -Idep/include -I./src/dep/dr_wav -I./src/dep/filters -I./src/dep/freeverb -I./src/dep/gverb/include -I./src/dep/minimp3 -I./src/dep/lodepng -I./src/dep/pffft -I./src/dep/AudioFile -I./src/dep/resampler -I./src/dep

SOURCES = $(wildcard src/*.cpp src/dep/filters/*.cpp src/dep/freeverb/*.cpp src/dep/gverb/src/*.c src/dep/lodepng/*.cpp src/dep/pffft/*.c src/dep/resampler/*.cpp src/dep/*.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk
