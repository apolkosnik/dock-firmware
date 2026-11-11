# Host-side test of the mouse report decoding and packet accumulation: make -C tests/usb_mouse
ROOT := ../..
CXX ?= g++
# -Wno-cpp: rust_api.h includes <cstdbool>, which newer libstdc++ flags as deprecated.
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Werror -Wno-cpp -fsanitize=address,undefined -fno-sanitize-recover=all
BUILD := build

.PHONY: test clean

test: $(BUILD)/test_usb_mouse
	./$(BUILD)/test_usb_mouse

$(BUILD)/test_usb_mouse: test_usb_mouse.cpp $(ROOT)/src/mouse/mouse.cpp $(ROOT)/include/mouse/mouse.h $(ROOT)/include/rust_api.h
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(ROOT)/include -o $@ test_usb_mouse.cpp $(ROOT)/src/mouse/mouse.cpp

clean:
	rm -rf $(BUILD)
