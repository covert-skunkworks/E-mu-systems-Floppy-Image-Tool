//  Description:
//
//		fjob.h -- the program's work, without its window: making floppy images
//		in memory, saving them, and copying a whole floppy to and from an image.
//		The GUI runs the transfers on a thread of their own and watches the
//		progress fields; the command line and the tests call them directly.
//

#ifndef FJOB_H
#define FJOB_H

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

//------------------------------------------------------------------------------
//--------------------------- Macros and constants -----------------------------
//------------------------------------------------------------------------------

#define FJOB_MAX_BAD 2880

#define FJOB_OK          0
#define FJOB_E_DRIVE     -1  // the drive would not open: see message
#define FJOB_E_IMAGE     -2  // the image cannot be used: see message
#define FJOB_E_WRITE     -3  // a sector could not be written
#define FJOB_E_VERIFY    -4  // read-back differs: bad[] lists the sectors
#define FJOB_E_CANCELLED -5
#define FJOB_E_OUTPUT    -6  // the output file cannot be written

//------------------------------------------------------------------------------
//-------------------------------- Public types --------------------------------
//------------------------------------------------------------------------------

typedef enum
{
	FJOB_IDLE = 0,
	FJOB_READING,
	FJOB_WRITING,
	FJOB_VERIFYING,
	FJOB_FINISHED
} fjob_phase;

typedef struct
{
	volatile int phase;     // fjob_phase
	volatile int done;      // sectors done in this phase
	volatile int total;     // sectors in this phase
	volatile int cancel;    // set from outside to stop between tracks
	int  result;            // FJOB_OK or FJOB_E_ after FJOB_FINISHED
	int  bad[FJOB_MAX_BAD]; // sectors unreadable (read), or differing (write)
	int  nbad;
	char message[512];      // the outcome, one line for the user
} fjob;

// Floppy images made and kept in memory, one or (for a spanning bank) several.
// Nothing reaches a disk until the user writes them to a floppy or saves them.
typedef struct
{
	uint8_t *data;  // disks * FIMG_SIZE_144 bytes
	int      disks;
} fjob_set;

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Make a bank's images in EOS's own format, one per floppy.
  *
  * @param  s Receives the images, replacing what it held. Start from zeros.
  * @param  bank_path The bank, or NULL for a blank floppy.
  * @param  name Bank name, NULL or "" to take it from the file's name.
  * @param  msg Receives the outcome, for the user.
  * @param  n Room in msg.
  *
  * @retval The number of images made, or -1 with the reason in msg.
  */
int fjob_set_eos( fjob_set *s, const char *bank_path, const char *name, char *msg, unsigned n );

/**
  * @brief  Make a DOS floppy image holding some files.
  *
  * @param  s Receives the image, replacing what it held. Start from zeros.
  * @param  paths The files.
  * @param  count How many; 0 makes a blank floppy.
  * @param  label Volume label, may be NULL.
  * @param  msg Receives the outcome, for the user: what each file is called on
  *         the floppy, one a line.
  * @param  n Room in msg.
  *
  * @retval 1, or -1 with the reason in msg.
  */
int fjob_set_dos( fjob_set *s, const char *const *paths, int count, const char *label,
                  char *msg, unsigned n );

/**
  * @brief  One image of a set.
  *
  * @param  s The set.
  * @param  disk Which, from 1.
  *
  * @retval The image, FIMG_SIZE_144 bytes, or NULL if there is no such disk.
  */
const uint8_t *fjob_set_disk( const fjob_set *s, int disk );

/**
  * @brief  Save a set to image files; several disks get numbered names.
  *
  * @param  s The set.
  * @param  path The file; "x.img" gives "x_1of3.img" and so on for a set.
  * @param  msg Receives the outcome, for the user.
  * @param  n Room in msg.
  *
  * @retval 0 if saved, or -1 with the reason in msg.
  */
int fjob_set_save( const fjob_set *s, const char *path, char *msg, unsigned n );

/**
  * @brief  Free a set's images.
  *
  * @param  s The set; left empty.
  *
  * @retval None.
  */
void fjob_set_free( fjob_set *s );

/**
  * @brief  Read a whole floppy into an image file.
  *
  * The drive is opened read-only. A track that fails is read again sector by
  * sector, with retries; a sector that stays unreadable is zeros in the image
  * and is listed in bad[]. The image is still written, so what can be saved of
  * a failing floppy is saved.
  *
  * @param  j Progress and result. Cleared here, except cancel.
  * @param  drive A drive's id.
  * @param  out_path The image to write.
  *
  * @retval FJOB_OK, also with bad sectors (see nbad), or an FJOB_E_ code.
  */
int fjob_read( fjob *j, const char *drive, const char *out_path );

/**
  * @brief  Write an image file to a floppy and read it all back.
  *
  * @param  j Progress and result. Cleared here, except cancel.
  * @param  drive A drive's id.
  * @param  image_path The image to write.
  *
  * @retval FJOB_OK if every sector read back the same, or an FJOB_E_ code.
  */
int fjob_write( fjob *j, const char *drive, const char *image_path );

/**
  * @brief  Write an image in memory to a floppy and read it all back.
  *
  * The image's size must be the size of the floppy in the drive.
  *
  * @param  j Progress and result. Cleared here, except cancel.
  * @param  drive A drive's id.
  * @param  img The image.
  * @param  len Its size.
  *
  * @retval FJOB_OK if every sector read back the same, or an FJOB_E_ code.
  */
int fjob_write_mem( fjob *j, const char *drive, const uint8_t *img, size_t len );

/**
  * @brief  The file name of one disk of a set: "x.img" gives "x_2of3.img".
  *
  * @param  path The name asked for; used as it is for a single disk.
  * @param  disk This disk, from 1.
  * @param  disks Disks in the set.
  * @param  out Receives the name.
  * @param  n Room in out.
  *
  * @retval None.
  */
void fjob_disk_path( const char *path, int disk, int disks, char *out, unsigned n );

/**
  * @brief  Tell a sector's position on the floppy.
  *
  * @param  sector The sector.
  * @param  spt Sectors a track.
  * @param  out Receives "C/H/S" with the sector numbered from 1, as a PC does.
  * @param  n Room in out.
  *
  * @retval None.
  */
void fjob_chs( int sector, int spt, char *out, unsigned n );

#endif
