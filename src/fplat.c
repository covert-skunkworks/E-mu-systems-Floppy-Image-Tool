//  Description:
//
//		fplat.c -- the few file-system calls that differ between the systems.
//
//		On Windows the C library's narrow calls take the ANSI code page, not
//		UTF-8, so a bank with an accented name would not open if dropped on the
//		window; everything goes through the wide calls instead.
//
//		Every file this program saves goes through fplat_write_file, and it will
//		only ever write to an ordinary file. A path can also name a hard disk
//		(/dev/sda, \\.\PhysicalDrive0), and the plain C way of saving, fopen with
//		"wb", would overwrite one given the rights to. Asking first is not enough,
//		since the answer can change before the open, so the file is opened without
//		emptying it, the open handle is what gets checked, and only then is it
//		cut to length.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "fplat.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#endif

//------------------------------------------------------------------------------
//------------------------------- Local functions ------------------------------
//------------------------------------------------------------------------------

#ifdef _WIN32
/**
  * @brief  UTF-8 to UTF-16.
  *
  * @param  s The UTF-8 text.
  * @param  out Receives the text.
  * @param  n Room in out, in characters.
  *
  * @retval 1 if converted, 0 if not.
  */
static int widen( const char *s, wchar_t *out, int n )
{
	return MultiByteToWideChar( CP_UTF8, 0, s, -1, out, n ) > 0;
}

/**
  * @brief  Does a path name a device rather than a file?
  *
  * Windows spells a device several ways: \\.\C:, //./C:, \\?\C:, \??\C:, and
  * plain names such as NUL or COM1, in any folder. Windows' own conversion to a
  * full path turns all but \??\ into the \\.\ or \\?\ form, so that is what gets
  * compared, rather than a list of spellings that is bound to miss one. \??\ has
  * to be looked for as typed, because the conversion takes it for an ordinary
  * folder while the file calls hand it to the system as it is.
  *
  * @param  p The path.
  *
  * @retval 1 if it does or if that cannot be told, 0 if not.
  */
static int is_device_path( const wchar_t *p )
{
	wchar_t f[FPLAT_PATH];
	DWORD n;

	if ( wcsncmp( p, L"\\??\\", 4 ) == 0 )
	{
		return 1;
	}
	n = GetFullPathNameW( p, FPLAT_PATH, f, NULL );
	if ( n == 0 || n >= FPLAT_PATH )
	{
		return 1;
	}
	return wcsncmp( f, L"\\\\.\\", 4 ) == 0 || wcsncmp( f, L"\\\\?\\", 4 ) == 0;
}
#endif

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

FILE *fplat_fopen( const char *path, const char *mode )
{
#ifdef _WIN32
	wchar_t p[FPLAT_PATH], m[16];

	if ( !widen( path, p, FPLAT_PATH ) || !widen( mode, m, 16 ) )
	{
		return NULL;
	}
	return _wfopen( p, m );
#else
	return fopen( path, mode );
#endif
}

int fplat_read_file( const char *path, uint8_t **data, size_t *len, size_t max )
{
	uint64_t size;
	FILE *f;
	uint8_t *p;

	if ( !fplat_is_regular( path, &size ) )
	{
		return -1;
	}
	if ( size > max )
	{
		return -2;
	}
	p = malloc( size ? (size_t)size : 1 );
	if ( p == NULL )
	{
		return -3;
	}
	f = fplat_fopen( path, "rb" );
	if ( f == NULL || fread( p, 1, (size_t)size, f ) != (size_t)size )
	{
		if ( f != NULL )
		{
			fclose( f );
		}
		free( p );
		return -1;
	}
	fclose( f );
	*data = p;
	*len = (size_t)size;
	return 0;
}

int fplat_write_target_ok( const char *path )
{
#ifdef _WIN32
	wchar_t p[FPLAT_PATH];
	DWORD a;

	if ( !widen( path, p, FPLAT_PATH ) || is_device_path( p ) )
	{
		return 0;
	}
	a = GetFileAttributesW( p );
	return a == INVALID_FILE_ATTRIBUTES || !( a & ( FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE ) );
#else
	struct stat st;

	return stat( path, &st ) != 0 || S_ISREG( st.st_mode );
#endif
}

