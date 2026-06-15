// Kitty-graphics build of taskman: same program as taskman.c, but with the
// animated pixel backdrop enabled (cells still used for the UI, and for
// terminals without graphics support). See taskman.c for everything.
// SPDX-License-Identifier: 0BSD
#define TUI_BUILD_GFX 1
#include "taskman.c"
