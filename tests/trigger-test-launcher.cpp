#include <windows.h>
#include <cassert>
// Reserve the original game image range at process creation, before the CRT,
// Windows heaps and SDL allocate low addresses. RunTests replaces this dummy
// host image with the original game's bytes and exits without returning here.
volatile char reserved_game_image[0xe00000];
int main(int argc,char**argv){assert(argc==2);reserved_game_image[sizeof(reserved_game_image)-1]=1;auto dll=LoadLibraryA("independent-test.dll");assert(dll);auto run=reinterpret_cast<int(*)(const char*)>(GetProcAddress(dll,"RunTests"));assert(run);run(argv[1]);return 3;}
