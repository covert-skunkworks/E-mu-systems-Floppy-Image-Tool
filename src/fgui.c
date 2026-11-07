//  Description:
//
//		fgui.c -- the program's window, drawn with Nuklear on SDL2, after the
//		emulator's nkwin.c. Three pages: make an image, write one to a floppy,
//		read a floppy to one.
//
//		What the make page makes stays in memory (fjob_set). From there it goes
//		straight to a floppy, or to an image file if the user chooses to save
//		it, or both. No file is needed along the way.
//
//		The transfers run on a thread of their own, so the window keeps drawing
//		the progress and can offer Cancel; while one runs, everything that
//		could start another or change its inputs is disabled.
//
//		Writing asks first, in a box that names the drive and the image, and
//		its default answer (Return and Esc alike) is Cancel.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "fgui.h"
#include "fdrive.h"
#include "fimg.h"
#include "fjob.h"
#include "fplat.h"
#include "nk.h"
#include "app_icon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <SDL2/SDL_syswm.h>
#endif

//------------------------------------------------------------------------------
//------------------------ Local macros and constants --------------------------
//------------------------------------------------------------------------------

#ifndef FI_VERSION
#define FI_VERSION "dev"
#endif

#define WIN_W       860
#define WIN_H       660
#define ROW         26.0f
#define TEXT_LINE   19.0f
#define LOG_LINES   300
#define LOG_LEN     200
#define MAX_FILES   224
#define MAX_DROPS   64
#define BROWSE_MAX  4096
#define IMG_FILTER  "Floppy images|*.img;*.ima;*.flp"
#define BANK_FILTER "E-mu banks|*.e4b;*.e4r"

enum { TAB_MAKE, TAB_WRITE, TAB_READ };
enum { FMT_EOS, FMT_FAT };
enum { PICK_OPEN, PICK_SAVE, PICK_MULTI };

//------------------------------------------------------------------------------
//------------------------- Local types and structures -------------------------
//------------------------------------------------------------------------------

typedef struct
{
	char     path[FPLAT_PATH];
	uint64_t size;
} file_item;

typedef struct
{
	char name[256];
	int  dir;
} browse_item;

#ifdef FDR_TEST_DRIVE
#define MAX_CLICKS 32

typedef struct
{
	int frame;
	int x;
	int y;
} test_click;

static test_click clicks[MAX_CLICKS];
static int        nclicks;
#endif

//------------------------------------------------------------------------------
//------------------------------ Local variables -------------------------------
//------------------------------------------------------------------------------

extern const unsigned char font_ttf[];  // built in by tools/bin2c.c
extern const unsigned long font_ttf_len;

static SDL_Window        *win;
static SDL_Renderer      *ren;
static struct nk_context *ctx;
static SDL_Event          held[16];  // see frame_begin
static int                nheld;
static char              *drops[MAX_DROPS];
static int                ndrops;
static int                close_asked;

static int       tab;
static int       format;
static char      label[FIMG_LABEL_LEN + 1] = "E-MU";
static char      bank_name[FIMG_BANK_NAME_LEN + 1];
static file_item files[MAX_FILES];
static int       nfiles;
static int       file_sel = -1;
static char      make_out[FPLAT_PATH];    // the name offered when saving
static fjob_set  made;                    // the images made, in memory
static char      made_name[128];          // what they are, for the user

static fdr_drive drives[FDR_MAX_DRIVES];
static int       ndrives;
static int       drive_sel;

enum { SRC_MADE, SRC_FILE };
static int       write_src = SRC_FILE;
static int       write_disk = 1;              // which image of the set, from 1
static char      write_img[FPLAT_PATH + 16];  // "_12of34" may be added to a path
static char      write_info[1024];
static char      read_out[FPLAT_PATH];

static char      log_text[LOG_LINES][LOG_LEN];
static int       log_n;
static int       log_scroll;

static fjob         job;
static SDL_Thread  *job_thread;
static int          job_is_write;
static const uint8_t *job_mem;     // the image to write, when it is in memory
static char         job_drive[32];
static char         job_path[FPLAT_PATH];

//------------------------------------------------------------------------------
//------------------------------- Local functions ------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Add lines to the log at the bottom of the window.
  *
  * @param  text One or more lines.
  *
  * @retval None.
  */
static void log_add( const char *text )
{
	const char *p = text;

	while ( *p )
	{
		const char *e = strchr( p, '\n' );
		size_t n = e != NULL ? (size_t)( e - p ) : strlen( p );

		if ( log_n == LOG_LINES )
		{
			memmove( log_text[0], log_text[1], ( LOG_LINES - 1 ) * LOG_LEN );
			log_n--;
		}
		snprintf( log_text[log_n++], LOG_LEN, "%.*s", (int)n, p );
		p += n + ( e != NULL ? 1 : 0 );
	}
	log_scroll = 1;
}

/**
  * @brief  Whether a transfer is running.
  *
  * @retval 1 if one is, 0 if not.
  */
static int job_running( void )
{
	return job_thread != NULL;
}

/**
  * @brief  The body of the transfer's thread.
  *
  * @param  arg Unused.
  *
  * @retval 0.
  */
static int job_main( void *arg )
{
	(void)arg;
	if ( job_mem != NULL )
	{
		fjob_write_mem( &job, job_drive, job_mem, FIMG_SIZE_144 );
	}
	else if ( job_is_write )
	{
		fjob_write( &job, job_drive, job_path );
	}
	else
	{
		fjob_read( &job, job_drive, job_path );
	}
	return 0;
}

/**
  * @brief  Start a transfer on its thread.
  *
  * @param  write 1 to write the image, 0 to read the floppy.
  * @param  path The image file; ignored when mem is given.
  * @param  mem The image to write, in memory, or NULL.
  *
  * @retval None.
  */
static void job_start( int write, const char *path, const uint8_t *mem )
{
	if ( job_running() || drive_sel >= ndrives )
	{
		return;
	}
	job_is_write = write;
	job_mem = mem;
	snprintf( job_drive, sizeof job_drive, "%s", drives[drive_sel].id );
	snprintf( job_path, sizeof job_path, "%s", path );
	memset( &job, 0, sizeof job );
	job.phase = write ? FJOB_WRITING : FJOB_READING;
	job_thread = SDL_CreateThread( job_main, "floppy", NULL );
	if ( job_thread == NULL )
	{
		log_add( "Cannot start the transfer." );
	}
}

/**
  * @brief  The image next in a set, once one disk is written.
  *
  * @param  path The image just written.
  * @param  out Receives the next one's path if it exists.
  * @param  n Room in out.
  *
  * @retval 1 if there is a next one, 0 if not.
  */
static int next_in_set( const char *path, char *out, size_t n )
{
	uint8_t *d = NULL;
	size_t len = 0;
	fimg_info info;
	char of[32], *hit;
	char copy[FPLAT_PATH];
	int ok = 0;

	if ( fplat_read_file( path, &d, &len, FIMG_SIZE_144 ) != 0 )
	{
		return 0;
	}
	fimg_inspect( d, len, &info );
	free( d );
	if ( info.kind != FIMG_KIND_EOS_BANK || info.disk >= info.disks )
	{
		return 0;
	}
	snprintf( of, sizeof of, "_%dof%d", info.disk, info.disks );
	snprintf( copy, sizeof copy, "%s", path );
	hit = strstr( copy, of );
	if ( hit != NULL )
	{
		*hit = 0;
		snprintf( out, n, "%s_%dof%d%s", copy, info.disk + 1, info.disks, hit + strlen( of ) );
		ok = fplat_is_regular( out, NULL );
	}
	return ok;
}

