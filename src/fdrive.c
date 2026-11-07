//  Description:
//
//		fdrive.c -- raw access to a real floppy drive, and nothing but a floppy
//		drive.
//
//		A wrong target here destroys someone's disk, so the checks are layered
//		and all of them have to pass. fdr_list and fdr_open use the same test,
//		so a drive that was never listed cannot be opened by naming it either.
//
//		Windows:
//		  - the name must be one drive letter and a colon;
//		  - GetDriveType must say DRIVE_REMOVABLE;
//		  - QueryDosDevice must map it to \Device\Floppy<n>. USB sticks map to
//		    \Device\HarddiskVolume<n> and memory cards likewise, whatever their
//		    drive letter; USB floppy drives run on sfloppy.sys and get the
//		    floppy name too;
//		  - on open, the geometry must report a 3.5" floppy media type with 512
//		    byte sectors, 2 heads, 80 cylinders and 18 or 9 sectors a track.
//		Writing also locks and dismounts the volume, so Windows has no cached
//		idea of the disk left, and asks the driver whether the disk is write
//		protected.
//
//		Linux:
//		  - /dev/fd0 and /dev/fd1 must be block devices of the floppy major (2)
//		    with a floppy geometry from FDGETPRM;
//		  - a USB floppy is a SCSI disk, /dev/sdX, so it is taken only when
//		    sysfs says it is removable and exactly 2880 or 1440 sectors long,
//		    a size nothing else has, and it is not mounted.
//
//		macOS: no raw access (fdr_supported returns 0). Apple's USB floppy
//		support cannot be tried here, and untried code has no place on a path
//		that writes raw to a disk. Images are still made there.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "fdrive.h"
#include "fplat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#elif defined( __linux__ )
#include <dirent.h>
#include <fcntl.h>
#include <linux/fd.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <errno.h>
#endif

//------------------------------------------------------------------------------
//------------------------ Local macros and constants --------------------------
//------------------------------------------------------------------------------

#define SECTOR        512
#define TRACK_BYTES   ( 18 * SECTOR )
#define SIZE_144      ( 2880 * SECTOR )
#define SIZE_720      ( 1440 * SECTOR )
#define MAX_FAKE_LIST 64

//------------------------------------------------------------------------------
//------------------------- Local types and structures -------------------------
//------------------------------------------------------------------------------

struct fdr_handle
{
	int write;
	int sectors;
	int spt;
#ifdef FDR_TEST_DRIVE
	int   fake;
	FILE *file;  // the stand-in drive
#endif
#ifdef _WIN32
	HANDLE   dev;
	int      locked;
	uint8_t *aligned;  // unbuffered I/O needs a sector-aligned buffer
#elif defined( __linux__ )
	int fd;
#endif
};

#ifdef FDR_TEST_DRIVE
//------------------------------------------------------------------------------
//------------------------------ Local variables -------------------------------
//------------------------------------------------------------------------------

static char fake_path[FPLAT_PATH];
static int  fake_bad[MAX_FAKE_LIST];
static int  fake_nbad;
static int  fake_flip[MAX_FAKE_LIST];
static int  fake_nflip;
#endif

//------------------------------------------------------------------------------
//------------------------------- Local functions ------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Copy a message into the caller's error buffer.
  *
  * @param  err The buffer, may be NULL.
  * @param  n Its room.
  * @param  msg The message.
  *
  * @retval None.
  */
static void set_err( char *err, size_t n, const char *msg )
{
	if ( err != NULL && n > 0 )
	{
		snprintf( err, n, "%s", msg );
	}
}

#ifdef FDR_TEST_DRIVE
/**
  * @brief  Whether any sector of a range is in a list.
  *
  * @param  list The list.
  * @param  n Its length.
  * @param  sector First sector of the range.
  * @param  count Sectors in the range.
  *
  * @retval 1 if one is, 0 if not.
  */
static int in_list( const int *list, int n, int sector, int count )
{
	int i;

	for ( i = 0; i < n; i++ )
	{
		if ( list[i] >= sector && list[i] < sector + count )
		{
			return 1;
		}
	}
	return 0;
}

