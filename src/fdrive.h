//  Description:
//
//		fdrive.h -- raw access to a real floppy drive, and nothing but a floppy
//		drive. fdrive.c explains how other drives are kept out.
//

#ifndef FDRIVE_H
#define FDRIVE_H

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

//------------------------------------------------------------------------------
//--------------------------- Macros and constants -----------------------------
//------------------------------------------------------------------------------

#define FDR_MAX_DRIVES  8

// Error codes of fdr_open and of the transfers.
#define FDR_E_NOTFLOPPY   -1
#define FDR_E_NOMEDIA     -2
#define FDR_E_BADMEDIA    -3
#define FDR_E_PROTECTED   -4
#define FDR_E_OPEN        -5
#define FDR_E_IO          -6
#define FDR_E_BUSY        -7
#define FDR_E_UNSUPPORTED -8

//------------------------------------------------------------------------------
//-------------------------------- Public types --------------------------------
//------------------------------------------------------------------------------

typedef struct
{
	char id[32];     // what fdr_open takes: "A:", "/dev/fd0"
	char label[64];  // what the user sees
} fdr_drive;

typedef struct fdr_handle fdr_handle;

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Whether this build can reach a real floppy drive at all.
  *
  * @retval 1 Windows and Linux, 0 elsewhere (image making still works there).
  */
int fdr_supported( void );

/**
  * @brief  List the floppy drives. Nothing that is not one is ever listed.
  *
  * Touches no media, so a listing does not spin the drive up.
  *
  * @param  out Receives the drives, room for FDR_MAX_DRIVES.
  *
  * @retval The count.
  */
int fdr_list( fdr_drive *out );

/**
  * @brief  Open a drive, after every check that it is a floppy drive.
  *
  * @param  id A drive's id from fdr_list.
  * @param  write 1 to write. Otherwise the drive is opened read-only.
  * @param  out Receives the handle.
  * @param  err Receives what went wrong.
  * @param  errn Room in err.
  *
  * @retval 0 if open, or an FDR_E_ code.
  */
int fdr_open( const char *id, int write, fdr_handle **out, char *err, size_t errn );

/**
  * @brief  Sectors on the floppy in the drive.
  *
  * @param  h The drive.
  *
  * @retval 2880 or 1440.
  */
int fdr_sectors( const fdr_handle *h );

/**
  * @brief  Sectors on one track of the floppy.
  *
  * @param  h The drive.
  *
  * @retval 18 or 9.
  */
int fdr_track_sectors( const fdr_handle *h );

/**
  * @brief  Read sectors.
  *
  * @param  h The drive.
  * @param  sector First sector.
  * @param  count How many, one track at most.
  * @param  buf Receives them.
  *
  * @retval 0 if read, FDR_E_IO if not.
  */
int fdr_read( fdr_handle *h, int sector, int count, uint8_t *buf );

/**
  * @brief  Write sectors. Refused on a handle opened read-only.
  *
  * @param  h The drive.
  * @param  sector First sector.
  * @param  count How many, one track at most.
  * @param  buf The data.
  *
  * @retval 0 if written, FDR_E_IO or FDR_E_PROTECTED if not.
  */
int fdr_write( fdr_handle *h, int sector, int count, const uint8_t *buf );

/**
  * @brief  Push writes out to the floppy and drop anything cached.
  *
  * @param  h The drive.
  *
  * @retval 0 if done, FDR_E_IO if not.
  */
int fdr_flush( fdr_handle *h );

/**
  * @brief  Close a drive, unlocking it.
  *
  * @param  h The drive, may be NULL.
  *
  * @retval None.
  */
void fdr_close( fdr_handle *h );

#ifdef FDR_TEST_DRIVE
// Only in the test builds (make test, make testgui): the program itself lists
// the PC's floppy drives and nothing else.
#define FDR_FAKE_ID "fake"

/**
  * @brief  Add a stand-in drive backed by an ordinary file, for tests.
  *
  * Only an ordinary file of exactly a floppy's size is taken, never a device.
  *
  * @param  path The file.
  * @param  bad Sectors that fail to read and to write, may be NULL.
  * @param  nbad How many.
  * @param  flip Sectors written wrongly without an error, for the verify; may
  *         be NULL.
  * @param  nflip How many.
  *
  * @retval 0 if taken, -1 if not.
  */
int fdr_fake_set( const char *path, const int *bad, int nbad, const int *flip, int nflip );
#endif

#endif
