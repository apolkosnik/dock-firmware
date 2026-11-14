# Host-side test of the keyboard report decoding: make -C tests/usb_keyboard
ROOT := ../..
CXX ?= g++
# -Wno-cpp: rust_api.h includes <cstdbool>, which newer libstdc++ flags as deprecated.
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Werror -Wno-cpp -fsanitize=address,undefined -fno-sanitize-recover=all
BUILD := build

.PHONY: test clean

test: $(BUILD)/test_usb_keyboard
	./$(BUILD)/test_usb_keyboard

$(BUILD)/test_usb_keyboard: test_usb_keyboard.cpp $(ROOT)/include/keyboard/keyboard.h $(ROOT)/include/rust_api.h
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(ROOT)/include -o $@ test_usb_keyboard.cpp

clean:
	rm -rf $(BUILD)