int fplat_write_file( const char *path, const void *data, size_t len )
{
	const uint8_t *b = data;
	int ok = 1;

#ifdef _WIN32
	wchar_t p[FPLAT_PATH];
	HANDLE h;

	if ( !widen( path, p, FPLAT_PATH ) )
	{
		return -1;
	}
	if ( !fplat_write_target_ok( path ) )
	{
		return -2;
	}
	h = CreateFileW( p, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	if ( h == INVALID_HANDLE_VALUE )
	{
		return -1;
	}
	// consoles, ports and pipes report otherwise; a raw disk was refused by name
	if ( GetFileType( h ) != FILE_TYPE_DISK )
	{
		CloseHandle( h );
		return -2;
	}
	ok = SetEndOfFile( h ) != 0;
	while ( ok && len > 0 )
	{
		DWORD done = 0;
		DWORD chunk = len > 0x40000000u ? 0x40000000u : (DWORD)len;

		ok = WriteFile( h, b, chunk, &done, NULL ) && done == chunk;
		b += chunk;
		len -= chunk;
	}
	if ( !CloseHandle( h ) )
	{
		ok = 0;
	}
#else
	struct stat st;
	int fd;

	if ( !fplat_write_target_ok( path ) )
	{
		return -2;
	}
	// O_NONBLOCK: a FIFO nobody reads refuses at once instead of hanging here
	fd = open( path, O_WRONLY | O_CREAT | O_NONBLOCK | O_NOCTTY | O_CLOEXEC, 0666 );
	if ( fd < 0 )
	{
		return errno == ENXIO ? -2 : -1;
	}
	if ( fstat( fd, &st ) != 0 || !S_ISREG( st.st_mode ) )
	{
		close( fd );
		return -2;
	}
	ok = ftruncate( fd, 0 ) == 0;
	while ( ok && len > 0 )
	{
		ssize_t done = write( fd, b, len );

		if ( done < 0 && errno == EINTR )
		{
			continue;
		}
		ok = done > 0;
		if ( ok )
		{
			b += done;
			len -= (size_t)done;
		}
	}
	if ( close( fd ) != 0 )
	{
		ok = 0;
	}
#endif
	return ok ? 0 : -1;
}

int fplat_is_regular( const char *path, uint64_t *size )
{
#ifdef _WIN32
	wchar_t p[FPLAT_PATH];
	WIN32_FILE_ATTRIBUTE_DATA a;

	// a raw volume such as \\.\A: has attributes of its own, so the name is checked first
	if ( !widen( path, p, FPLAT_PATH ) || is_device_path( p ) ||
	     !GetFileAttributesExW( p, GetFileExInfoStandard, &a ) ||
	     ( a.dwFileAttributes & ( FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE ) ) )
	{
		return 0;
	}
	if ( size != NULL )
	{
		*size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
	}
	return 1;
#else
	struct stat st;

	if ( stat( path, &st ) != 0 || !S_ISREG( st.st_mode ) )
	{
		return 0;
	}
	if ( size != NULL )
	{
		*size = (uint64_t)st.st_size;
	}
	return 1;
#endif
}

int fplat_is_dir( const char *path )
{
#ifdef _WIN32
	wchar_t p[FPLAT_PATH];
	DWORD a;

	if ( !widen( path, p, FPLAT_PATH ) )
	{
		return 0;
	}
	a = GetFileAttributesW( p );
	return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY );
#else
	struct stat st;

	return stat( path, &st ) == 0 && S_ISDIR( st.st_mode );
#endif
}

const char *fplat_base( const char *path )
{
	const char *b = path, *p;

	for ( p = path; *p; p++ )
	{
		if ( *p == '/' || *p == '\\' )
		{
			b = p + 1;
		}
	}
	return b;
}

int fplat_full_path( const char *path, char *out, size_t n )
{
#ifdef _WIN32
	wchar_t p[FPLAT_PATH], f[FPLAT_PATH];

	if ( !widen( path, p, FPLAT_PATH ) || GetFullPathNameW( p, FPLAT_PATH, f, NULL ) == 0 ||
	     WideCharToMultiByte( CP_UTF8, 0, f, -1, out, (int)n, NULL, NULL ) == 0 )
	{
		return -1;
	}
	return 0;
#else
	char f[PATH_MAX];
	size_t len;

	if ( realpath( path, f ) == NULL )
	{
		return -1;
	}
	len = strlen( f );
	if ( len >= n )
	{
		return -1;
	}
	memcpy( out, f, len + 1 );
	return 0;
#endif
}

int fplat_list_dir( const char *dir, void ( *fn )( const char *, int, void * ), void *ctx )
{
#ifdef _WIN32
	wchar_t p[FPLAT_PATH + 8];
	char utf[FPLAT_PATH];
	WIN32_FIND_DATAW d;
	HANDLE h;
	size_t len;

	if ( !widen( dir, p, FPLAT_PATH ) )
	{
		return -1;
	}
	len = wcslen( p );
	if ( len && p[len - 1] != L'\\' && p[len - 1] != L'/' )
	{
		p[len++] = L'\\';
	}
	p[len++] = L'*';
	p[len] = 0;
	h = FindFirstFileW( p, &d );
	if ( h == INVALID_HANDLE_VALUE )
	{
		return -1;
	}
	do
	{
		if ( WideCharToMultiByte( CP_UTF8, 0, d.cFileName, -1, utf, sizeof utf, NULL, NULL ) )
		{
			fn( utf, ( d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0, ctx );
		}
	} while ( FindNextFileW( h, &d ) );
	FindClose( h );
	return 0;
#else
	DIR *d = opendir( dir );
	struct dirent *e;
	char path[FPLAT_PATH * 2];

	if ( d == NULL )
	{
		return -1;
	}
	while ( ( e = readdir( d ) ) != NULL )
	{
		snprintf( path, sizeof path, "%s/%s", dir, e->d_name );
		fn( e->d_name, fplat_is_dir( path ), ctx );
	}
	closedir( d );
	return 0;
#endif
}

int fplat_stricmp( const char *a, const char *b )
{
	for ( ; *a && *b; a++, b++ )
	{
		int x = tolower( (unsigned char)*a ), y = tolower( (unsigned char)*b );

		if ( x != y )
		{
			return x - y;
		}
	}
	return (unsigned char)*a - (unsigned char)*b;
}

uint32_t fplat_dos_time( void )
{
	time_t now = time( NULL );
	struct tm *m = localtime( &now );
	uint32_t t, d;

	if ( m == NULL || m->tm_year < 80 )
	{
		return 0;
	}
	t = (uint32_t)( m->tm_hour << 11 | m->tm_min << 5 | m->tm_sec / 2 );
	d = (uint32_t)( ( m->tm_year - 80 ) << 9 | ( m->tm_mon + 1 ) << 5 | m->tm_mday );
	return d << 16 | t;
}
