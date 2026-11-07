//  Description:
//
//		main.c -- floppy_image: floppy disks for E-mu samplers on EOS before its
//		FAT32 support. With no arguments it opens the window; the options below
//		do the same work from a command line, for scripts and tests.
//
//		A write from the command line needs -yes as well as the drive: there is
//		no box to ask in.
//
//		Only the test builds (FDR_TEST_DRIVE) know the -fake and -click options.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "fdrive.h"
#include "fgui.h"
#include "fimg.h"
#include "fjob.h"
#include "fplat.h"
#include "nk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

//------------------------------------------------------------------------------
//------------------------ Local macros and constants --------------------------
//------------------------------------------------------------------------------

#ifndef FI_VERSION
#define FI_VERSION "dev"
#endif

#define MAX_LIST 64

//------------------------------------------------------------------------------
//------------------------------ Local variables -------------------------------
//------------------------------------------------------------------------------

static const char usage_text[] =
	"floppy_image " FI_VERSION ": floppy disks for E-mu samplers on EOS before 4.7\n"
	"\n"
	"  floppy_image                          the window\n"
	"  floppy_image -new eos -o OUT.img [-name NAME] [BANK]\n"
	"                                        EOS's own format: one bank, on as many\n"
	"                                        images as it takes (OUT_1of3.img ...);\n"
	"                                        no bank makes a blank floppy\n"
	"  floppy_image -new dos -o OUT.img [-label LABEL] [FILE...]\n"
	"                                        a DOS (FAT12) floppy holding the files\n"
	"  floppy_image -info IMAGE              what an image holds\n"
	"  floppy_image -drives                  the floppy drives\n"
	"  floppy_image -read DRIVE OUT.img      save a floppy; the drive stays read-only\n"
	"  floppy_image -write DRIVE IMAGE -yes  write an image, then verify it\n"
	"  floppy_image -smoke OUT.bmp [PAGES]   draw the window once, save it, exit\n"
	"  floppy_image -window [PAGES]          the window, with the pages filled in\n"
	"     PAGES: -tab 0|1|2 (make, write, read)  -dos  -add FILE  -image IMG  -o OUT\n"
#ifdef FDR_TEST_DRIVE
	"\n"
	"  Test build: -fake FILE[:bad=N,N][:flip=N,N] first adds a stand-in drive, an\n"
	"  ordinary file; -click FRAME:X:Y among PAGES scripts a mouse click.\n"
#endif
	;

//------------------------------------------------------------------------------
//------------------------------- Local functions ------------------------------
//------------------------------------------------------------------------------

#ifdef FDR_TEST_DRIVE
/**
  * @brief  Parse "N,N,N" into a list.
  *
  * @param  s The text.
  * @param  out Receives the numbers.
  *
  * @retval How many.
  */
static int parse_list( const char *s, int *out )
{
	int n = 0;

	while ( *s && n < MAX_LIST )
	{
		char *e;

		out[n++] = (int)strtol( s, &e, 10 );
		s = *e == ',' ? e + 1 : "";
	}
	return n;
}

/**
  * @brief  Set up the stand-in drive from the -fake option.
  *
  * @param  spec FILE[:bad=N,N][:flip=N,N]; Windows paths may hold a colon.
  *
  * @retval 0 if set, -1 if not.
  */
static int fake_option( const char *spec )
{
	char path[FPLAT_PATH], *p;
	int bad[MAX_LIST], flip[MAX_LIST], nbad = 0, nflip = 0;

	snprintf( path, sizeof path, "%s", spec );
	p = strstr( path, ":flip=" );
	if ( p != NULL )
	{
		nflip = parse_list( p + 6, flip );
		*p = 0;
	}
	p = strstr( path, ":bad=" );
	if ( p != NULL )
	{
		nbad = parse_list( p + 5, bad );
		*p = 0;
	}
	return fdr_fake_set( path, bad, nbad, flip, nflip );
}

#endif

/**
  * @brief  Print a finished transfer's outcome.
  *
  * @param  j The job.
  *
  * @retval 0 if it succeeded with nothing bad, 1 if not.
  */