/**
  * @brief  Refresh the description of the image on the write page.
  *
  * @retval None.
  */
static void write_info_update( void )
{
	uint8_t *d = NULL;
	size_t len = 0;
	int rc;
	fimg_info info;

	write_info[0] = 0;
	if ( write_src == SRC_MADE )
	{
		const uint8_t *img = fjob_set_disk( &made, write_disk );

		if ( img != NULL )
		{
			fimg_inspect( img, FIMG_SIZE_144, &info );
			snprintf( write_info, sizeof write_info, "%s", info.text );
		}
		return;
	}
	if ( !write_img[0] )
	{
		return;
	}
	rc = fplat_read_file( write_img, &d, &len, FIMG_SIZE_144 );
	if ( rc == -2 )
	{
		snprintf( write_info, sizeof write_info, "Too large to be a floppy image." );
		return;
	}
	if ( rc != 0 )
	{
		snprintf( write_info, sizeof write_info, "Cannot read this file." );
		return;
	}
	fimg_inspect( d, len, &info );
	free( d );
	snprintf( write_info, sizeof write_info, "%s", info.text );
}

/**
  * @brief  Collect a finished transfer and report it.
  *
  * @retval None.
  */
static void job_poll( void )
{
	int i, spt;
	char line[LOG_LEN];

	if ( job_thread == NULL || job.phase != FJOB_FINISHED )
	{
		return;
	}
	SDL_WaitThread( job_thread, NULL );
	job_thread = NULL;
	log_add( job.message );
	spt = job.total == FIMG_SECTORS_720 ? 9 : 18;
	for ( i = 0; i < job.nbad && i < 100; i++ )
	{
		char chs[40];

		fjob_chs( job.bad[i], spt, chs, sizeof chs );
		snprintf( line, sizeof line, "  sector %d, C/H/S %s", job.bad[i], chs );
		log_add( line );
	}
	if ( job.nbad > 100 )
	{
		snprintf( line, sizeof line, "  and %d more", job.nbad - 100 );
		log_add( line );
	}
	if ( !job_is_write && job.result == FJOB_OK )
	{
		snprintf( write_img, sizeof write_img, "%s", job_path );
		write_src = SRC_FILE;
		write_info_update();
		log_add( write_info );
	}
	if ( job_mem != NULL && job.result == FJOB_OK && write_disk < made.disks )
	{
		write_disk++;
		write_info_update();
		snprintf( line, sizeof line, "Next: put in a new floppy for disk %d of %d.", write_disk, made.disks );
		log_add( line );
	}
	else if ( job_mem == NULL && job_is_write && job.result == FJOB_OK )
	{
		char next[FPLAT_PATH + 16];

		if ( next_in_set( job_path, next, sizeof next ) )
		{
			snprintf( write_img, sizeof write_img, "%s", next );
			write_info_update();
			snprintf( line, sizeof line, "Next: put in a new floppy for %s.", fplat_base( next ) );
			log_add( line );
		}
	}
}

/**
  * @brief  Ask a yes-or-no question whose default answer is No.
  *
  * @param  title The box's title.
  * @param  msg The question.
  * @param  yes The label of the button that goes ahead.
  *
  * @retval 1 if the user went ahead, 0 if not.
  */
static int ask( const char *title, const char *msg, const char *yes )
{
	const SDL_MessageBoxButtonData b[2] =
	{
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
		{ 0, 1, yes }
	};
	SDL_MessageBoxData d;
	int hit = 0;

	memset( &d, 0, sizeof d );
	d.flags = SDL_MESSAGEBOX_WARNING;
	d.window = win;
	d.title = title;
	d.message = msg;
	d.numbuttons = 2;
	d.buttons = b;
	return SDL_ShowMessageBox( &d, &hit ) == 0 && hit == 1;
}

#ifdef FDR_TEST_DRIVE
/**
  * @brief  Feed the test build's scripted clicks to SDL: move, press, release.
  *
  * @param  frame The frame about to start.
  *
  * @retval None.
  */
static void test_clicks( int frame )
{
	int i;

	for ( i = 0; i < nclicks; i++ )
	{
		SDL_Event e;
		int step = frame - clicks[i].frame;

		if ( step != 0 && step != 2 && step != 4 )
		{
			continue;
		}
		memset( &e, 0, sizeof e );
		if ( step == 0 )
		{
			e.type = SDL_MOUSEMOTION;
			e.motion.x = clicks[i].x;
			e.motion.y = clicks[i].y;
		}
		else
		{
			e.type = step == 2 ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
			e.button.button = SDL_BUTTON_LEFT;
			e.button.state = step == 2 ? SDL_PRESSED : SDL_RELEASED;
			e.button.clicks = 1;
			e.button.x = clicks[i].x;
			e.button.y = clicks[i].y;
		}
		SDL_PushEvent( &e );
	}
}
#endif

/**
  * @brief  Start a frame: events in, the Nuklear window begun.
  *
  * Nuklear fires a button while the mouse button is down in that frame, so a
  * press and its release in one frame would be lost and the button would need
  * a second click. A release that follows a press of the same frame waits for
  * the next frame.
  *
  * @retval None.
  */
static void frame_begin( void )
{
	SDL_Event ev;
	int pressed = 0, holding = 0, i, w, h;

	for ( i = 0; i < ndrops; i++ )
	{
		SDL_free( drops[i] );
	}
	ndrops = 0;
	nk_input_begin( ctx );
	for ( i = 0; i < nheld; i++ )
	{
		if ( held[i].type == SDL_MOUSEBUTTONDOWN )
		{
			pressed |= 1 << ( held[i].button.button & 7 );
		}
		nk_sdl_handle_event( &held[i] );
	}
	nheld = 0;
	while ( SDL_PollEvent( &ev ) )
	{
		if ( ev.type == SDL_QUIT ||
		     ( ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE ) )
		{
			close_asked = 1;
			continue;
		}
		if ( ev.type == SDL_DROPFILE )
		{
			if ( ndrops < MAX_DROPS )
			{
				drops[ndrops++] = ev.drop.file;
			}
			else
			{
				SDL_free( ev.drop.file );
			}
			continue;
		}
		if ( ( ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP ) && nheld < 16 &&
		     ( holding || ( ev.type == SDL_MOUSEBUTTONUP && ( pressed & 1 << ( ev.button.button & 7 ) ) ) ) )
		{
			held[nheld++] = ev;
			holding = 1;
			continue;
		}
		if ( ev.type == SDL_MOUSEBUTTONDOWN )
		{
			pressed |= 1 << ( ev.button.button & 7 );
		}
		nk_sdl_handle_event( &ev );
	}
	nk_input_end( ctx );
	SDL_GetWindowSize( win, &w, &h );
	nk_begin( ctx, "main", nk_rect( 0, 0, (float)w, (float)h ), NK_WINDOW_NO_SCROLLBAR );
}

