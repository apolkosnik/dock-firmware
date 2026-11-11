# Host-side test of the GunCon 3 decoder: make -C tests/guncon3
ROOT := ../..
CXX ?= g++
# -Wno-cpp: rust_api.h includes <cstdbool>, which newer libstdc++ flags as deprecated.
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Werror -Wno-cpp -fsanitize=address,undefined -fno-sanitize-recover=all
SEED ?= 12345
COUNT ?= 20000
BUILD := build

.PHONY: test clean

test: $(BUILD)/test_guncon3 $(BUILD)/vectors.txt
	./$(BUILD)/test_guncon3 $(BUILD)/vectors.txt

$(BUILD)/test_guncon3: test_guncon3.cpp $(ROOT)/src/usb_host/guncon3_protocol.cpp $(ROOT)/include/usb_host/guncon3_protocol.h $(ROOT)/include/rust_api.h
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(ROOT)/include -o $@ test_guncon3.cpp $(ROOT)/src/usb_host/guncon3_protocol.cpp

$(BUILD)/vectors.txt: gen_vectors.py
	mkdir -p $(BUILD)
	python3 -I gen_vectors.py $(SEED) $(COUNT) > $@

clean:
	rm -rf $(BUILD)
