// Writes a fixed payload to stdout with embedded newlines and NO trailing
// newline, in binary mode. Used by the byte-exact streaming regression: a
// relay that captures-and-rewrites would add or alter bytes; a true stream
// reproduces the payload exactly.
#include <cstdio>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

int main() {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY); // no \n -> \r\n translation
#endif
    static const char payload[] = "one\ntwo\nno-trailing-newline";
    std::fwrite(payload, 1, sizeof(payload) - 1, stdout); // exclude NUL
    std::fflush(stdout);
    return 0;
}
