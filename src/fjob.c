//  Description:
//
//		fjob.c -- making floppy images in memory, saving them, and copying a
//		whole floppy to and from an image.
//
//		Images are made in memory rather than in a file, so the user can write
//		them straight to a floppy, save them, or both, and leaves nothing behind
//		on the PC by writing one: no temporary folder is involved.
//
//		The floppy is read and written a track at a time: it is what the drive
//		is good at (one revolution rather than eighteen).
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "fjob.h"
#include "fdrive.h"
#include "fimg.h"
#include "fplat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//------------------------------------------------------------------------------
//------------------------ Local macros and constants --------------------------
//------------------------------------------------------------------------------

#define SECTOR_TRIES 3

//------------------------------------------------------------------------------
//------------------------------- Local functions ------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Clear a job's state, leaving a cancel that is already asked for.
  *
  * @param  j The job.
  *
  * @retval None.
  */
static void job_reset( fjob *j )
{
	j->phase = FJOB_IDLE;
	j->done = j->total = 0;
	j->result = FJOB_OK;
	j->nbad = 0;
	j->message[0] = 0;
}

/**
  * @brief  End a job.
  *
  * @param  j The job.
  * @param  rc The result.
  *
  * @retval rc.
  */
static int job_end( fjob *j, int rc )
{
	j->result = rc;
	j->phase = FJOB_FINISHED;
	return rc;
}

/**
  * @brief  Read one track, falling back to single sectors.
  *
  * @param  j The job; unreadable sectors go into bad[].
  * @param  h The drive.
  * @param  sector The track's first sector.
  * @param  count Sectors on the track.
  * @param  buf Receives them; unreadable ones are zeros.
  *
  * @retval None.
  */
static void read_track( fjob *j, fdr_handle *h, int sector, int count, uint8_t *buf )
{
	int s, t;

	if ( fdr_read( h, sector, count, buf ) == 0 )
	{
		return;
	}
	for ( s = 0; s < count; s++ )
	{
		int ok = 0;

		for ( t = 0; t < SECTOR_TRIES && !ok; t++ )
		{
			ok = fdr_read( h, sector + s, 1, buf + s * FIMG_SECTOR ) == 0;
		}
		if ( !ok )
		{
			memset( buf + s * FIMG_SECTOR, 0, FIMG_SECTOR );
			if ( j->nbad < FJOB_MAX_BAD )
			{
				j->bad[j->nbad++] = sector + s;
			}
		}
	}
}

/**
  * @brief  Write one track, falling back to single sectors.
  *
  * @param  h The drive.
  * @param  sector The track's first sector.
  * @param  count Sectors on the track.
  * @param  buf The data.
  * @param  failed Receives the sector that would not write.
  *
  * @retval 0 if written, or an FDR_E_ code.
  */
static int write_track( fdr_handle *h, int sector, int count, const uint8_t *buf, int *failed )
{
	int s, rc;

	rc = fdr_write( h, sector, count, buf );
	if ( rc == 0 || rc == FDR_E_PROTECTED )
	{
		*failed = sector;
		return rc;
	}
	for ( s = 0; s < count; s++ )
	{
		rc = fdr_write( h, sector + s, 1, buf + s * FIMG_SECTOR );
		if ( rc != 0 )
		{
			*failed = sector + s;
			return rc;
		}
	}
	return 0;
}

/**
  * @brief  Say what happened to a bank's sample byte order.
  *
  * @param  swapped Samples swapped.
  * @param  already The bank was in floppy order to begin with.
  * @param  out Receives the note; empty if there is nothing to say.
  * @param  n Room in out.
  *
  * @retval The length written.
  */
static unsigned order_note( int swapped, int already, char *out, unsigned n )
{
	int k = 0;

	if ( already )
	{
		k = snprintf( out, n, " (its samples are in floppy byte order already: it came off a floppy?)" );
	}
	else if ( swapped > 0 )
	{
		k = snprintf( out, n, " (%d sample%s put in floppy byte order)", swapped, swapped == 1 ? "" : "s" );
	}
	else if ( n > 0 )
	{
		out[0] = 0;
	}
	return k < 0 ? 0u : (unsigned)k < n ? (unsigned)k : n - 1;
}