static int report( const fjob *j )
{
	int i;

	printf( "%s\n", j->message );
	for ( i = 0; i < j->nbad; i++ )
	{
		char chs[40];

		fjob_chs( j->bad[i], j->total == FIMG_SECTORS_720 ? 9 : 18, chs, sizeof chs );
		printf( "  sector %d, C/H/S %s\n", j->bad[i], chs );
	}
	return j->result != FJOB_OK || j->nbad != 0;
}

/**
  * @brief  The -new command.
  *
  * @param  argc Arguments after the format.
  * @param  argv The arguments.
  * @param  fmt "eos" or "dos".
  *
  * @retval The process's exit code.
  */
static int cmd_new( int argc, char **argv, const char *fmt )
{
	const char *out = NULL, *name = NULL, *lab = NULL;
	const char **list = calloc( (size_t)argc + 1, sizeof *list );
	static char msg[8192];
	fjob_set set = { NULL, 0 };
	int i, n = 0, rc;

	if ( list == NULL )
	{
		return 1;
	}
	for ( i = 0; i < argc; i++ )
	{
		if ( strcmp( argv[i], "-o" ) == 0 && i + 1 < argc )
		{
			out = argv[++i];
		}
		else if ( strcmp( argv[i], "-name" ) == 0 && i + 1 < argc )
		{
			name = argv[++i];
		}
		else if ( strcmp( argv[i], "-label" ) == 0 && i + 1 < argc )
		{
			lab = argv[++i];
		}
		else
		{
			list[n++] = argv[i];
		}
	}
	if ( out == NULL || ( strcmp( fmt, "eos" ) == 0 && n > 1 ) )
	{
		free( list );
		fputs( usage_text, stderr );
		return 2;
	}
	if ( strcmp( fmt, "eos" ) == 0 )
	{
		rc = fjob_set_eos( &set, n ? list[0] : NULL, name, msg, sizeof msg );
	}
	else if ( strcmp( fmt, "dos" ) == 0 )
	{
		rc = fjob_set_dos( &set, list, n, lab, msg, sizeof msg );
	}
	else
	{
		free( list );
		fputs( usage_text, stderr );
		return 2;
	}
	free( list );
	printf( "%s\n", msg );
	if ( rc < 0 )
	{
		return 1;
	}
	rc = fjob_set_save( &set, out, msg, sizeof msg );
	fjob_set_free( &set );
	printf( "%s\n", msg );
	return rc != 0;
}

/**
  * @brief  The -info command.
  *
  * @param  path The image.
  *
  * @retval The process's exit code.
  */
static int cmd_info( const char *path )
{
	uint8_t *d = NULL;
	size_t len = 0;
	fimg_info info;

	if ( fplat_read_file( path, &d, &len, FIMG_SIZE_144 ) != 0 )
	{
		fprintf( stderr, "Cannot read %s, or it is too large to be a floppy image.\n", path );
		return 1;
	}
	fimg_inspect( d, len, &info );
	free( d );
	printf( "%s\n", info.text );
	return info.kind == FIMG_KIND_UNKNOWN;
}

/**
  * @brief  The -drives command.
  *
  * @retval The process's exit code.
  */
static int cmd_drives( void )
{
	fdr_drive d[FDR_MAX_DRIVES];
	int i, n = fdr_list( d );

	if ( !fdr_supported() )
	{
		printf( "No raw floppy access on this system.\n" );
	}
	for ( i = 0; i < n; i++ )
	{
		printf( "%s\t%s\n", d[i].id, d[i].label );
	}
	if ( n == 0 && fdr_supported() )
	{
		printf( "No floppy drive found.\n" );
	}
	return 0;
}

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  The program.
  *
  * SDL_main is defined by SDL on some systems, and this file has no use for
  * its argument handling, so SDL is told to leave main alone.
  *
  * @param  argc Argument count.
  * @param  argv The arguments.
  *
  * @retval The process's exit code.
  */
