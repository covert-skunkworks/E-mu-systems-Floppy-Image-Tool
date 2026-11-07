//  Description:
//
//		fimg.h -- floppy images for E-mu samplers running EOS: making them and
//		telling what an image holds. No drive access and no GUI here, so this
//		module is what the unit tests exercise.
//
//		See fimg.c for the on-disk layouts and how they were established.
//

#ifndef FIMG_H
#define FIMG_H

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

//------------------------------------------------------------------------------
//--------------------------- Macros and constants -----------------------------
//------------------------------------------------------------------------------

#define FIMG_SECTOR       512
#define FIMG_SECTORS_144  2880
#define FIMG_SIZE_144     ( FIMG_SECTORS_144 * FIMG_SECTOR )
#define FIMG_SECTORS_720  1440
#define FIMG_SIZE_720     ( FIMG_SECTORS_720 * FIMG_SECTOR )

// An EOS floppy keeps sector 0 for its header; the bank fills all the others.
#define FIMG_EOS_DISK_BYTES  ( ( FIMG_SECTORS_144 - 1 ) * FIMG_SECTOR )
// EOS's prompt prints the disk numbers with %2d.
#define FIMG_EOS_MAX_DISKS   99
#define FIMG_BANK_NAME_LEN   16

// Data sectors of the FAT layout below (1 sector per cluster).
#define FIMG_FAT_DATA_SECTORS 2847
#define FIMG_FAT_ROOT_ENTRIES 224
#define FIMG_LABEL_LEN        11

// Rows of a listing (fimg_list): far more than a floppy's list should need. The
// last row says so if a list was cut.
#define FIMG_LIST_MAX         4096

//------------------------------------------------------------------------------
//-------------------------------- Public types --------------------------------
//------------------------------------------------------------------------------

typedef enum
{
	FIMG_KIND_UNKNOWN = 0,
	FIMG_KIND_EOS_BLANK,   // EOS's own format, nothing saved on it yet
	FIMG_KIND_EOS_BANK,    // EOS's own format, one disk of a bank
	FIMG_KIND_FAT12,       // DOS floppy
	FIMG_KIND_UPDATE       // boot loader's OS update floppy
} fimg_kind;

typedef struct
{
	fimg_kind kind;
	char      name[FIMG_BANK_NAME_LEN + 1];  // EOS bank name, or FAT label
	char      form[5];    // EOS: "E4B0" / "E4Br"
	int       disk;       // EOS: this disk, from 1
	int       disks;      // EOS: disks in the set
	uint32_t  bank_bytes; // EOS: size of the whole bank
	int       files;      // FAT: files in the root
	uint32_t  free_bytes; // FAT: free space
	char      text[1024]; // what to show the user, several lines
} fimg_info;

// One row of what an image holds.
typedef struct
{
	char     name[32];  // a file's name, or "P000 Name" for what is in a bank
	char     what[64];  // what it is
	uint32_t size;      // in bytes; 0 when there is no size to show
	int      inside;    // 1 if it is in the bank of the row above it
} fimg_item;

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Check that a buffer is a bank EOS can load.
  *
  * @param  data The file.
  * @param  len Its size.
  * @param  form Receives the form type and a terminator, may be NULL.
  *
  * @retval 1 if it is a FORM E4B0 / E4Br file, 0 if not.
  */
int fimg_is_bank( const uint8_t *data, size_t len, char form[5] );

/**
  * @brief  The bank name a file name stands for.
  *
  * Drops the folders, the extension and the "B.NNN-" prefix the disk manager
  * and FAT disks use, and cuts the rest to EOS's 16 characters.
  *
  * @param  path A file path or name.
  * @param  out Receives the name.
  *
  * @retval None.
  */
void fimg_bank_name( const char *path, char out[FIMG_BANK_NAME_LEN + 1] );

/**
  * @brief  Put a bank's samples in the byte order floppies hold them in.
  *
  * Both floppy formats want this, see fimg.c: a bank file from a hard disk has
  * little-endian samples, and on a floppy EOS has them big-endian. A bank whose
  * samples already read as big-endian (it came off a floppy) is left as it is.
  *
  * @param  bank The bank file, changed in place.
  * @param  len Its size.
  * @param  already Receives 1 if it was left as it is, may be NULL.
  *
  * @retval How many samples were swapped; 0 for a bank with no samples.
  */
int fimg_bank_to_floppy_order( uint8_t *bank, size_t len, int *already );

/**
  * @brief  Make an image of a floppy EOS has formatted and not yet written.
  *
  * EOS's Format writes tracks only: every sector holds the fill byte 55.
  *
  * @param  img Receives FIMG_SIZE_144 bytes.
  *
  * @retval None.
  */
void fimg_eos_blank( uint8_t *img );