/**
  * @brief  Get room for a set's images, in place of what it held.
  *
  * @param  s The set.
  * @param  disks How many images.
  * @param  msg Receives the reason if there is no memory.
  * @param  n Room in msg.
  *
  * @retval 0 if there is room, -1 if not.
  */
static int set_alloc( fjob_set *s, int disks, char *msg, unsigned n )
{
	fjob_set_free( s );
	s->data = malloc( (size_t)disks * FIMG_SIZE_144 );
	if ( s->data == NULL )
	{
		snprintf( msg, n, "Out of memory." );
		return -1;
	}
	s->disks = disks;
	return 0;
}

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

void fjob_disk_path( const char *path, int disk, int disks, char *out, unsigned n )
{
	const char *base = fplat_base( path );
	const char *dot = strrchr( base, '.' );
	int stem = dot != NULL ? (int)( dot - path ) : (int)strlen( path );

	if ( disks <= 1 )
	{
		snprintf( out, n, "%s", path );
		return;
	}
	snprintf( out, n, "%.*s_%dof%d%s", stem, path, disk, disks, dot != NULL ? dot : "" );
}

int fjob_set_eos( fjob_set *s, const char *bank_path, const char *name, char *msg, unsigned n )
{
	uint8_t *bank = NULL;
	size_t len = 0;
	int disks, d, rc, swapped, already;
	unsigned k;
	char bank_name[FIMG_BANK_NAME_LEN + 1];

	if ( bank_path == NULL )
	{
		if ( set_alloc( s, 1, msg, n ) != 0 )
		{
			return -1;
		}
		fimg_eos_blank( s->data );
		snprintf( msg, n, "A blank EOS floppy." );
		return 1;
	}
	rc = fplat_read_file( bank_path, &bank, &len, (size_t)FIMG_EOS_MAX_DISKS * FIMG_EOS_DISK_BYTES );
	if ( rc != 0 )
	{
		if ( rc == -2 )
		{
			snprintf( msg, n, "%s needs more than %d floppies.", fplat_base( bank_path ), FIMG_EOS_MAX_DISKS );
		}
		else
		{
			snprintf( msg, n, "Cannot read %s.", fplat_base( bank_path ) );
		}
		return -1;
	}
	if ( !fimg_is_bank( bank, len, NULL ) )
	{
		free( bank );
		snprintf( msg, n, "%s is not an EOS bank (FORM E4B0 or E4Br).", fplat_base( bank_path ) );
		return -1;
	}
	if ( name != NULL && name[0] )
	{
		snprintf( bank_name, sizeof bank_name, "%s", name );
	}
	else
	{
		fimg_bank_name( bank_path, bank_name );
	}
	disks = fimg_eos_disks( len );
	if ( set_alloc( s, disks, msg, n ) != 0 )
	{
		free( bank );
		return -1;
	}
	swapped = fimg_bank_to_floppy_order( bank, len, &already );
	for ( d = 1; d <= disks; d++ )
	{
		fimg_eos_disk( s->data + (size_t)( d - 1 ) * FIMG_SIZE_144, bank_name, bank, len, d );
	}
	free( bank );
	k = (unsigned)snprintf( msg, n, "Bank \"%s\" on %d EOS floppy image%s", bank_name, disks, disks == 1 ? "" : "s" );
	if ( k < n )
	{
		order_note( swapped, already, msg + k, n - k );
	}
	return disks;
}

