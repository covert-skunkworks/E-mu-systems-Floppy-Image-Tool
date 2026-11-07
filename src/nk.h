//  Description:
//
//		nk.h -- Nuklear (third_party/nuklear, 4.9.4) as this program uses it.
//		Nuklear's options change its structures, so every file that uses it
//		includes this header rather than nuklear.h; nk.c holds the
//		implementation with the SDL_Renderer back-end from Nuklear's demos.
//

#ifndef NK_H
#define NK_H

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_SDL_RENDERER_SDL_H <SDL2/SDL.h>
// main.c keeps its own main: SDL's renaming is not needed and needs SDL2main.
#define SDL_MAIN_HANDLED

#include <SDL2/SDL.h>
#include "nuklear.h"
#include "demo/sdl_renderer/nuklear_sdl_renderer.h"

#endif
