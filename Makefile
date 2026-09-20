CXX ?= g++
RAYLIB_DIR ?= /home/jon/raylib-src
LIBS = $(RAYLIB_DIR)/build-linux/raylib/libraylib.a -lm -lpthread -ldl -lrt -lGL -lX11 -lXrandr -lXinerama -lXi -lXcursor -lXext
recomp-launcher: src/main.cpp src/settings.hpp
	$(CXX) -std=c++17 -O2 -Wall -Wextra -I $(RAYLIB_DIR)/src src/main.cpp $(LIBS) -o $@
test: tests/settings.cpp src/settings.hpp
	$(CXX) -std=c++17 -O2 -Wall -Wextra -I src tests/settings.cpp -o tests/settings_test
	./tests/settings_test
.PHONY: test