int main( int argc, char **argv )
{
	static fjob job;
	const char *smoke = NULL;
	int tab = 0, i = 1;

	SDL_SetMainReady();
#ifdef _WIN32
	// A window program has no console, so a command line would see no output.
	if ( argc > 1 && AttachConsole( ATTACH_PARENT_PROCESS ) )
	{
		if ( GetFileType( GetStdHandle( STD_OUTPUT_HANDLE ) ) == FILE_TYPE_UNKNOWN )
		{
			freopen( "CONOUT$", "w", stdout );
		}
		if ( GetFileType( GetStdHandle( STD_ERROR_HANDLE ) ) == FILE_TYPE_UNKNOWN )
		{
			freopen( "CONOUT$", "w", stderr );
		}
	}
#endif
#ifdef FDR_TEST_DRIVE
	while ( i + 1 < argc && strcmp( argv[i], "-fake" ) == 0 )
	{
		if ( fake_option( argv[i + 1] ) != 0 )
		{
			fprintf( stderr, "-fake takes an ordinary file of a floppy's size.\n" );
			return 2;
		}
		i += 2;
	}
#endif
	if ( i >= argc )
	{
		return fgui_run( NULL, 0 );
	}
	if ( strcmp( argv[i], "-window" ) == 0 || strcmp( argv[i], "-smoke" ) == 0 )
	{
		int dos = 0;

		if ( strcmp( argv[i], "-smoke" ) == 0 && i + 1 < argc )
		{
			smoke = argv[++i];
		}
		for ( i++; i < argc; i++ )
		{
			if ( strcmp( argv[i], "-dos" ) == 0 )
			{
				dos = 1;
				fgui_preset( dos, NULL, NULL, NULL );
			}
			else if ( i + 1 >= argc )
			{
				fputs( usage_text, stderr );
				return 2;
			}
#ifdef FDR_TEST_DRIVE
			else if ( strcmp( argv[i], "-click" ) == 0 )
			{
				int f = 0, x = 0, y = 0;

				if ( sscanf( argv[++i], "%d:%d:%d", &f, &x, &y ) == 3 )
				{
					fgui_test_click( f, x, y );
				}
			}
#endif
			else if ( strcmp( argv[i], "-tab" ) == 0 )
			{
				char *end;
				long t = strtol( argv[++i], &end, 10 );

				if ( end == argv[i] || *end || t < 0 || t > 2 )  // the three pages
				{
					fputs( usage_text, stderr );
					return 2;
				}
				tab = (int)t;
			}
			else if ( strcmp( argv[i], "-add" ) == 0 )
			{
				fgui_preset( dos, argv[++i], NULL, NULL );
			}
			else if ( strcmp( argv[i], "-image" ) == 0 )
			{
				fgui_preset( dos, NULL, argv[++i], NULL );
			}
			else if ( strcmp( argv[i], "-o" ) == 0 )
			{
				fgui_preset( dos, NULL, NULL, argv[++i] );
			}
			else
			{
				fputs( usage_text, stderr );
				return 2;
			}
		}
		// the screenshot is saved by SDL, not by fplat_write_file
		if ( smoke != NULL && !fplat_write_target_ok( smoke ) )
		{
			fprintf( stderr, "Refused: %s is not an ordinary file.\n", smoke );
			return 2;
		}
		return fgui_run( smoke, tab );
	}
	if ( strcmp( argv[i], "-new" ) == 0 && i + 1 < argc )
	{
		return cmd_new( argc - i - 2, argv + i + 2, argv[i + 1] );
	}
	if ( strcmp( argv[i], "-info" ) == 0 && i + 1 < argc )
	{
		return cmd_info( argv[i + 1] );
	}
	if ( strcmp( argv[i], "-drives" ) == 0 )
	{
		return cmd_drives();
	}
	if ( strcmp( argv[i], "-read" ) == 0 && i + 2 < argc )
	{
		fjob_read( &job, argv[i + 1], argv[i + 2] );
		return report( &job );
	}
	if ( strcmp( argv[i], "-write" ) == 0 && i + 2 < argc )
	{
		if ( i + 3 >= argc || strcmp( argv[i + 3], "-yes" ) != 0 )
		{
			fprintf( stderr, "This replaces everything on the floppy in %s. Add -yes to go ahead.\n", argv[i + 1] );
			return 2;
		}
		fjob_write( &job, argv[i + 1], argv[i + 2] );
		return report( &job );
	}
	if ( strcmp( argv[i], "-version" ) == 0 )
	{
		printf( "floppy_image " FI_VERSION "\n" );
		return 0;
	}
	fputs( usage_text, stderr );
	return 2;
}
