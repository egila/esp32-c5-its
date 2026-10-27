# https://github.com/Technica-Engineering/vector_blf
VECTOR_BLF ?= ../../../../extern/Technica-Engineering_vector-blf/usr/local
# https://github.com/fpcap/fpcap
FPCAP ?= ../../../../extern/fpcap_fpcap

CXXFLAGS ?= -O2 -Wall

SRC = log-cvt.cpp blf.cpp pcap.cpp
INC = log-cvt.h ../../esp32-c5-its-rx/include/log-data.h

# -D_FILE_OFFSET_BITS=64: open logs over 2 GiB on 32-bit hosts too
log-cvt: $(SRC) $(INC)
	$(CXX) -std=c++20 -D_FILE_OFFSET_BITS=64 $(CPPFLAGS) $(CXXFLAGS) -o log-cvt $(SRC) -I ../../esp32-c5-its-rx/include -I $(VECTOR_BLF)/include -I $(FPCAP)/include $(LDFLAGS) -L $(VECTOR_BLF)/lib -L $(FPCAP)/build -L $(FPCAP)/build/_deps/zstd-build/lib -Wl,-rpath,$(abspath $(VECTOR_BLF)/lib) -lVector_BLF -lfpcap -lzstd

clean:
	rm -f log-cvt

.PHONY: clean