int fjob_set_dos( fjob_set *s, const char *const *paths, int count, const char *label,
                  char *msg, unsigned n )
{
	uint32_t when = fplat_dos_time();
	unsigned k;
	int i, swapped, already;

	if ( set_alloc( s, 1, msg, n ) != 0 )
	{
		return -1;
	}
	fimg_fat_blank( s->data, label );
	k = (unsigned)snprintf( msg, n, "DOS floppy image, %d file%s\n", count, count == 1 ? "" : "s" );
	for ( i = 0; i < count; i++ )
	{
		uint8_t *data = NULL;
		size_t len = 0;
		char stored[13];
		int rc;

		if ( fplat_read_file( paths[i], &data, &len, FIMG_SIZE_144 ) == -1 )
		{
			fjob_set_free( s );
			snprintf( msg, n, "Cannot read %s.", fplat_base( paths[i] ) );
			return -1;
		}
		swapped = already = 0;
		if ( data != NULL && fimg_is_bank( data, len, NULL ) )
		{
			swapped = fimg_bank_to_floppy_order( data, len, &already );
		}
		rc = data != NULL ? fimg_fat_add( s->data, fplat_base( paths[i] ), data, len, when, stored ) : -3;
		free( data );
		if ( rc != 0 )
		{
			fjob_set_free( s );
			if ( rc == -3 )
			{
				snprintf( msg, n, "%s does not fit on the floppy.", fplat_base( paths[i] ) );
			}
			else if ( rc == -2 )
			{
				snprintf( msg, n, "The floppy's directory is full at %s.", fplat_base( paths[i] ) );
			}
			else
			{
				snprintf( msg, n, "%s cannot be added.", fplat_base( paths[i] ) );
			}
			return -1;
		}
		if ( k + 300 < n )
		{
			k += (unsigned)snprintf( msg + k, n - k, "  %s as %s", fplat_base( paths[i] ), stored );
			k += order_note( swapped, already, msg + k, n - k );
			k += (unsigned)snprintf( msg + k, n - k, "\n" );
		}
	}
	return 1;
}

const uint8_t *fjob_set_disk( const fjob_set *s, int disk )
{
	if ( s->data == NULL || disk < 1 || disk > s->disks )
	{
		return NULL;
	}
	return s->data + (size_t)( disk - 1 ) * FIMG_SIZE_144;
}

int fjob_set_save( const fjob_set *s, const char *path, char *msg, unsigned n )
{
	char name[FPLAT_PATH + 16];
	int d;

	// all of them first, so a refusal leaves no part of a set behind
	for ( d = 1; d <= s->disks; d++ )
	{
		fjob_disk_path( path, d, s->disks, name, sizeof name );
		if ( !fplat_write_target_ok( name ) )
		{
			snprintf( msg, n, "Refused: %s is not an ordinary file.", name );
			return -1;
		}
	}
	for ( d = 1; d <= s->disks; d++ )
	{
		fjob_disk_path( path, d, s->disks, name, sizeof name );
		if ( fjob_save( fjob_set_disk( s, d ), FIMG_SIZE_144, name, msg, n ) != 0 )
		{
			return -1;
		}
	}
	fjob_disk_path( path, 1, s->disks, name, sizeof name );
	if ( s->disks == 1 )
	{
		snprintf( msg, n, "Saved %s.", fplat_base( name ) );
	}
	else
	{
		snprintf( msg, n, "Saved %d image files, from %s.", s->disks, fplat_base( name ) );
	}
	return 0;
}

void fjob_set_free( fjob_set *s )
{
	free( s->data );
	s->data = NULL;
	s->disks = 0;
}

void fjob_chs( int sector, int spt, char *out, unsigned n )
{
	int c = sector / ( spt * 2 );
	int rest = sector % ( spt * 2 );

	snprintf( out, n, "%d/%d/%d", c, rest / spt, rest % spt + 1 );
}

int fjob_save( const uint8_t *img, size_t len, const char *path, char *msg, unsigned n )
{
	int rc = fplat_write_file( path, img, len );

	if ( rc == -2 )
	{
		snprintf( msg, n, "Refused: %s is not an ordinary file.", path );
	}
	else if ( rc != 0 )
	{
		snprintf( msg, n, "Cannot write %s.", path );
	}
	else
	{
		snprintf( msg, n, "Saved %s.", fplat_base( path ) );
	}
	return rc != 0 ? -1 : 0;
}