/**
  * @brief  Open the stand-in drive.
  *
  * @param  write 1 to write.
  * @param  out Receives the handle.
  * @param  err Receives what went wrong.
  * @param  errn Room in err.
  *
  * @retval 0 if open, or an FDR_E_ code.
  */
static int fake_open( int write, fdr_handle **out, char *err, size_t errn )
{
	uint64_t size = 0;
	fdr_handle *h;
	FILE *f;

	if ( !fake_path[0] || !fplat_is_regular( fake_path, &size ) ||
	     ( size != SIZE_144 && size != SIZE_720 ) )
	{
		set_err( err, errn, "The test drive's file is not a floppy image." );
		return FDR_E_NOTFLOPPY;
	}
	f = fplat_fopen( fake_path, write ? "r+b" : "rb" );
	if ( f == NULL )
	{
		set_err( err, errn, "Cannot open the test drive's file." );
		return FDR_E_OPEN;
	}
	h = calloc( 1, sizeof *h );
	if ( h == NULL )
	{
		fclose( f );
		return FDR_E_OPEN;
	}
	h->fake = 1;
	h->file = f;
	h->write = write;
	h->sectors = (int)( size / SECTOR );
	h->spt = size == SIZE_144 ? 18 : 9;
	*out = h;
	return 0;
}

/**
  * @brief  Read from the stand-in drive.
  *
  * @param  h The drive.
  * @param  sector First sector.
  * @param  count How many.
  * @param  buf Receives them.
  *
  * @retval 0 if read, FDR_E_IO if not.
  */
static int fake_read( fdr_handle *h, int sector, int count, uint8_t *buf )
{
	if ( in_list( fake_bad, fake_nbad, sector, count ) ||
	     fseek( h->file, (long)sector * SECTOR, SEEK_SET ) != 0 ||
	     fread( buf, SECTOR, (size_t)count, h->file ) != (size_t)count )
	{
		return FDR_E_IO;
	}
	return 0;
}

/**
  * @brief  Write to the stand-in drive.
  *
  * @param  h The drive.
  * @param  sector First sector.
  * @param  count How many.
  * @param  buf The data.
  *
  * @retval 0 if written, FDR_E_IO if not.
  */
static int fake_write( fdr_handle *h, int sector, int count, const uint8_t *buf )
{
	int i;

	if ( in_list( fake_bad, fake_nbad, sector, count ) ||
	     fseek( h->file, (long)sector * SECTOR, SEEK_SET ) != 0 )
	{
		return FDR_E_IO;
	}
	for ( i = 0; i < count; i++ )
	{
		uint8_t s[SECTOR];

		memcpy( s, buf + i * SECTOR, SECTOR );
		if ( in_list( fake_flip, fake_nflip, sector + i, 1 ) )
		{
			s[0] ^= 0xFF;
		}
		if ( fwrite( s, SECTOR, 1, h->file ) != 1 )
		{
			return FDR_E_IO;
		}
	}
	return fflush( h->file ) == 0 ? 0 : FDR_E_IO;
}
#endif

#ifdef _WIN32
/**
  * @brief  The drive test: is this name a floppy drive, by everything that can
  *         be asked without touching the media?
  *
  * @param  id The name.
  *
  * @retval 1 if it is, 0 if not.
  */
static int win_is_floppy( const char *id )
{
	char root[4] = "?:\\", dev[3] = "?:", target[512];

	if ( strlen( id ) != 2 || id[0] < 'A' || id[0] > 'Z' || id[1] != ':' )
	{
		return 0;
	}
	root[0] = dev[0] = id[0];
	if ( GetDriveTypeA( root ) != DRIVE_REMOVABLE )
	{
		return 0;
	}
	if ( QueryDosDeviceA( dev, target, sizeof target ) == 0 )
	{
		return 0;
	}
	return strncmp( target, "\\Device\\Floppy", 14 ) == 0;
}

/**
  * @brief  Check the media in an open drive and note its size.
  *
  * @param  h The drive.
  * @param  err Receives what went wrong.
  * @param  errn Room in err.
  *
  * @retval 0 if it is a 1.44 MB or 720 KB floppy, or an FDR_E_ code.
  */