/**
  * @brief  Finish a frame and show it.
  *
  * @param  shot A BMP to save the frame into, or NULL.
  *
  * @retval 0, or -1 if the BMP could not be saved.
  */
static int frame_end( const char *shot )
{
	int rc = 0;

	nk_end( ctx );
	SDL_SetRenderDrawColor( ren, 45, 45, 45, 255 );
	SDL_RenderClear( ren );
	nk_sdl_render( NK_ANTI_ALIASING_ON );
	if ( shot != NULL )
	{
		int w, h;
		SDL_Surface *s;

		rc = -1;
		if ( SDL_GetRendererOutputSize( ren, &w, &h ) == 0 &&
		     ( s = SDL_CreateRGBSurfaceWithFormat( 0, w, h, 32, SDL_PIXELFORMAT_ARGB8888 ) ) != NULL )
		{
			if ( SDL_RenderReadPixels( ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch ) == 0 &&
			     SDL_SaveBMP( s, shot ) == 0 )
			{
				rc = 0;
			}
			SDL_FreeSurface( s );
		}
	}
	SDL_RenderPresent( ren );
	SDL_Delay( 15 );
	return rc;
}

/**
  * @brief  A row of equal columns.
  *
  * @param  cols How many.
  *
  * @retval None.
  */
static void row_eq( int cols )
{
	nk_layout_row_dynamic( ctx, ROW, cols );
}

/**
  * @brief  A row of columns in given shares.
  *
  * @param  cols How many.
  * @param  ratio Their shares.
  *
  * @retval None.
  */
static void row_ratio( int cols, const float *ratio )
{
	nk_layout_row( ctx, NK_DYNAMIC, ROW, cols, ratio );
}

/**
  * @brief  A button that can be greyed out.
  *
  * @param  text The label.
  * @param  enabled 0 draws it dead.
  *
  * @retval 1 if it was pressed.
  */
static int button( const char *text, int enabled )
{
	if ( enabled )
	{
		return nk_button_label( ctx, text );
	}
	nk_widget_disable_begin( ctx );
	nk_button_label( ctx, text );
	nk_widget_disable_end( ctx );
	return 0;
}

/**
  * @brief  A paragraph of text, in a row tall enough for it.
  *
  * @param  text The text; newlines start lines.
  * @param  width_chars Roughly how many characters fit on a line.
  *
  * @retval None.
  */
static void paragraph( const char *text, int width_chars )
{
	const char *p = text;

	while ( *p )
	{
		const char *e = strchr( p, '\n' );
		int n = e != NULL ? (int)( e - p ) : (int)strlen( p );
		char line[512];

		snprintf( line, sizeof line, "%.*s", n, p );
		nk_layout_row_dynamic( ctx, TEXT_LINE * (float)( 1 + n / width_chars ), 1 );
		nk_label_wrap( ctx, line );
		p += n + ( e != NULL ? 1 : 0 );
	}
}

#ifdef _WIN32
/**
  * @brief  The window's handle, to own a system dialog.
  *
  * @retval The handle, or NULL.
  */
static HWND owner_window( void )
{
	SDL_SysWMinfo info;

	SDL_VERSION( &info.version );
	return win != NULL && SDL_GetWindowWMInfo( win, &info ) ? info.info.win.window : NULL;
}

/**
  * @brief  Choose a file with Windows' own dialog.
  *
  * @param  title The dialog's title.
  * @param  mode PICK_OPEN, PICK_SAVE or PICK_MULTI.
  * @param  filter "Description|*.a;*.b".
  * @param  path The starting file in, the choice out (UTF-8); PICK_MULTI gives
  *         one path a line.
  * @param  n Room in path.
  *
  * @retval 0 if chosen, -1 if not.
  */
static int pick_file( const char *title, int mode, const char *filter, char *path, size_t n )
{
	static wchar_t buf[32768];
	wchar_t wflt[512], wtitle[256];
	char flt[512], desc[200], pat[200];
	const char *bar = strchr( filter, '|' );
	OPENFILENAMEW o;
	int k;

	snprintf( desc, sizeof desc, "%.*s", (int)( bar - filter ), filter );
	snprintf( pat, sizeof pat, "%s", bar + 1 );
	k = snprintf( flt, sizeof flt, "%s (%s)", desc, pat ) + 1;
	k += snprintf( flt + k, sizeof flt - (size_t)k, "%s", pat ) + 1;
	k += snprintf( flt + k, sizeof flt - (size_t)k, "All files (*.*)" ) + 1;
	k += snprintf( flt + k, sizeof flt - (size_t)k, "*.*" ) + 1;
	flt[k] = 0;
	MultiByteToWideChar( CP_UTF8, 0, flt, k + 1, wflt, 512 );
	MultiByteToWideChar( CP_UTF8, 0, title, -1, wtitle, 256 );
	buf[0] = 0;
	if ( mode != PICK_MULTI )
	{
		MultiByteToWideChar( CP_UTF8, 0, path, -1, buf, 32768 );
	}
	memset( &o, 0, sizeof o );
	o.lStructSize = sizeof o;
	o.hwndOwner = owner_window();
	o.lpstrFilter = wflt;
	o.lpstrFile = buf;
	o.nMaxFile = 32768;
	o.lpstrTitle = wtitle;
	if ( mode == PICK_SAVE )
	{
		o.lpstrDefExt = L"img";
	}
	o.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR |
	          ( mode == PICK_SAVE ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST ) |
	          ( mode == PICK_MULTI ? OFN_ALLOWMULTISELECT : 0 );
	if ( !( mode == PICK_SAVE ? GetSaveFileNameW( &o ) : GetOpenFileNameW( &o ) ) )
	{
		return -1;
	}
	if ( mode == PICK_MULTI && buf[wcslen( buf ) + 1] != 0 )
	{
		// several files: the folder, then the names, each 0-terminated
		const wchar_t *dir = buf, *p = buf + wcslen( buf ) + 1;
		size_t used = 0;

		path[0] = 0;
		for ( ; *p && used + FPLAT_PATH < n; p += wcslen( p ) + 1 )
		{
			char d[FPLAT_PATH], f[FPLAT_PATH];

			WideCharToMultiByte( CP_UTF8, 0, dir, -1, d, sizeof d, NULL, NULL );
			WideCharToMultiByte( CP_UTF8, 0, p, -1, f, sizeof f, NULL, NULL );
			used += (size_t)snprintf( path + used, n - used, "%s%s\\%s", used ? "\n" : "", d, f );
		}
		return 0;
	}
	return WideCharToMultiByte( CP_UTF8, 0, buf, -1, path, (int)n, NULL, NULL ) ? 0 : -1;
}
#else

static browse_item *br_items;
static unsigned char *br_sel;
static int br_n;
static int br_mode;
static char br_dir[FPLAT_PATH];
static char br_name[256];
static char br_exts[200];

/**
  * @brief  Whether a file's name fits the browser's filter.
  *
  * @param  name The name.
  *
  * @retval 1 if it does, 0 if not.
  */