int fjob_read( fjob *j, const char *drive, const char *out_path )
{
	uint8_t *img = NULL;
	size_t len = 0;
	int rc;

	job_reset( j );
	// before the drive, not after a read that can take minutes
	if ( !fplat_write_target_ok( out_path ) )
	{
		snprintf( j->message, sizeof j->message, "Refused: %s is not an ordinary file.", out_path );
		return job_end( j, FJOB_E_OUTPUT );
	}
	rc = fjob_read_mem( j, drive, &img, &len );
	if ( rc != FJOB_OK )
	{
		return rc;
	}
	rc = fjob_save( img, len, out_path, j->message, sizeof j->message );
	free( img );
	if ( rc != 0 )
	{
		return job_end( j, FJOB_E_OUTPUT );
	}
	if ( j->nbad )
	{
		snprintf( j->message, sizeof j->message,
		          "Saved %s with %d unreadable sector(s), zeros in the image.",
		          fplat_base( out_path ), j->nbad );
	}
	else
	{
		snprintf( j->message, sizeof j->message, "Saved %s: all %d sectors read.",
		          fplat_base( out_path ), j->total );
	}
	return job_end( j, FJOB_OK );
}

int fjob_read_mem( fjob *j, const char *drive, uint8_t **img_out, size_t *len )
{
	fdr_handle *h;
	uint8_t *img;
	int rc, s, sectors, spt;
	char err[256] = "";

	job_reset( j );
	*img_out = NULL;
	*len = 0;
	rc = fdr_open( drive, 0, &h, err, sizeof err );
	if ( rc != 0 )
	{
		snprintf( j->message, sizeof j->message, "%s", err );
		return job_end( j, FJOB_E_DRIVE );
	}
	sectors = fdr_sectors( h );
	spt = fdr_track_sectors( h );
	img = malloc( (size_t)sectors * FIMG_SECTOR );
	if ( img == NULL )
	{
		fdr_close( h );
		snprintf( j->message, sizeof j->message, "Out of memory." );
		return job_end( j, FJOB_E_OUTPUT );
	}
	j->total = sectors;
	j->phase = FJOB_READING;
	for ( s = 0; s < sectors && !j->cancel; s += spt )
	{
		read_track( j, h, s, spt, img + (size_t)s * FIMG_SECTOR );
		j->done = s + spt;
	}
	fdr_close( h );
	if ( j->cancel )
	{
		free( img );
		snprintf( j->message, sizeof j->message, "Cancelled: nothing was kept of this read." );
		return job_end( j, FJOB_E_CANCELLED );
	}
	*img_out = img;
	*len = (size_t)sectors * FIMG_SECTOR;
	if ( j->nbad )
	{
		snprintf( j->message, sizeof j->message,
		          "Read, with %d unreadable sector(s): zeros in the image.", j->nbad );
	}
	else
	{
		snprintf( j->message, sizeof j->message, "Read: all %d sectors.", sectors );
	}
	return job_end( j, FJOB_OK );
}

int fjob_write( fjob *j, const char *drive, const char *image_path )
{
	uint8_t *img = NULL;
	size_t len = 0;
	int rc;

	job_reset( j );
	rc = fplat_read_file( image_path, &img, &len, FIMG_SIZE_144 );
	if ( rc != 0 )
	{
		snprintf( j->message, sizeof j->message,
		          "%s is not a floppy image (1474560 or 737280 bytes).", fplat_base( image_path ) );
		return job_end( j, FJOB_E_IMAGE );
	}
	rc = fjob_write_mem( j, drive, img, len );
	free( img );
	return rc;
}