static int win_check_media( fdr_handle *h, char *err, size_t errn )
{
	DISK_GEOMETRY g;
	DWORD got = 0;

	if ( !DeviceIoControl( h->dev, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &g, sizeof g, &got, NULL ) )
	{
		DWORD e = GetLastError();

		if ( e == ERROR_NOT_READY || e == ERROR_NO_MEDIA_IN_DRIVE )
		{
			set_err( err, errn, "There is no floppy in the drive." );
			return FDR_E_NOMEDIA;
		}
		if ( e == ERROR_UNRECOGNIZED_MEDIA )
		{
			set_err( err, errn, "The floppy is not formatted. Format it first, on the "
			                    "sampler or on the PC." );
			return FDR_E_BADMEDIA;
		}
		set_err( err, errn, "The drive does not report a floppy geometry." );
		return FDR_E_NOTFLOPPY;
	}
	if ( g.MediaType != F3_1Pt44_512 && g.MediaType != F3_720_512 )
	{
		set_err( err, errn, g.MediaType == RemovableMedia || g.MediaType == FixedMedia ?
		         "This drive reports hard disk or memory media, not a floppy. Refused." :
		         "The floppy is not 1.44 MB or 720 KB." );
		return g.MediaType == RemovableMedia || g.MediaType == FixedMedia ?
		       FDR_E_NOTFLOPPY : FDR_E_BADMEDIA;
	}
	if ( g.BytesPerSector != SECTOR || g.TracksPerCylinder != 2 ||
	     g.Cylinders.QuadPart != 80 ||
	     g.SectorsPerTrack != ( g.MediaType == F3_1Pt44_512 ? 18u : 9u ) )
	{
		set_err( err, errn, "The floppy's geometry is not the standard one." );
		return FDR_E_BADMEDIA;
	}
	h->spt = (int)g.SectorsPerTrack;
	h->sectors = h->spt * 2 * 80;
	return 0;
}

/**
  * @brief  Open a Windows floppy drive.
  *
  * @param  id The drive.
  * @param  write 1 to write.
  * @param  out Receives the handle.
  * @param  err Receives what went wrong.
  * @param  errn Room in err.
  *
  * @retval 0 if open, or an FDR_E_ code.
  */
