//  Description:
//
//		fplat.h -- the few file-system calls that differ between the systems.
//		Paths are UTF-8 everywhere, as SDL's drop events give them.
//

#ifndef FPLAT_H
#define FPLAT_H

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

//------------------------------------------------------------------------------
//--------------------------- Macros and constants -----------------------------
//------------------------------------------------------------------------------

#define FPLAT_PATH 1024

#ifdef _WIN32
#define FPLAT_SEP '\\'
#else
#define FPLAT_SEP '/'
#endif

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  fopen with a UTF-8 path.
  *
  * @param  path The file.
  * @param  mode As for fopen.
  *
  * @retval The stream, or NULL.
  */
FILE *fplat_fopen( const char *path, const char *mode );

/**
  * @brief  Read a whole file into a new buffer.
  *
  * @param  path The file.
  * @param  data Receives the buffer, which the caller frees.
  * @param  len Receives the size.
  * @param  max Refuse a larger file.
  *
  * @retval 0 if read, -1 cannot read it, -2 larger than max, -3 out of memory.
  */
int fplat_read_file( const char *path, uint8_t **data, size_t *len, size_t max );

/**
  * @brief  May this path be saved to? It must not name a device or a folder.
  *
  * This only looks: it opens nothing, so it is safe on any path, and it lets a
  * long job fail before it starts. fplat_write_file checks again for itself.
  *
  * @param  path The path.
  *
  * @retval 1 if it is an ordinary file or does not exist yet, 0 if not.
  */
int fplat_write_target_ok( const char *path );

/**
  * @brief  Write a whole file, replacing what it held. Only ever an ordinary file.
  *
  * @param  path The file.
  * @param  data The bytes.
  * @param  len How many.
  *
  * @retval 0 if written, -1 cannot write it, -2 refused: a device or a folder.
  */
int fplat_write_file( const char *path, const void *data, size_t len );

/**
  * @brief  Check that a path names an ordinary file, not a device or folder.
  *
  * @param  path The path.
  * @param  size Receives the size, may be NULL.
  *
  * @retval 1 if it does, 0 if not.
  */
int fplat_is_regular( const char *path, uint64_t *size );

/**
  * @brief  Check for a folder.
  *
  * @param  path The path.
  *
  * @retval 1 if it is one, 0 if not.
  */
int fplat_is_dir( const char *path );

/**
  * @brief  The name part of a path.
  *
  * @param  path The path.
  *
  * @retval A pointer into path.
  */
const char *fplat_base( const char *path );

/**
  * @brief  The absolute form of a path.
  *
  * @param  path The path.
  * @param  out Receives it.
  * @param  n Room in out.
  *
  * @retval 0 if made, -1 if not.
  */
int fplat_full_path( const char *path, char *out, size_t n );

/**
  * @brief  Call a function for each entry of a folder.
  *
  * @param  dir The folder.
  * @param  fn Gets each entry's name, whether it is a folder, and ctx.
  * @param  ctx Passed on.
  *
  * @retval 0 if listed, -1 if not.
  */
int fplat_list_dir( const char *dir, void ( *fn )( const char *, int, void * ), void *ctx );

/**
  * @brief  Compare names ignoring ASCII case.
  *
  * @param  a One name.
  * @param  b The other.
  *
  * @retval As strcmp.
  */
int fplat_stricmp( const char *a, const char *b );

/**
  * @brief  Now, in a FAT directory entry's format.
  *
  * @retval The date in the high half, the time in the low half.
  */
uint32_t fplat_dos_time( void );

#endif