int fjob_write_mem( fjob *j, const char *drive, const uint8_t *img, size_t len )
{
	fdr_handle *h;
	uint8_t *back;
	int rc, s, sectors, spt, failed = 0;
	char err[256] = "";

	job_reset( j );
	if ( len != FIMG_SIZE_144 && len != FIMG_SIZE_720 )
	{
		snprintf( j->message, sizeof j->message, "That is not a floppy image (1474560 or 737280 bytes)." );
		return job_end( j, FJOB_E_IMAGE );
	}
	rc = fdr_open( drive, 1, &h, err, sizeof err );
	if ( rc != 0 )
	{
		snprintf( j->message, sizeof j->message, "%s", err );
		return job_end( j, FJOB_E_DRIVE );
	}
	sectors = fdr_sectors( h );
	spt = fdr_track_sectors( h );
	if ( (size_t)sectors * FIMG_SECTOR != len )
	{
		fdr_close( h );
		snprintf( j->message, sizeof j->message,
		          "The image is %u KB but the floppy is %d KB. Nothing was written.",
		          (unsigned)( len / 1024 ), sectors / 2 );
		return job_end( j, FJOB_E_IMAGE );
	}
	back = malloc( (size_t)spt * FIMG_SECTOR );
	if ( back == NULL )
	{
		fdr_close( h );
		snprintf( j->message, sizeof j->message, "Out of memory." );
		return job_end( j, FJOB_E_IMAGE );
	}

	j->total = sectors;
	j->phase = FJOB_WRITING;
	for ( s = 0; s < sectors && !j->cancel; s += spt )
	{
		rc = write_track( h, s, spt, img + (size_t)s * FIMG_SECTOR, &failed );
		if ( rc != 0 )
		{
			char chs[40];

			fjob_chs( failed, spt, chs, sizeof chs );
			if ( rc == FDR_E_PROTECTED )
			{
				snprintf( j->message, sizeof j->message, "The floppy is write protected." );
			}
			else
			{
				snprintf( j->message, sizeof j->message,
				          "Cannot write sector %d (C/H/S %s). The floppy is incomplete.", failed, chs );
			}
			fdr_close( h );
			free( back );
			return job_end( j, FJOB_E_WRITE );
		}
		j->done = s + spt;
	}
	if ( j->cancel )
	{
		fdr_close( h );
		free( back );
		snprintf( j->message, sizeof j->message, "Cancelled: the floppy is incomplete." );
		return job_end( j, FJOB_E_CANCELLED );
	}
	fdr_flush( h );

	j->done = 0;
	j->phase = FJOB_VERIFYING;
	for ( s = 0; s < sectors && !j->cancel; s += spt )
	{
		int k;
		const uint8_t *want = img + (size_t)s * FIMG_SECTOR;

		if ( fdr_read( h, s, spt, back ) == 0 && memcmp( back, want, (size_t)spt * FIMG_SECTOR ) == 0 )
		{
			j->done = s + spt;
			continue;
		}
		for ( k = 0; k < spt; k++ )
		{
			if ( ( fdr_read( h, s + k, 1, back ) != 0 ||
			       memcmp( back, want + k * FIMG_SECTOR, FIMG_SECTOR ) != 0 ) &&
			     j->nbad < FJOB_MAX_BAD )
			{
				j->bad[j->nbad++] = s + k;
			}
		}
		j->done = s + spt;
	}
	fdr_close( h );
	free( back );
	if ( j->cancel )
	{
		snprintf( j->message, sizeof j->message, "Written, but the verify was cancelled." );
		return job_end( j, FJOB_E_CANCELLED );
	}
	if ( j->nbad )
	{
		snprintf( j->message, sizeof j->message,
		          "Verify failed: %d sector(s) differ from the image. Do not trust this floppy.",
		          j->nbad );
		return job_end( j, FJOB_E_VERIFY );
	}
	snprintf( j->message, sizeof j->message, "Written and verified: all %d sectors read back the same.",
	          sectors );
	return job_end( j, FJOB_OK );
}