static int br_match( const char *name )
{
	const char *p = br_exts;
	size_t len = strlen( name );

	while ( *p )
	{
		const char *q = strchr( p, ';' );
		size_t k = q != NULL ? (size_t)( q - p ) : strlen( p );

		if ( k > 1 && p[0] == '*' && len >= k - 1 )
		{
			char ext[32];

			snprintf( ext, sizeof ext, "%.*s", (int)( k - 1 ), p + 1 );
			if ( fplat_stricmp( name + len - ( k - 1 ), ext ) == 0 )
			{
				return 1;
			}
		}
		p += k + ( q != NULL ? 1 : 0 );
	}
	return 0;
}

/**
  * @brief  Take one folder entry into the browser's list.
  *
  * @param  name The entry's name.
  * @param  is_dir Whether it is a folder.
  * @param  ctx_ Unused.
  *
  * @retval None.
  */
static void br_add( const char *name, int is_dir, void *ctx_ )
{
	(void)ctx_;
	if ( name[0] == '.' || br_n >= BROWSE_MAX || strlen( name ) >= sizeof br_items[0].name ||
	     ( !is_dir && !br_match( name ) ) )
	{
		return;
	}
	snprintf( br_items[br_n].name, sizeof br_items[0].name, "%s", name );
	br_items[br_n].dir = is_dir;
	br_sel[br_n++] = 0;
}

/**
  * @brief  Folders first, then by name.
  *
  * @param  a One entry.
  * @param  b The other.
  *
  * @retval As strcmp.
  */
static int br_cmp( const void *a, const void *b )
{
	const browse_item *x = a, *y = b;

	return x->dir != y->dir ? y->dir - x->dir : fplat_stricmp( x->name, y->name );
}

/**
  * @brief  List the browser's folder.
  *
  * @retval None.
  */
static void br_list( void )
{
	size_t len = strlen( br_dir );

	if ( len && br_dir[len - 1] != '/' && len + 1 < sizeof br_dir )
	{
		br_dir[len] = '/';
		br_dir[len + 1] = 0;
	}
	br_n = 0;
	fplat_list_dir( br_dir[0] ? br_dir : ".", br_add, NULL );
	if ( br_n )
	{
		qsort( br_items, (size_t)br_n, sizeof *br_items, br_cmp );
	}
}

/**
  * @brief  Go to the folder above.
  *
  * @retval None.
  */
static void br_up( void )
{
	size_t len = strlen( br_dir );

	while ( len && br_dir[len - 1] == '/' )
	{
		br_dir[--len] = 0;
	}
	while ( len && br_dir[len - 1] != '/' )
	{
		br_dir[--len] = 0;
	}
	if ( !len )
	{
		snprintf( br_dir, sizeof br_dir, "/" );
	}
	br_list();
}

/**
  * @brief  Draw the browser for one frame.
  *
  * @param  title What is being chosen.
  *
  * @retval 1 chosen, -1 cancelled, 0 still choosing.
  */
static int br_ui( const char *title )
{
	static const float top[3] = { 0.12f, 0.73f, 0.15f };
	static const float bottom[4] = { 0.12f, 0.58f, 0.15f, 0.15f };
	const struct nk_input *in = &ctx->input;
	int w, h, i, chosen = 0;

	SDL_GetWindowSize( win, &w, &h );
	row_eq( 1 );
	nk_label( ctx, title, NK_TEXT_LEFT );
	row_ratio( 3, top );
	nk_label( ctx, "Folder:", NK_TEXT_LEFT );
	if ( nk_edit_string_zero_terminated( ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, br_dir, (int)sizeof br_dir,
	                                     nk_filter_default ) & NK_EDIT_COMMITED )
	{
		br_list();
	}
	if ( nk_button_label( ctx, "Up" ) )
	{
		br_up();
	}
	nk_layout_row_dynamic( ctx, (float)h - 3 * ROW - 60, 1 );
	if ( nk_group_begin( ctx, "files", NK_WINDOW_BORDER ) )
	{
		for ( i = 0; i < br_n; i++ )
		{
			char text[300];
			struct nk_rect b;
			int dbl;
			nk_bool on;

			nk_layout_row_dynamic( ctx, 20, 1 );
			snprintf( text, sizeof text, "%s%s", br_items[i].name, br_items[i].dir ? "/" : "" );
			b = nk_widget_bounds( ctx );
			dbl = nk_input_has_mouse_click_in_rect( in, NK_BUTTON_DOUBLE, b ) && in->mouse.buttons[NK_BUTTON_DOUBLE].clicked;
			on = br_mode == PICK_MULTI ? br_sel[i] : !br_items[i].dir && strcmp( br_items[i].name, br_name ) == 0;
			if ( nk_selectable_label( ctx, text, NK_TEXT_LEFT, &on ) || dbl )
			{
				if ( br_items[i].dir )
				{
					size_t have = strlen( br_dir ), add = strlen( br_items[i].name );

					if ( have + add < sizeof br_dir )
					{
						memcpy( br_dir + have, br_items[i].name, add + 1 );
					}
					br_list();
					break;
				}
				if ( br_mode == PICK_MULTI )
				{
					br_sel[i] = on || dbl;
				}
				else
				{
					snprintf( br_name, sizeof br_name, "%s", br_items[i].name );
				}
				chosen = dbl;
			}
		}
		nk_group_end( ctx );
	}
	row_ratio( 4, bottom );
	nk_label( ctx, br_mode == PICK_MULTI ? "" : "File:", NK_TEXT_LEFT );
	if ( br_mode == PICK_MULTI )
	{
		nk_label( ctx, "Click the files to take.", NK_TEXT_LEFT );
	}
	else
	{
		nk_edit_string_zero_terminated( ctx, NK_EDIT_FIELD, br_name, (int)sizeof br_name, nk_filter_default );
	}
	if ( nk_button_label( ctx, "OK" ) )
	{
		chosen = 1;
	}
	if ( nk_button_label( ctx, "Cancel" ) )
	{
		return -1;
	}
	return chosen;
}

/**
  * @brief  Choose a file with a browser drawn in the window.
  *
  * @param  title What is being chosen.
  * @param  mode PICK_OPEN, PICK_SAVE or PICK_MULTI.
  * @param  filter "Description|*.a;*.b".
  * @param  path The starting file in, the choice out; PICK_MULTI gives one path
  *         a line.
  * @param  n Room in path.
  *
  * @retval 0 if chosen, -1 if not.
  */
