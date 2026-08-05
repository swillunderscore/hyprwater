# HyprGlass Plugin

CXX ?= g++
# -MMD -MP: emit a .d file per object listing the headers it included, so a
# header edit rebuilds every file that sees it. Without this, changing a struct
# in Globals.hpp recompiled only the .cpp you touched and left the rest linked
# against the OLD layout — which does not fail to build, it CRASHES at runtime
# in whichever field moved. Cost one afternoon; never again.
CXXFLAGS = -fPIC -g -O2 -std=c++23 -MMD -MP
LDFLAGS = -shared
INCLUDES = $(shell pkg-config --cflags hyprland pixman-1 libdrm)
LIBS = $(shell pkg-config --libs hyprland)

ifeq ($(basename $(CXX)),g++)
	CXXFLAGS += --no-gnu-unique
endif

TARGET = hyprwater.so
SOURCES = src/main.cpp src/GlassDecoration.cpp src/GlassPassElement.cpp src/GlassRenderer.cpp src/GlassLayerSurface.cpp src/GlassLayerPassElement.cpp src/GlassLayerCompositeElement.cpp src/PluginConfig.cpp src/ShaderManager.cpp
OBJ = $(SOURCES:.cpp=.o)
DEPS = $(OBJ:.o=.d)

all: $(TARGET)

%.o : %.cpp
	@echo "[$(CXX)] $<"
	@$(CXX) -c $(CXXFLAGS) $(INCLUDES) $< -o $@

$(TARGET): $(OBJ)
	@echo "Linking $(TARGET)..."
	@$(CXX) $(LDFLAGS) $(OBJ) -o $@.tmp $(LIBS)
	@mv -f $@.tmp $@
	@echo "Done!"

clean:
	rm -f $(OBJ) $(DEPS) $(TARGET) $(TARGET).tmp

-include $(DEPS)

.PHONY: all clean
