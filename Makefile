CC ?= cc
CXX ?= c++
CFLAGS ?= -O2 -g
CXXFLAGS ?= -O2 -g
OPENVR_SDK ?= ../openvr
STEAMVR_LIBDIR ?= /opt/steamvr/bin/linuxarm64
WARNINGS = -Wall -Wextra -Wpedantic

.PHONY: all
all: tools/frametap tools/unpack_raw10 tools/eyegaze tools/eyetap

tools/frametap: tools/frametap.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ $< $(LDLIBS)

tools/unpack_raw10: tools/unpack_raw10.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ $< $(LDLIBS)

tools/eyetap: tools/eyetap.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ $< $(LDLIBS)

tools/eyegaze: tools/eyegaze.cpp $(OPENVR_SDK)/headers/openvr.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(WARNINGS) -std=c++17 -pthread -I"$(OPENVR_SDK)/headers" $(LDFLAGS) -o $@ $< -L"$(STEAMVR_LIBDIR)" -Wl,-rpath,"$(STEAMVR_LIBDIR)" -lopenvr_api $(LDLIBS)