static int pick_file( const char *title, int mode, const char *filter, char *path, size_t n )
{
	const char *bar = strchr( filter, '|' );
	int rc = 0, i;
	char full[FPLAT_PATH];

	br_items = calloc( BROWSE_MAX, sizeof *br_items );
	br_sel = calloc( BROWSE_MAX, 1 );
	if ( br_items == NULL || br_sel == NULL )
	{
		free( br_items );
		free( br_sel );
		return -1;
	}
	br_mode = mode;
	br_name[0] = 0;
	snprintf( br_exts, sizeof br_exts, "%s", bar + 1 );
	snprintf( br_dir, sizeof br_dir, "." );
	if ( mode != PICK_MULTI && path[0] )
	{
		char *base;

		snprintf( br_dir, sizeof br_dir, "%s", path );
		base = (char *)fplat_base( br_dir );
		snprintf( br_name, sizeof br_name, "%s", base );
		*base = 0;
		if ( !br_dir[0] )
		{
			snprintf( br_dir, sizeof br_dir, "." );
		}
	}
	if ( fplat_full_path( br_dir, full, sizeof full ) == 0 )
	{
		snprintf( br_dir, sizeof br_dir, "%s", full );
	}
	br_list();
	while ( rc == 0 && !close_asked )
	{
		frame_begin();
		rc = br_ui( title );
		frame_end( NULL );
		if ( rc > 0 && mode != PICK_MULTI && !br_name[0] )
		{
			rc = 0;
		}
	}
	if ( rc > 0 )
	{
		if ( mode == PICK_MULTI )
		{
			size_t used = 0;

			path[0] = 0;
			for ( i = 0; i < br_n && used + FPLAT_PATH < n; i++ )
			{
				if ( br_sel[i] )
				{
					used += (size_t)snprintf( path + used, n - used, "%s%s%s", used ? "\n" : "", br_dir, br_items[i].name );
				}
			}
			rc = used ? 1 : -1;
		}
		else
		{
			snprintf( path, n, "%s%s", br_dir, br_name );
		}
	}
	free( br_items );
	free( br_sel );
	return rc > 0 ? 0 : -1;
}
#endif

/**
  * @brief  Add a file to the make page's list.
  *
  * On an EOS floppy the file replaces what is there, one bank being all that
  * format holds.
  *
  * @param  path The file.
  *
  * @retval None.
  */
static void make_add_file( const char *path )
{
	uint64_t size = 0;
	char msg[FPLAT_PATH + 64];

	if ( !fplat_is_regular( path, &size ) )
	{
		snprintf( msg, sizeof msg, "Not a file: %s", path );
		log_add( msg );
		return;
	}
	if ( format == FMT_EOS )
	{
		nfiles = 0;
		fimg_bank_name( path, bank_name );
	}
	if ( nfiles >= MAX_FILES )
	{
		log_add( "The floppy's directory holds 224 files." );
		return;
	}
	snprintf( files[nfiles].path, sizeof files[nfiles].path, "%s", path );
	files[nfiles].size = size;
	nfiles++;
	if ( !make_out[0] )
	{
		char *dot;

		snprintf( make_out, sizeof make_out, "%s", path );
		dot = strrchr( (char *)fplat_base( make_out ), '.' );
		if ( dot != NULL && (size_t)( dot - make_out ) + 5 < sizeof make_out )
		{
			memcpy( dot, ".img", 5 );
		}
	}
}

/**
  * @brief  Make the image(s) the make page describes, in memory.
  *
  * @retval 1 if made, 0 if not.
  */
static int make_build( void )
{
	static char msg[4096];
	int rc;

	if ( format == FMT_EOS )
	{
		rc = fjob_set_eos( &made, nfiles ? files[0].path : NULL, bank_name, msg, sizeof msg );
	}
	else
	{
		const char *paths[MAX_FILES];
		int i;

		for ( i = 0; i < nfiles; i++ )
		{
			paths[i] = files[i].path;
		}
		rc = fjob_set_dos( &made, paths, nfiles, label, msg, sizeof msg );
	}
	log_add( msg );
	if ( rc <= 0 )
	{
		made_name[0] = 0;
		return 0;
	}
	if ( format == FMT_EOS )
	{
		snprintf( made_name, sizeof made_name, "%s", nfiles ? bank_name : "Blank EOS floppy" );
	}
	else
	{
		snprintf( made_name, sizeof made_name, "DOS floppy \"%s\", %d file%s", label, nfiles, nfiles == 1 ? "" : "s" );
	}
	return 1;
}

/**
  * @brief  The Save button: make the images and put them in a file.
  *
  * What was made stays in memory, so it can go to a floppy as well.
  *
  * @retval None.
  */
static void make_save( void )
{
	char msg[FPLAT_PATH + 64];

	if ( pick_file( "Save the image as", PICK_SAVE, IMG_FILTER, make_out, sizeof make_out ) != 0 )
	{
		return;
	}
#ifndef _WIN32
	// Windows' own dialog has asked already
	if ( fplat_is_regular( make_out, NULL ) )
	{
		char q[FPLAT_PATH + 64];

		snprintf( q, sizeof q, "%s exists. Replace it?", make_out );
		if ( !ask( "Save image", q, "Replace" ) )
		{
			return;
		}
	}
#endif
	if ( !make_build() )
	{
		return;
	}
	fjob_set_save( &made, make_out, msg, sizeof msg );
	log_add( msg );
	write_src = SRC_MADE;
	write_disk = 1;
	write_info_update();
}

/**
  * @brief  The Write button: make the images and go to the write page with them.
  *
  * @retval None.
  */
static void make_write( void )
{
	if ( !make_build() )
	{
		return;
	}
	write_src = SRC_MADE;
	write_disk = 1;
	write_info_update();
	ndrives = fdr_list( drives );
	drive_sel = 0;
	tab = TAB_WRITE;
}

/**
  * @brief  Draw the file list and the capacity line of the make page.
  *
  * @param  list_h The list's height.
  *
  * @retval None.
  */
