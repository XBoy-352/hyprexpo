# Else exist specifically for clang
ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif

CXXFLAGS = -shared -fPIC -g -std=c++2b -Wno-c++11-narrowing -Wno-narrowing
INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon lua5.4`
LIBS = `pkg-config --libs pangocairo xkbcommon lua5.4`

SRC = main.cpp Dispatchers.cpp PluginConfig.cpp Overview.cpp OverviewInteraction.cpp OverviewRender.cpp ExpoGesture.cpp OverviewPassElement.cpp HyprexpoLogic.cpp
HEADERS = globals.hpp Dispatchers.hpp PluginConfig.hpp HyprlandConfigCompat.hpp Overview.hpp OverviewInternal.hpp ExpoGesture.hpp OverviewPassElement.hpp HyprexpoConfig.hpp HyprexpoLogic.hpp
TARGET = hyprexpo.so
TEST_TARGET = HyprexpoLogicTests
INSTALL_DIR = /var/cache/hyprpm/$(USER)/hyprexpo
INSTALL_NAME = hyprexpo.so

all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(EXTRA_FLAGS) $(INCLUDES) $(SRC) -o $@ $(LIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(INSTALL_DIR)/$(INSTALL_NAME)

clean:
	rm -f ./$(TARGET) ./$(TEST_TARGET)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): HyprexpoLogic.cpp HyprexpoLogic.hpp HyprexpoConfig.hpp tests/HyprexpoLogicTests.cpp
	$(CXX) -std=c++2b -Wall -Wextra -Werror HyprexpoLogic.cpp tests/HyprexpoLogicTests.cpp -o $@

.PHONY: all clean install test
