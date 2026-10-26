// stb_vorbis 的实现单独放一个编译单元:
// 它的 .c 里有不少短宏(FAST_FLOAT / LEEK 之类),和 windows.h 混在一个 TU 里会撞车
// (实测报 winnt.h 语法错误)。这里只 include 实现,不引入任何 Windows 头。
#include "../third_party/stb_vorbis.c"