static void make_files_ui( float list_h )
{
	static const float bar[4] = { 0.22f, 0.14f, 0.14f, 0.50f };
	char line[FPLAT_PATH + 64];
	int i;

	nk_layout_row_dynamic( ctx, list_h, 1 );
	if ( nk_group_begin( ctx, "make_files", NK_WINDOW_BORDER ) )
	{
		if ( nfiles == 0 )
		{
			nk_layout_row_dynamic( ctx, 20, 1 );
			nk_label( ctx, format == FMT_EOS ? "No bank chosen: a blank floppy image. Drop a bank here, or Add." :
			                                   "No files: an empty floppy image. Drop files here, or Add.", NK_TEXT_LEFT );
		}
		for ( i = 0; i < nfiles; i++ )
		{
			nk_bool on = i == file_sel;

			nk_layout_row_dynamic( ctx, 20, 1 );
			if ( format == FMT_FAT )
			{
				uint8_t sfn[11];
				char text[13] = "";
				int k, t = 0;

				fimg_short_name( fplat_base( files[i].path ), sfn );
				for ( k = 0; k < 11; k++ )
				{
					if ( k == 8 && sfn[8] != ' ' )
					{
						text[t++] = '.';
					}
					if ( sfn[k] != ' ' )
					{
						text[t++] = (char)sfn[k];
					}
				}
				text[t] = 0;
				snprintf( line, sizeof line, "B%03d  %-12s  %s  (%lu KB)", i, text, fplat_base( files[i].path ),
				          (unsigned long)( ( files[i].size + 1023 ) / 1024 ) );
			}
			else
			{
				snprintf( line, sizeof line, "%s  (%lu KB)", fplat_base( files[i].path ),
				          (unsigned long)( ( files[i].size + 1023 ) / 1024 ) );
			}
			if ( nk_selectable_label( ctx, line, NK_TEXT_LEFT, &on ) )
			{
				file_sel = on ? i : -1;
			}
		}
		nk_group_end( ctx );
	}

	row_ratio( 4, bar );
	if ( button( format == FMT_EOS ? "Choose bank..." : "Add files...", !job_running() ) )
	{
		static char picked[FPLAT_PATH * 32];

		picked[0] = 0;
		if ( pick_file( format == FMT_EOS ? "Bank" : "Files", format == FMT_EOS ? PICK_OPEN : PICK_MULTI,
		                format == FMT_EOS ? BANK_FILTER : BANK_FILTER ";*.wav;*.aif;*.aiff;*.mid", picked, sizeof picked ) == 0 )
		{
			char *p = picked;

			while ( p != NULL && *p )
			{
				char *e = strchr( p, '\n' );

				if ( e != NULL )
				{
					*e = 0;
				}
				make_add_file( p );
				p = e != NULL ? e + 1 : NULL;
			}
		}
	}
	if ( button( "Remove", file_sel >= 0 && file_sel < nfiles ) )
	{
		memmove( &files[file_sel], &files[file_sel + 1], (size_t)( nfiles - file_sel - 1 ) * sizeof files[0] );
		nfiles--;
		file_sel = -1;
	}
	if ( button( "Clear", nfiles > 0 ) )
	{
		nfiles = 0;
		file_sel = -1;
	}
	if ( format == FMT_EOS )
	{
		if ( nfiles )
		{
			int d = fimg_eos_disks( (size_t)files[0].size );

			if ( d < 0 )
			{
				snprintf( line, sizeof line, "Too large: more than %d floppies.", FIMG_EOS_MAX_DISKS );
			}
			else
			{
				snprintf( line, sizeof line, "%lu KB: %d floppy image%s", (unsigned long)( ( files[0].size + 1023 ) / 1024 ), d, d == 1 ? "" : "s" );
			}
		}
		else
		{
			snprintf( line, sizeof line, "One floppy holds %d KB of bank.", FIMG_EOS_DISK_BYTES / 1024 );
		}
	}
	else
	{
		uint32_t used = 0;

		for ( i = 0; i < nfiles; i++ )
		{
			used += fimg_fat_sectors( (size_t)files[i].size );
		}
		snprintf( line, sizeof line, "%u of %d KB used%s", (unsigned)( used / 2 ), FIMG_FAT_DATA_SECTORS / 2,
		          used > FIMG_FAT_DATA_SECTORS ? ": TOO MUCH" : "" );
	}
	nk_label( ctx, line, NK_TEXT_RIGHT );
	if ( format == FMT_FAT && nfiles > 50 )
	{
		row_eq( 1 );
		nk_label( ctx, "EOS 2.51a shows only the first 50 files; 2.80f and later showed all 70 tested.", NK_TEXT_LEFT );
	}
}

/**
  * @brief  Draw the make page.
  *
  * @param  h The page's height.
  *
  * @retval None.
  */
static void make_ui( float h )
{
	static const float fmt_row[3] = { 0.14f, 0.36f, 0.50f };
	static const float name_row[2] = { 0.14f, 0.36f };
	static const float act_row[3] = { 0.40f, 0.30f, 0.30f };
	static const char *const names[2] = { "EOS floppy (one bank)", "DOS floppy (several files)" };
	int f;
	float list_h;

	row_ratio( 3, fmt_row );
	nk_label( ctx, "Format:", NK_TEXT_LEFT );
	f = nk_combo( ctx, names, 2, format, (int)ROW, nk_vec2( 300, 90 ) );
	if ( f != format )
	{
		format = f;
		if ( format == FMT_EOS && nfiles > 1 )
		{
			nfiles = 1;
		}
		file_sel = -1;
	}
	nk_label( ctx, "1.44 MB", NK_TEXT_LEFT );
	paragraph( format == FMT_EOS ?
	           "EOS's own format: no file system, the bank fills the floppy and runs on to as many floppies as it needs. "
	           "Loads on every EOS tested, 2.51a to 4.70. A PC cannot read these floppies." :
	           "An ordinary DOS floppy, also readable on a PC, for small banks; the list's order is the order of EOS's "
	           "bank numbers. Loads on every EOS tested, 2.51a to 4.70; names show as 8.3.", 100 );

	nk_layout_row( ctx, NK_DYNAMIC, ROW, 2, name_row );
	if ( format == FMT_EOS )
	{
		nk_label( ctx, "Bank name:", NK_TEXT_LEFT );
		nk_edit_string_zero_terminated( ctx, NK_EDIT_FIELD, bank_name, (int)sizeof bank_name, nk_filter_ascii );
	}
	else
	{
		nk_label( ctx, "Label:", NK_TEXT_LEFT );
		nk_edit_string_zero_terminated( ctx, NK_EDIT_FIELD, label, (int)sizeof label, nk_filter_ascii );
	}

	list_h = h - 5 * ROW - 2 * TEXT_LINE - 56;
	make_files_ui( list_h < 60 ? 60 : list_h );

	row_ratio( 3, act_row );
	nk_label( ctx, "Made in memory; a file only if you save one.", NK_TEXT_LEFT );
	if ( button( "Save as image file...", !job_running() ) )
	{
		make_save();
	}
	if ( button( "Write to floppy...", !job_running() ) )
	{
		make_write();
	}
}

/**
  * @brief  Draw the drive selector shared by the write and read pages.
  *
  * @retval 1 if a drive can be used, 0 if not.
  */
static int drive_ui( void )
{
	static const float r[3] = { 0.14f, 0.54f, 0.32f };
	const char *names[FDR_MAX_DRIVES];
	int i;

	for ( i = 0; i < ndrives; i++ )
	{
		names[i] = drives[i].label;
	}
	row_ratio( 3, r );
	nk_label( ctx, "Drive:", NK_TEXT_LEFT );
	if ( ndrives > 0 )
	{
		drive_sel = nk_combo( ctx, names, ndrives, drive_sel, (int)ROW, nk_vec2( 400, 140 ) );
	}
	else
	{
		nk_label( ctx, "No floppy drive found", NK_TEXT_LEFT );
	}
	if ( button( "Look again", !job_running() ) )
	{
		ndrives = fdr_list( drives );
		drive_sel = 0;
	}
	if ( !fdr_supported() && ndrives == 0 )
	{
		paragraph( "This system has no raw floppy access in this program: make images here, then write them "
		           "on Windows or Linux.", 100 );
	}
	else if ( ndrives == 0 )
	{
		paragraph( "Only real floppy drives are offered here, never hard disks, USB sticks or memory cards.", 100 );
	}
	return ndrives > 0;
}

/**
  * @brief  Draw a running transfer's progress and its Cancel button.
  *
  * @retval None.
  */
static void progress_ui( void )
{
	static const float r[2] = { 0.82f, 0.18f };
	nk_size cur;
	char text[128] = "";
	int phase = job.phase, total = job.total, done = job.done;

	if ( job_running() )
	{
		snprintf( text, sizeof text, "%s: %d of %d sectors",
		          phase == FJOB_WRITING ? "Writing" : phase == FJOB_VERIFYING ? "Verifying" : "Reading",
		          done, total );
	}
	row_eq( 1 );
	nk_label( ctx, text, NK_TEXT_LEFT );
	row_ratio( 2, r );
	cur = total > 0 && job_running() ? (nk_size)done : 0;
	nk_progress( ctx, &cur, total > 0 ? (nk_size)total : 1, NK_FIXED );
	if ( button( "Cancel", job_running() ) )
	{
		job.cancel = 1;
	}
}