static int os_open( const char *id, int write, fdr_handle **out, char *err, size_t errn )
{
	char path[8] = "\\\\.\\?:";
	fdr_handle *h;
	int rc;
	DWORD got = 0;

	if ( !win_is_floppy( id ) )
	{
		set_err( err, errn, "That drive is not a floppy drive. Refused." );
		return FDR_E_NOTFLOPPY;
	}
	path[4] = id[0];
	h = calloc( 1, sizeof *h );
	if ( h == NULL )
	{
		return FDR_E_OPEN;
	}
	h->aligned = VirtualAlloc( NULL, TRACK_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
	h->dev = CreateFileA( path, write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
	                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	                      FILE_FLAG_NO_BUFFERING | ( write ? FILE_FLAG_WRITE_THROUGH : 0 ), NULL );
	if ( h->dev == INVALID_HANDLE_VALUE || h->aligned == NULL )
	{
		DWORD e = GetLastError();

		set_err( err, errn, e == ERROR_ACCESS_DENIED ?
		         "Windows refused access to the drive. Run this program as administrator." :
		         "Cannot open the drive." );
		fdr_close( h );
		return FDR_E_OPEN;
	}
	h->write = write;
	rc = win_check_media( h, err, errn );
	if ( rc != 0 )
	{
		fdr_close( h );
		return rc;
	}
	if ( write )
	{
		if ( !DeviceIoControl( h->dev, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, &got, NULL ) &&
		     GetLastError() == ERROR_WRITE_PROTECT )
		{
			set_err( err, errn, "The floppy is write protected." );
			fdr_close( h );
			return FDR_E_PROTECTED;
		}
		if ( !DeviceIoControl( h->dev, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL ) )
		{
			set_err( err, errn, "Another program is using the floppy. Close it and try again." );
			fdr_close( h );
			return FDR_E_BUSY;
		}
		h->locked = 1;
		DeviceIoControl( h->dev, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL );
	}
	*out = h;
	return 0;
}

/**
  * @brief  Read sectors from an open drive.
  *
  * @param  h The drive.
  * @param  sector First sector.
  * @param  count How many.
  * @param  buf Receives them.
  *
  * @retval 0 if read, FDR_E_IO if not.
  */
static int os_read( fdr_handle *h, int sector, int count, uint8_t *buf )
{
	LARGE_INTEGER at;
	DWORD got = 0, n = (DWORD)count * SECTOR;

	at.QuadPart = (LONGLONG)sector * SECTOR;
	if ( !SetFilePointerEx( h->dev, at, NULL, FILE_BEGIN ) ||
	     !ReadFile( h->dev, h->aligned, n, &got, NULL ) || got != n )
	{
		return FDR_E_IO;
	}
	memcpy( buf, h->aligned, n );
	return 0;
}

/**
  * @brief  Write sectors to an open drive.
  *
  * @param  h The drive.
  * @param  sector First sector.
  * @param  count How many.
  * @param  buf The data.
  *
  * @retval 0 if written, FDR_E_IO or FDR_E_PROTECTED if not.
  */
static int os_write( fdr_handle *h, int sector, int count, const uint8_t *buf )
{
	LARGE_INTEGER at;
	DWORD got = 0, n = (DWORD)count * SECTOR;

	memcpy( h->aligned, buf, n );
	at.QuadPart = (LONGLONG)sector * SECTOR;
	if ( !SetFilePointerEx( h->dev, at, NULL, FILE_BEGIN ) ||
	     !WriteFile( h->dev, h->aligned, n, &got, NULL ) || got != n )
	{
		return GetLastError() == ERROR_WRITE_PROTECT ? FDR_E_PROTECTED : FDR_E_IO;
	}
	return 0;
}

/**
  * @brief  Flush an open drive.
  *
  * @param  h The drive.
  *
  * @retval 0 if done, FDR_E_IO if not.
  */
static int os_flush( fdr_handle *h )
{
	return !h->write || FlushFileBuffers( h->dev ) ? 0 : FDR_E_IO;
}

/**
  * @brief  Release what an open drive holds.
  *
  * @param  h The drive.
  *
  * @retval None.
  */
static void os_close( fdr_handle *h )
{
	DWORD got = 0;

	if ( h->dev != INVALID_HANDLE_VALUE && h->dev != NULL )
	{
		if ( h->locked )
		{
			DeviceIoControl( h->dev, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL );
		}
		CloseHandle( h->dev );
	}
	if ( h->aligned != NULL )
	{
		VirtualFree( h->aligned, 0, MEM_RELEASE );
	}
}

/**
  * @brief  List the floppy drives.
  *
  * @param  out Receives them.
  *
  * @retval The count.
  */
static int os_list( fdr_drive *out )
{
	int n = 0;
	char c;

	for ( c = 'A'; c <= 'Z' && n < FDR_MAX_DRIVES; c++ )
	{
		char id[3] = "?:";

		id[0] = c;
		if ( win_is_floppy( id ) )
		{
			snprintf( out[n].id, sizeof out[n].id, "%s", id );
			snprintf( out[n].label, sizeof out[n].label, "%s floppy drive", id );
			n++;
		}
	}
	return n;
}

#elif defined( __linux__ )

/**
  * @brief  Read a one-line sysfs value.
  *
  * @param  path The file.
  * @param  v Receives the number.
  *
  * @retval 1 if read, 0 if not.
  */
static int sys_long( const char *path, long *v )
{
	FILE *f = fopen( path, "r" );
	int ok;

	if ( f == NULL )
	{
		return 0;
	}
	ok = fscanf( f, "%ld", v ) == 1;
	fclose( f );
	return ok;
}

/**
  * @brief  Whether a device is mounted, or has a mounted partition.
  *
  * @param  dev Its path.
  *
  * @retval 1 if it is, or if that cannot be told, 0 if not.
  */
static int is_mounted( const char *dev )
{
	FILE *f = fopen( "/proc/mounts", "r" );
	char line[1024];
	size_t n = strlen( dev );
	int found = 0;

	if ( f == NULL )
	{
		return 1;
	}
	while ( !found && fgets( line, sizeof line, f ) != NULL )
	{
		found = strncmp( line, dev, n ) == 0;
	}
	fclose( f );
	return found;
}

/**
  * @brief  The drive test for a USB floppy: /dev/sdX of a floppy's exact size.
  *
  * @param  name The name after /dev/.
  * @param  sectors Receives the size.
  *
  * @retval 1 if it is one, 0 if not.
  */
static int usb_is_floppy( const char *name, long *sectors )
{
	char path[128];
	long removable = 0, size = 0;

	if ( strncmp( name, "sd", 2 ) != 0 || !name[2] || name[3] )
	{
		return 0;
	}
	snprintf( path, sizeof path, "/sys/block/%s/removable", name );
	if ( !sys_long( path, &removable ) || removable != 1 )
	{
		return 0;
	}
	snprintf( path, sizeof path, "/sys/block/%s/size", name );
	if ( !sys_long( path, &size ) || ( size != 2880 && size != 1440 ) )
	{
		return 0;
	}
	*sectors = size;
	return 1;
}

/**
  * @brief  The drive test for a built-in drive: /dev/fd0 or /dev/fd1.
  *
  * @param  id The device path.
  *
  * @retval 1 if it is one, 0 if not.
  */
static int fd_is_floppy( const char *id )
{
	struct stat st;

	return ( strcmp( id, "/dev/fd0" ) == 0 || strcmp( id, "/dev/fd1" ) == 0 ) &&
	       stat( id, &st ) == 0 && S_ISBLK( st.st_mode ) && major( st.st_rdev ) == 2;
}

/**
  * @brief  Open a Linux floppy drive.
  *
  * @param  id The drive.
  * @param  write 1 to write.
  * @param  out Receives the handle.
  * @param  err Receives what went wrong.
  * @param  errn Room in err.
  *
  * @retval 0 if open, or an FDR_E_ code.
  */
static int os_open( const char *id, int write, fdr_handle **out, char *err, size_t errn )
{
	fdr_handle *h;
	long usb_size = 0;
	int builtin = fd_is_floppy( id );
	int usb = !builtin && strncmp( id, "/dev/", 5 ) == 0 && usb_is_floppy( id + 5, &usb_size );
	int fd;

	if ( !builtin && !usb )
	{
		set_err( err, errn, "That device is not a floppy drive. Refused." );
		return FDR_E_NOTFLOPPY;
	}
	if ( is_mounted( id ) )
	{
		set_err( err, errn, "The floppy is mounted. Unmount it first." );
		return FDR_E_BUSY;
	}
	fd = open( id, ( write ? O_RDWR : O_RDONLY ) | ( write ? O_EXCL : 0 ) | O_CLOEXEC );
	if ( fd < 0 )
	{
		set_err( err, errn, errno == EACCES ?
		         "No permission for the drive. Run as root, or for a built-in drive join the 'floppy' "
		         "group. Do not join 'disk': it also opens the hard disks." :
		         errno == EROFS ? "The floppy is write protected." :
		         errno == ENXIO ? "There is no floppy in the drive." : "Cannot open the drive." );
		return errno == EROFS ? FDR_E_PROTECTED : errno == ENXIO ? FDR_E_NOMEDIA : FDR_E_OPEN;
	}
	h = calloc( 1, sizeof *h );
	if ( h == NULL )
	{
		close( fd );
		return FDR_E_OPEN;
	}
	h->fd = fd;
	h->write = write;
	if ( builtin )
	{
		struct floppy_struct g;

		if ( ioctl( fd, FDGETPRM, &g ) != 0 )
		{
			set_err( err, errn, "There is no floppy in the drive, or it is not formatted." );
			fdr_close( h );
			return FDR_E_NOMEDIA;
		}
		if ( g.size != 2880 && g.size != 1440 )
		{
			set_err( err, errn, "The floppy is not 1.44 MB or 720 KB." );
			fdr_close( h );
			return FDR_E_BADMEDIA;
		}
		h->sectors = (int)g.size;
	}
	else
	{
		h->sectors = (int)usb_size;
	}
	h->spt = h->sectors == 2880 ? 18 : 9;
	*out = h;
	return 0;
}

/**
  * @brief  Read sectors from an open drive.
  *
  * @param  h The drive.
  * @param  sector First sector.
  * @param  count How many.
  * @param  buf Receives them.
  *
  * @retval 0 if read, FDR_E_IO if not.
  */
static int os_read( fdr_handle *h, int sector, int count, uint8_t *buf )
{
	size_t n = (size_t)count * SECTOR;

	return pread( h->fd, buf, n, (off_t)sector * SECTOR ) == (ssize_t)n ? 0 : FDR_E_IO;
}

/**
  * @brief  Write sectors to an open drive.
  *
  * @param  h The drive.
  * @param  sector First sector.
  * @param  count How many.
  * @param  buf The data.
  *
  * @retval 0 if written, FDR_E_IO or FDR_E_PROTECTED if not.
  */
static int os_write( fdr_handle *h, int sector, int count, const uint8_t *buf )
{
	size_t n = (size_t)count * SECTOR;

	if ( pwrite( h->fd, buf, n, (off_t)sector * SECTOR ) != (ssize_t)n )
	{
		return errno == EROFS ? FDR_E_PROTECTED : FDR_E_IO;
	}
	return 0;
}

/**
  * @brief  Flush an open drive, so a verify reads the floppy and not the cache.
  *
  * @param  h The drive.
  *
  * @retval 0 if done, FDR_E_IO if not.
  */
static int os_flush( fdr_handle *h )
{
	if ( h->write && fsync( h->fd ) != 0 )
	{
		return FDR_E_IO;
	}
	ioctl( h->fd, BLKFLSBUF, 0 );
	return 0;
}

/**
  * @brief  Release what an open drive holds.
  *
  * @param  h The drive.
  *
  * @retval None.
  */
static void os_close( fdr_handle *h )
{
	if ( h->fd > 0 )
	{
		close( h->fd );
	}
}

/**
  * @brief  List the floppy drives.
  *
  * @param  out Receives them.
  *
  * @retval The count.
  */
static int os_list( fdr_drive *out )
{
	static const char *const fds[2] = { "/dev/fd0", "/dev/fd1" };
	DIR *d;
	struct dirent *e;
	int n = 0, i;

	for ( i = 0; i < 2; i++ )
	{
		if ( fd_is_floppy( fds[i] ) )
		{
			snprintf( out[n].id, sizeof out[n].id, "%s", fds[i] );
			snprintf( out[n].label, sizeof out[n].label, "%s floppy drive", fds[i] );
			n++;
		}
	}
	d = opendir( "/sys/block" );
	if ( d == NULL )
	{
		return n;
	}
	while ( n < FDR_MAX_DRIVES && ( e = readdir( d ) ) != NULL )
	{
		long size;

		if ( usb_is_floppy( e->d_name, &size ) )
		{
			snprintf( out[n].id, sizeof out[n].id, "/dev/%.16s", e->d_name );
			snprintf( out[n].label, sizeof out[n].label, "/dev/%.16s USB floppy drive", e->d_name );
			n++;
		}
	}
	closedir( d );
	return n;
}

#else

/**
  * @brief  No raw access on this system.
  *
  * @param  id The drive.
  * @param  write 1 to write.
  * @param  out Receives the handle.
  * @param  err Receives what went wrong.
  * @param  errn Room in err.
  *
  * @retval FDR_E_UNSUPPORTED.
  */
static int os_open( const char *id, int write, fdr_handle **out, char *err, size_t errn )
{
	(void)id; (void)write; (void)out;
	set_err( err, errn, "This build has no raw floppy access on this system." );
	return FDR_E_UNSUPPORTED;
}

/**
  * @brief  No raw access on this system.
  *
  * @param  h The drive.
  * @param  sector First sector.
  * @param  count How many.
  * @param  buf Receives them.
  *
  * @retval FDR_E_UNSUPPORTED.
  */
static int os_read( fdr_handle *h, int sector, int count, uint8_t *buf )
{
	(void)h; (void)sector; (void)count; (void)buf;
	return FDR_E_UNSUPPORTED;
}

/**
  * @brief  No raw access on this system.
  *
  * @param  h The drive.
  * @param  sector First sector.
  * @param  count How many.
  * @param  buf The data.
  *
  * @retval FDR_E_UNSUPPORTED.
  */
static int os_write( fdr_handle *h, int sector, int count, const uint8_t *buf )
{
	(void)h; (void)sector; (void)count; (void)buf;
	return FDR_E_UNSUPPORTED;
}

/**
  * @brief  No raw access on this system.
  *
  * @param  h The drive.
  *
  * @retval FDR_E_UNSUPPORTED.
  */
static int os_flush( fdr_handle *h )
{
	(void)h;
	return FDR_E_UNSUPPORTED;
}

/**
  * @brief  No raw access on this system.
  *
  * @param  h The drive.
  *
  * @retval None.
  */
static void os_close( fdr_handle *h )
{
	(void)h;
}

/**
  * @brief  No raw access on this system.
  *
  * @param  out Receives no drives.
  *
  * @retval 0.
  */
static int os_list( fdr_drive *out )
{
	(void)out;
	return 0;
}

#endif

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

int fdr_supported( void )
{
#if defined( _WIN32 ) || defined( __linux__ )
	return 1;
#else
	return 0;
#endif
}

int fdr_list( fdr_drive *out )
{
	int n = os_list( out );

#ifdef FDR_TEST_DRIVE
	if ( fake_path[0] && n < FDR_MAX_DRIVES )
	{
		snprintf( out[n].id, sizeof out[n].id, "%s", FDR_FAKE_ID );
		snprintf( out[n].label, sizeof out[n].label, "Test drive (a file, not a floppy)" );
		n++;
	}
#endif
	return n;
}

int fdr_open( const char *id, int write, fdr_handle **out, char *err, size_t errn )
{
	*out = NULL;
#ifdef FDR_TEST_DRIVE
	if ( strcmp( id, FDR_FAKE_ID ) == 0 )
	{
		return fake_open( write, out, err, errn );
	}
#endif
	return os_open( id, write, out, err, errn );
}

int fdr_sectors( const fdr_handle *h )
{
	return h->sectors;
}

int fdr_track_sectors( const fdr_handle *h )
{
	return h->spt;
}

int fdr_read( fdr_handle *h, int sector, int count, uint8_t *buf )
{
	if ( sector < 0 || count < 1 || count > h->spt || sector + count > h->sectors )
	{
		return FDR_E_IO;
	}
#ifdef FDR_TEST_DRIVE
	if ( h->fake )
	{
		return fake_read( h, sector, count, buf );
	}
#endif
	return os_read( h, sector, count, buf );
}

int fdr_write( fdr_handle *h, int sector, int count, const uint8_t *buf )
{
	if ( !h->write || sector < 0 || count < 1 || count > h->spt || sector + count > h->sectors )
	{
		return FDR_E_IO;
	}
#ifdef FDR_TEST_DRIVE
	if ( h->fake )
	{
		return fake_write( h, sector, count, buf );
	}
#endif
	return os_write( h, sector, count, buf );
}

int fdr_flush( fdr_handle *h )
{
#ifdef FDR_TEST_DRIVE
	if ( h->fake )
	{
		return 0;
	}
#endif
	return os_flush( h );
}

void fdr_close( fdr_handle *h )
{
	if ( h == NULL )
	{
		return;
	}
#ifdef FDR_TEST_DRIVE
	if ( h->fake )
	{
		fclose( h->file );
		free( h );
		return;
	}
#endif
	os_close( h );
	free( h );
}

#ifdef FDR_TEST_DRIVE
int fdr_fake_set( const char *path, const int *bad, int nbad, const int *flip, int nflip )
{
	uint64_t size = 0;

	if ( !fplat_is_regular( path, &size ) || ( size != SIZE_144 && size != SIZE_720 ) ||
	     nbad > MAX_FAKE_LIST || nflip > MAX_FAKE_LIST )
	{
		return -1;
	}
	snprintf( fake_path, sizeof fake_path, "%s", path );
	fake_nbad = nbad;
	fake_nflip = nflip;
	if ( nbad > 0 )
	{
		memcpy( fake_bad, bad, (size_t)nbad * sizeof *bad );
	}
	if ( nflip > 0 )
	{
		memcpy( fake_flip, flip, (size_t)nflip * sizeof *flip );
	}
	return 0;
}
#endif