/**
  * @brief  How many floppies a bank takes in EOS's own format.
  *
  * @param  bank_len Size of the bank file.
  *
  * @retval The count, or -1 if the bank is empty or needs too many disks.
  */
int fimg_eos_disks( size_t bank_len );

/**
  * @brief  Make the image of one disk of a bank in EOS's own format.
  *
  * @param  img Receives FIMG_SIZE_144 bytes.
  * @param  name Bank name, cut to 16 characters.
  * @param  bank The bank, in floppy byte order (fimg_bank_to_floppy_order).
  * @param  bank_len Its size.
  * @param  disk Which disk, from 1.
  *
  * @retval 0 if made, -1 not a bank, -2 no such disk in the set.
  */
int fimg_eos_disk( uint8_t *img, const char *name, const uint8_t *bank,
                   size_t bank_len, int disk );

/**
  * @brief  Make an empty DOS floppy as the E4 Ultra's FAT format lays it out.
  *
  * @param  img Receives FIMG_SIZE_144 bytes.
  * @param  label Volume label, EOS shows it as the folder's name; NULL or ""
  *         gives "E-MU".
  *
  * @retval None.
  */
void fimg_fat_blank( uint8_t *img, const char *label );

/**
  * @brief  The 8.3 name a file takes on a DOS floppy.
  *
  * @param  name A file name, without folders.
  * @param  out Receives eleven characters, the name then the extension, both
  *         space padded, as the directory entry holds them.
  *
  * @retval None.
  */
void fimg_short_name( const char *name, uint8_t out[11] );

/**
  * @brief  Add a file to the root of a DOS floppy image.
  *
  * The name goes in as a plain 8.3 entry: EOS before 4.7 does not show long
  * names (see fimg.c). EOS numbers the banks in the order they are added.
  *
  * @param  img The image.
  * @param  name The file's name, without folders.
  * @param  data The file.
  * @param  len Its size.
  * @param  dos_time Time and date in the directory entry's format, date in the
  *         high half; 0 leaves them empty.
  * @param  stored Receives the name it got as NAME.EXT, which a clash with an
  *         earlier file changes to NAME~1.EXT; may be NULL.
  *
  * @retval 0 if added, -1 not such an image, -2 no room in the directory,
  *         -3 the disk is full, -4 no free 8.3 name.
  */
int fimg_fat_add( uint8_t *img, const char *name, const uint8_t *data,
                  size_t len, uint32_t dos_time, char stored[13] );

/**
  * @brief  Read a file back from a DOS floppy image, for the tests.
  *
  * @param  img The image.
  * @param  index Which file of the root, from 0.
  * @param  out Receives the file, room for len bytes.
  * @param  len The file's size, as fimg_fat_entry gives it.
  *
  * @retval 0 if read, -1 no such file or a broken chain.
  */
int fimg_fat_read( const uint8_t *img, int index, uint8_t *out, size_t len );

/**
  * @brief  One file of the root of a DOS floppy image.
  *
  * @param  img The image.
  * @param  index Which file, from 0.
  * @param  name Receives the name as NAME.EXT.
  * @param  size Receives the size.
  *
  * @retval 0 if there is such a file, -1 if not.
  */
int fimg_fat_entry( const uint8_t *img, int index, char name[13], uint32_t *size );

/**
  * @brief  Sectors a file takes on a DOS floppy.
  *
  * @param  file_len The file's size.
  *
  * @retval The sectors; an empty file takes none.
  */
uint32_t fimg_fat_sectors( size_t file_len );

/**
  * @brief  Tell what an image holds.
  *
  * @param  img The image.
  * @param  size Its size.
  * @param  out Receives the result.
  *
  * @retval None.
  */
void fimg_inspect( const uint8_t *img, size_t size, fimg_info *out );

/**
  * @brief  List what an image holds: its files, and the presets, samples and
  *         sequences of its banks.
  *
  * The image may have come off a failing floppy, so nothing in it is trusted:
  * a table of contents that makes no sense gives a shorter list, never a read
  * outside the image. A bank's table of contents is at its start, so of a bank
  * on several floppies only disk 1 can list it.
  *
  * @param  img The image.
  * @param  size Its size.
  * @param  out Receives the rows.
  * @param  room Rows out can take; FIMG_LIST_MAX is plenty.
  *
  * @retval The number of rows; 0 if there is nothing to list.
  */
int fimg_list( const uint8_t *img, size_t size, fimg_item *out, int room );

/**
  * @brief  The file name to suggest for an image: the bank's name or the
  *         floppy's label, with what a file name cannot hold replaced.
  *
  * @param  info What fimg_inspect said about the image.
  * @param  out Receives the name, like "Bank_2of3.img", with no folder.
  * @param  n Room in out.
  *
  * @retval None.
  */
void fimg_image_name( const fimg_info *info, char *out, size_t n );

#endif