/**
  * @brief  Draw the write page's choice of what to write.
  *
  * @retval 1 if there is something to write, 0 if not.
  */
static int write_source_ui( void )
{
	static const float src_row[2] = { 0.14f, 0.86f };
	static const float file_row[3] = { 0.14f, 0.68f, 0.18f };
	static const float disk_row[3] = { 0.14f, 0.20f, 0.66f };
	char made_text[160];
	const char *names[2];
	int s;

	snprintf( made_text, sizeof made_text, "Made in memory: %s", made.disks ? made_name : "nothing yet" );
	names[SRC_MADE] = made_text;
	names[SRC_FILE] = "An image file";
	row_ratio( 2, src_row );
	nk_label( ctx, "Write:", NK_TEXT_LEFT );
	s = nk_combo( ctx, names, 2, write_src, (int)ROW, nk_vec2( 500, 90 ) );
	if ( s != write_src && !job_running() )
	{
		write_src = s;
		write_info_update();
	}
	if ( write_src == SRC_MADE )
	{
		if ( made.disks > 1 )
		{
			char of[32];
			int d;

			row_ratio( 3, disk_row );
			nk_label( ctx, "Disk:", NK_TEXT_LEFT );
			d = nk_propertyi( ctx, "#", 1, write_disk, made.disks, 1, 0.2f );
			if ( d != write_disk && !job_running() )
			{
				write_disk = d;
				write_info_update();
			}
			snprintf( of, sizeof of, "of %d", made.disks );
			nk_label( ctx, of, NK_TEXT_LEFT );
		}
		return made.disks > 0;
	}
	row_ratio( 3, file_row );
	nk_label( ctx, "Image:", NK_TEXT_LEFT );
	if ( nk_edit_string_zero_terminated( ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, write_img, (int)sizeof write_img,
	                                     nk_filter_default ) & NK_EDIT_COMMITED )
	{
		write_info_update();
	}
	if ( button( "Browse...", !job_running() ) )
	{
		if ( pick_file( "Image to write", PICK_OPEN, IMG_FILTER, write_img, sizeof write_img ) == 0 )
		{
			write_info_update();
		}
	}
	return write_img[0] != 0;
}

/**
  * @brief  Ask, then start the write.
  *
  * @retval None.
  */
static void write_go( void )
{
	char q[FPLAT_PATH * 2 + 1536], what[FPLAT_PATH + 64], line[LOG_LEN];

	write_info_update();
	if ( write_src == SRC_MADE )
	{
		snprintf( what, sizeof what, "%s (disk %d of %d, made in memory)", made_name, write_disk, made.disks );
	}
	else
	{
		snprintf( what, sizeof what, "%s", write_img );
	}
	snprintf( q, sizeof q, "Write this image to the floppy in %s?\n\nImage: %s\n%s\n\n"
	          "Everything on the floppy in %s will be replaced.",
	          drives[drive_sel].label, what, write_info, drives[drive_sel].label );
	if ( !ask( "Write to floppy", q, "Write" ) )
	{
		return;
	}
	snprintf( line, sizeof line, "Writing %.80s to %.40s", write_src == SRC_MADE ? made_name : fplat_base( write_img ),
	          drives[drive_sel].label );
	log_add( line );
	job_start( 1, write_img, write_src == SRC_MADE ? fjob_set_disk( &made, write_disk ) : NULL );
}

/**
  * @brief  Draw the write page.
  *
  * @retval None.
  */
static void write_ui( void )
{
	int ok = drive_ui();
	int have = write_source_ui();

	paragraph( write_info[0] ? write_info : "Make an image on the first page, or choose a file, to see what it holds.", 100 );
	paragraph( "Writing replaces everything on the floppy, sector by sector, then reads it all back and compares. "
	           "The floppy has to be formatted already, on the sampler or on the PC.", 100 );

	progress_ui();
	row_eq( 3 );
	nk_spacing( ctx, 2 );
	if ( button( "Write and verify...", ok && have && !job_running() ) )
	{
		write_go();
	}
}

/**
  * @brief  Draw the read page.
  *
  * @retval None.
  */
static void read_ui( void )
{
	static const float r[3] = { 0.14f, 0.68f, 0.18f };
	int ok = drive_ui();

	row_ratio( 3, r );
	nk_label( ctx, "Image file:", NK_TEXT_LEFT );
	nk_edit_string_zero_terminated( ctx, NK_EDIT_FIELD, read_out, (int)sizeof read_out, nk_filter_default );
	if ( button( "Browse...", !job_running() ) )
	{
		pick_file( "Save the floppy as", PICK_SAVE, IMG_FILTER, read_out, sizeof read_out );
	}
	paragraph( "The drive is opened read-only. A sector that stays unreadable after retries is saved as zeros and "
	           "reported below; the image is written anyway, so what can be saved is saved.", 100 );

	progress_ui();
	row_eq( 3 );
	nk_spacing( ctx, 2 );
	if ( button( "Read floppy", ok && read_out[0] && !job_running() ) )
	{
		int go = 1;

		if ( fplat_is_regular( read_out, NULL ) )
		{
			char q[FPLAT_PATH + 64];

			snprintf( q, sizeof q, "%s exists. Replace it?", read_out );
			go = ask( "Read floppy", q, "Replace" );
		}
		if ( go )
		{
			char line[LOG_LEN];

			snprintf( line, sizeof line, "Reading %s to %s", drives[drive_sel].label, fplat_base( read_out ) );
			log_add( line );
			job_start( 0, read_out, NULL );
		}
	}
}

/**
  * @brief  Draw the log.
  *
  * @param  h Its height.
  *
  * @retval None.
  */
static void log_ui( float h )
{
	int i;

	nk_layout_row_dynamic( ctx, h, 1 );
	if ( log_scroll )
	{
		nk_group_set_scroll( ctx, "log", 0, (nk_uint)( log_n * 22 ) );
		log_scroll = 0;
	}
	if ( nk_group_begin( ctx, "log", NK_WINDOW_BORDER ) )
	{
		for ( i = 0; i < log_n; i++ )
		{
			nk_layout_row_dynamic( ctx, 18, 1 );
			nk_label( ctx, log_text[i], NK_TEXT_LEFT );
		}
		nk_group_end( ctx );
	}
}

/**
  * @brief  Take the files dropped on the window this frame.
  *
  * @retval None.
  */
static void drops_take( void )
{
	int i;

	for ( i = 0; i < ndrops && !job_running(); i++ )
	{
		if ( tab == TAB_MAKE )
		{
			make_add_file( drops[i] );
		}
		else if ( tab == TAB_WRITE )
		{
			snprintf( write_img, sizeof write_img, "%s", drops[i] );
			write_src = SRC_FILE;
			write_info_update();
		}
	}
}

/**
  * @brief  Draw the whole window for one frame.
  *
  * @retval None.
  */
static void ui( void )
{
	static const char *const tabs[3] = { "Make image", "Write to floppy", "Read floppy" };
	int i, w, h;
	float page_h;

	SDL_GetWindowSize( win, &w, &h );
	page_h = (float)h - ROW - 150 - 30;
	row_eq( 3 );
	for ( i = 0; i < 3; i++ )
	{
		nk_bool on = tab == i;

		if ( nk_selectable_label( ctx, tabs[i], NK_TEXT_CENTERED, &on ) && on && !job_running() )
		{
			tab = i;
		}
	}
	nk_layout_row_dynamic( ctx, page_h, 1 );
	if ( nk_group_begin( ctx, "page", NK_WINDOW_NO_SCROLLBAR ) )
	{
		switch ( tab )
		{
		case TAB_WRITE:
			write_ui();
			break;
		case TAB_READ:
			read_ui();
			break;
		default:
			make_ui( page_h );
			break;
		}
		nk_group_end( ctx );
	}
	log_ui( 150 );
}

/**
  * @brief  Set the theme: the emulator's, brighter than Nuklear's default.
  *
  * @retval None.
  */
static void style_set( void )
{
	const struct nk_color t = nk_rgb( 230, 230, 230 ), hi = nk_rgb( 255, 255, 255 );

	ctx->style.window.padding = nk_vec2( 10, 8 );
	ctx->style.window.group_padding = nk_vec2( 10, 8 );
	ctx->style.button.normal = nk_style_item_color( nk_rgb( 88, 88, 88 ) );
	ctx->style.button.hover = nk_style_item_color( nk_rgb( 108, 108, 108 ) );
	ctx->style.button.active = nk_style_item_color( nk_rgb( 128, 128, 128 ) );
	ctx->style.button.border_color = nk_rgb( 120, 120, 120 );
	ctx->style.button.text_normal = nk_rgb( 235, 235, 235 );
	ctx->style.button.text_hover = hi;
	ctx->style.button.text_active = hi;
	ctx->style.text.color = t;
	ctx->style.selectable.text_normal = t;
	ctx->style.selectable.text_hover = hi;
	// the chosen page and list entry: the default's dark grey on grey is hard to see
	ctx->style.selectable.normal_active = nk_style_item_color( nk_rgb( 88, 88, 88 ) );
	ctx->style.selectable.hover_active = nk_style_item_color( nk_rgb( 108, 108, 108 ) );
	ctx->style.selectable.pressed_active = nk_style_item_color( nk_rgb( 108, 108, 108 ) );
	ctx->style.selectable.text_normal_active = hi;
	ctx->style.selectable.text_hover_active = hi;
	ctx->style.selectable.text_pressed_active = hi;
	ctx->style.combo.label_normal = t;
	ctx->style.combo.label_hover = hi;
	ctx->style.combo.label_active = hi;
	ctx->style.contextual_button.text_normal = t;
	ctx->style.contextual_button.text_hover = hi;
	ctx->style.edit.text_normal = t;
	ctx->style.edit.text_hover = hi;
	ctx->style.edit.text_active = hi;
	ctx->style.progress.cursor_normal = nk_style_item_color( nk_rgb( 255, 176, 32 ) );
	ctx->style.progress.cursor_hover = nk_style_item_color( nk_rgb( 255, 200, 90 ) );
	ctx->style.progress.cursor_active = nk_style_item_color( nk_rgb( 255, 200, 90 ) );
}

/**
  * @brief  Open the window.
  *
  * @retval 1 if open, 0 if not.
  */
static int window_open( void )
{
	struct nk_font_atlas *atlas;
	struct nk_font *font;
	SDL_Surface *icon;

	if ( SDL_Init( SDL_INIT_VIDEO ) != 0 )
	{
		return 0;
	}
	win = SDL_CreateWindow( "E-mu floppy images " FI_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                        WIN_W, WIN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE );
	if ( win == NULL )
	{
		return 0;
	}
	icon = SDL_CreateRGBSurfaceWithFormatFrom( (void *)APP_ICON_RGBA, 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32 );
	if ( icon != NULL )
	{
		SDL_SetWindowIcon( win, icon );
		SDL_FreeSurface( icon );
	}
	SDL_SetWindowMinimumSize( win, WIN_W * 4 / 5, WIN_H * 4 / 5 );
	ren = SDL_CreateRenderer( win, -1, SDL_RENDERER_ACCELERATED );
	if ( ren == NULL )
	{
		ren = SDL_CreateRenderer( win, -1, SDL_RENDERER_SOFTWARE );
	}
	if ( ren == NULL )
	{
		return 0;
	}
	ctx = nk_sdl_init( win, ren );
	nk_sdl_font_stash_begin( &atlas );
	font = nk_font_atlas_add_from_memory( atlas, (void *)font_ttf, (nk_size)font_ttf_len, 16.0f, NULL );
	nk_sdl_font_stash_end();
	if ( font != NULL )
	{
		nk_style_set_font( ctx, &font->handle );
	}
	style_set();
	SDL_EventState( SDL_DROPFILE, SDL_ENABLE );
	return 1;
}

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

void fgui_preset( int dos, const char *file, const char *image, const char *out )
{
	format = dos ? FMT_FAT : FMT_EOS;
	if ( out != NULL )
	{
		snprintf( make_out, sizeof make_out, "%s", out );
		snprintf( read_out, sizeof read_out, "%s", out );
	}
	if ( file != NULL )
	{
		make_add_file( file );
	}
	if ( image != NULL )
	{
		snprintf( write_img, sizeof write_img, "%s", image );
		write_src = SRC_FILE;
		write_info_update();
	}
}

#ifdef FDR_TEST_DRIVE
void fgui_test_click( int frame, int x, int y )
{
	if ( nclicks < MAX_CLICKS )
	{
		clicks[nclicks].frame = frame;
		clicks[nclicks].x = x;
		clicks[nclicks].y = y;
		nclicks++;
	}
}
#endif

int fgui_run( const char *smoke, int start_tab )
{
	int frames = 0, rc = 0;

	if ( !window_open() )
	{
		fprintf( stderr, "floppy_image: cannot open the window: %s\n", SDL_GetError() );
		return 1;
	}
	tab = start_tab;
	ndrives = fdr_list( drives );
	log_add( "Ready." );

	for ( ;; )
	{
		const char *shot = smoke != NULL && frames == 4 ? smoke : NULL;

		job_poll();
#ifdef FDR_TEST_DRIVE
		test_clicks( frames );
#endif
		frames++;
		frame_begin();
		drops_take();
		ui();
		if ( frame_end( shot ) != 0 )
		{
			rc = 1;
		}
		if ( shot != NULL )
		{
			break;
		}
		if ( close_asked )
		{
			close_asked = 0;
			if ( !job_running() )
			{
				break;
			}
			if ( ask( "Quit", "A transfer is running. Stop it? A floppy being written will be incomplete.", "Stop" ) )
			{
				job.cancel = 1;
				while ( job.phase != FJOB_FINISHED )
				{
					SDL_Delay( 20 );
				}
				SDL_WaitThread( job_thread, NULL );
				job_thread = NULL;
				break;
			}
		}
	}
	for ( frames = 0; frames < ndrops; frames++ )
	{
		SDL_free( drops[frames] );
	}
	fjob_set_free( &made );
	nk_sdl_shutdown();
	SDL_DestroyRenderer( ren );
	SDL_DestroyWindow( win );
	SDL_Quit();
	return rc;
}
