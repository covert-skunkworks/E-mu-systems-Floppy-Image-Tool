//  Description:
//
//		test_core.c -- unit tests of the image making and of the transfers, on
//		the stand-in drive. No SDL and no real drive: CI runs these on every
//		system.
//
//		The expected header bytes are those of floppies EOS 4.62 wrote in the
//		emulator (a 14.6 MB bank saved over ten floppies, and a small one).
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "../src/fdrive.h"
#include "../src/fimg.h"
#include "../src/fjob.h"
#include "../src/fplat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

//------------------------------------------------------------------------------
//------------------------ Local macros and constants --------------------------
//------------------------------------------------------------------------------

#define CHECK( c ) check( ( c ) != 0, #c, __LINE__ )

#define BANK_LEN       1000
#define LISTED_ENTRIES 5

//------------------------------------------------------------------------------
//------------------------------ Local variables -------------------------------
//------------------------------------------------------------------------------

static int failures;
static int checks;
static const char *folder;
static uint8_t img[FIMG_SIZE_144];
static uint8_t img2[FIMG_SIZE_144];
static fjob job;

//------------------------------------------------------------------------------
//------------------------------- Local functions ------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Count a check and report a failed one.
  *
  * @param  ok The result.
  * @param  what The condition's text.
  * @param  line Where.
  *
  * @retval None.
  */
static void check( int ok, const char *what, int line )
{
	checks++;
	if ( !ok )
	{
		failures++;
		printf( "FAILED line %d: %s\n", line, what );
	}
}

/**
  * @brief  A path in the test's folder.
  *
  * @param  name The file.
  *
  * @retval The path, in a static buffer.
  */
static const char *tmp( const char *name )
{
	static char p[FPLAT_PATH];

	snprintf( p, sizeof p, "%s/%s", folder, name );
	return p;
}

/**
  * @brief  Make a bank-shaped buffer: FORM, then counting bytes.
  *
  * @param  len Its size.
  *
  * @retval The buffer, which the caller frees.
  */
static uint8_t *make_bank( size_t len )
{
	uint8_t *b = malloc( len );
	size_t i;

	for ( i = 0; i < len; i++ )
	{
		b[i] = (uint8_t)( i * 7 + ( i >> 9 ) );
	}
	memcpy( b, "FORM", 4 );
	b[4] = (uint8_t)( ( len - 12 ) >> 24 );
	b[5] = (uint8_t)( ( len - 12 ) >> 16 );
	b[6] = (uint8_t)( ( len - 12 ) >> 8 );
	b[7] = (uint8_t)( len - 12 );
	memcpy( b + 8, "E4B0", 4 );
	return b;
}

/**
  * @brief  The bank name made from file names.
  *
  * @retval None.
  */
static void test_names( void )
{
	char n[FIMG_BANK_NAME_LEN + 1];
	uint8_t s[11];

	fimg_bank_name( "C:\\x\\B.005-Tmp.E4R", n );
	CHECK( strcmp( n, "Tmp" ) == 0 );
	fimg_bank_name( "a/Ultra E-Synth.E4B", n );
	CHECK( strcmp( n, "Ultra E-Synth" ) == 0 );
	fimg_bank_name( "A very long bank name indeed.e4b", n );
	CHECK( strcmp( n, "A very long bank" ) == 0 );
	fimg_short_name( "ice.e4b", s );
	CHECK( memcmp( s, "ICE     E4B", 11 ) == 0 );
	fimg_short_name( "Ultra Sessions X.E4R", s );
	CHECK( memcmp( s, "ULTRASESE4R", 11 ) == 0 );
	fimg_short_name( "B.003-My+Bank.e4b", s );
	CHECK( memcmp( s, "B003-MY_E4B", 11 ) == 0 );
}

/**
  * @brief  EOS's own format, one disk and a set.
  *
  * @retval None.
  */
static void test_eos( void )
{
	size_t big = (size_t)FIMG_EOS_DISK_BYTES * 2 + 1000;
	uint8_t *bank = make_bank( big );
	uint8_t *small = make_bank( 102402 );
	fimg_info info;

	fimg_eos_blank( img );
	CHECK( img[0] == 0x55 && img[FIMG_SIZE_144 - 1] == 0x55 );
	fimg_inspect( img, FIMG_SIZE_144, &info );
	CHECK( info.kind == FIMG_KIND_EOS_BLANK );

	// The fields as EOS writes them: 102402 bytes, 201 sectors, one disk.
	CHECK( fimg_eos_disks( 102402 ) == 1 );
	CHECK( fimg_eos_disk( img, "ICE", small, 102402, 1 ) == 0 );
	CHECK( memcmp( img, "Emulator 4 FdiskICE             ", 32 ) == 0 );
	CHECK( img[0x27] == 1 && img[0x2B] == 1 && img[0x2F] == 1 );
	CHECK( img[0x31] == 0x01 && img[0x32] == 0x90 && img[0x33] == 0x02 );
	CHECK( memcmp( img + 0x34, "E4B0", 4 ) == 0 );
	CHECK( img[0x3E] == 0 && img[0x3F] == 201 && img[0x4B] == 201 );
	CHECK( memcmp( img + 512, small, 102402 ) == 0 );
	CHECK( img[512 + 102402] == 0x55 );

	CHECK( fimg_eos_disks( FIMG_EOS_DISK_BYTES ) == 1 );
	CHECK( fimg_eos_disks( FIMG_EOS_DISK_BYTES + 1 ) == 2 );
	CHECK( fimg_eos_disks( 0 ) == -1 );
	CHECK( fimg_eos_disks( (size_t)FIMG_EOS_DISK_BYTES * 99 + 1 ) == -1 );
	CHECK( fimg_eos_disks( big ) == 3 );
	CHECK( fimg_eos_disk( img, "x", bank, big, 4 ) == -2 );
	CHECK( fimg_eos_disk( img, "x", (const uint8_t *)"not a bank at all", 17, 1 ) == -1 );

	// A set: the disks hold the file end to end, and each says which it is.
	CHECK( fimg_eos_disk( img, "Set", bank, big, 2 ) == 0 );
	CHECK( img[0x2B] == 2 && img[0x2F] == 3 );
	CHECK( memcmp( img + 512, bank + FIMG_EOS_DISK_BYTES, FIMG_EOS_DISK_BYTES ) == 0 );
	fimg_inspect( img, FIMG_SIZE_144, &info );
	CHECK( info.kind == FIMG_KIND_EOS_BANK && info.disk == 2 && info.disks == 3 );
	CHECK( strcmp( info.name, "Set" ) == 0 && info.bank_bytes == big );
	CHECK( fimg_eos_disk( img, "Set", bank, big, 3 ) == 0 );
	CHECK( memcmp( img + 512, bank + 2 * (size_t)FIMG_EOS_DISK_BYTES, 1000 ) == 0 );
	CHECK( img[512 + 1000] == 0x55 );

	// A long bank name is cut, not spilled into the next field.
	CHECK( fimg_eos_disk( img, "Twenty characters ok", small, 102402, 1 ) == 0 );
	CHECK( memcmp( img + 0x10, "Twenty character", 16 ) == 0 && img[0x20] == 0 );

	free( bank );
	free( small );
}

/**
  * @brief  The samples' byte order on a floppy.
  *
  * @retval None.
  */
static void test_order( void )
{
	enum { DATA = 92, WORDS = 2000, LEN = 12 + 8 + 16 + 8 + DATA + 2 * WORDS + 7 };
	uint8_t *b = calloc( 1, LEN );
	uint8_t *orig = malloc( LEN );
	size_t chunk = 12 + 8 + 16, data = chunk + 8, smp = data + DATA, i;
	uint32_t l = DATA + 2 * WORDS;
	int already = 0;

	memcpy( b, "FORM", 4 );
	memcpy( b + 8, "E4B0", 4 );
	memcpy( b + 12, "TOC1", 4 );  // a chunk that stays as it is
	b[19] = 16;
	for ( i = 0; i < 16; i++ )
	{
		b[20 + i] = (uint8_t)( 200 + i );
	}
	memcpy( b + chunk, "E3S1", 4 );
	b[chunk + 4] = (uint8_t)( l >> 24 );
	b[chunk + 5] = (uint8_t)( l >> 16 );
	b[chunk + 6] = (uint8_t)( l >> 8 );
	b[chunk + 7] = (uint8_t)l;
	b[data + 18 + 4] = DATA;  // parameter 1: the left start, little-endian
	for ( i = 0; i < WORDS; i++ )
	{
		// a slow ramp, little-endian, as on a hard disk
		int v = (int)i * 8 - 8000;

		b[smp + 2 * i] = (uint8_t)v;
		b[smp + 2 * i + 1] = (uint8_t)( v >> 8 );
	}
	memset( b + smp + 2 * WORDS, 0xEE, 7 );  // junk after the last chunk stays
	memcpy( orig, b, LEN );

	CHECK( fimg_bank_to_floppy_order( b, LEN, &already ) == 1 && already == 0 );
	CHECK( memcmp( b, orig, smp ) == 0 );
	for ( i = 0; i < WORDS; i++ )
	{
		CHECK( b[smp + 2 * i] == orig[smp + 2 * i + 1] && b[smp + 2 * i + 1] == orig[smp + 2 * i] );
	}
	CHECK( memcmp( b + smp + 2 * WORDS, orig + smp + 2 * WORDS, 7 ) == 0 );

	// a bank that came off a floppy is not swapped back
	memcpy( orig, b, LEN );
	CHECK( fimg_bank_to_floppy_order( b, LEN, &already ) == 0 && already == 1 );
	CHECK( memcmp( b, orig, LEN ) == 0 );

	// no samples: nothing to do
	memcpy( b + chunk, "E4P1", 4 );
	memcpy( orig, b, LEN );
	CHECK( fimg_bank_to_floppy_order( b, LEN, &already ) == 0 && already == 0 );
	CHECK( memcmp( b, orig, LEN ) == 0 );

	free( b );
	free( orig );
}

/**
  * @brief  The DOS format.
  *
  * @retval None.
  */
static void test_fat( void )
{
	uint8_t *a = make_bank( 102402 ), *b = make_bank( 700 );
	uint8_t *fill = make_bank( (size_t)FIMG_FAT_DATA_SECTORS * FIMG_SECTOR );
	uint8_t *back = malloc( 102402 );
	char stored[13], name[13];
	uint32_t size = 0;
	fimg_info info;
	int i;

	fimg_fat_blank( img, NULL );
	CHECK( memcmp( img + 3, "E-MU SYS", 8 ) == 0 && img[0] == 0xE9 );
	CHECK( img[510] == 0x55 && img[511] == 0xAA );
	CHECK( img[512] == 0xF0 && img[513] == 0xFF && img[514] == 0xFF );
	CHECK( img[33 * 512] == 0x55 );
	CHECK( memcmp( img + 43, "E-MU       ", 11 ) == 0 );
	fimg_inspect( img, FIMG_SIZE_144, &info );
	CHECK( info.kind == FIMG_KIND_FAT12 && info.files == 0 && info.free_bytes == FIMG_FAT_DATA_SECTORS * 512u );
	CHECK( strcmp( info.name, "E-MU" ) == 0 );

	CHECK( fimg_fat_add( img, "ICE.E4B", a, 102402, 0, stored ) == 0 );
	CHECK( strcmp( stored, "ICE.E4B" ) == 0 );
	CHECK( fimg_fat_add( img, "small.e4b", b, 700, 0, stored ) == 0 );
	CHECK( strcmp( stored, "SMALL.E4B" ) == 0 );
	CHECK( fimg_fat_entry( img, 0, name, &size ) == 0 && strcmp( name, "ICE.E4B" ) == 0 && size == 102402 );
	CHECK( fimg_fat_entry( img, 1, name, &size ) == 0 && strcmp( name, "SMALL.E4B" ) == 0 && size == 700 );
	CHECK( fimg_fat_entry( img, 2, name, &size ) == -1 );
	CHECK( fimg_fat_read( img, 0, back, 102402 ) == 0 && memcmp( back, a, 102402 ) == 0 );
	CHECK( fimg_fat_read( img, 1, back, 700 ) == 0 && memcmp( back, b, 700 ) == 0 );
	// the two FATs stay the same
	CHECK( memcmp( img + 512, img + 10 * 512, 9 * 512 ) == 0 );

	// names that clash get a tail, and files keep apart
	CHECK( fimg_fat_add( img, "Ultra Sessions.E4R", b, 700, 0, stored ) == 0 && strcmp( stored, "ULTRASES.E4R" ) == 0 );
	CHECK( fimg_fat_add( img, "Ultra Sessions X.E4R", b, 700, 0, stored ) == 0 && strcmp( stored, "ULTRAS~1.E4R" ) == 0 );
	CHECK( fimg_fat_add( img, "Ultra Sessions Y.E4R", b, 700, 0, stored ) == 0 && strcmp( stored, "ULTRAS~2.E4R" ) == 0 );

	// full disk, then a full directory
	fimg_fat_blank( img, "Label" );
	CHECK( memcmp( img + 43, "LABEL      ", 11 ) == 0 );
	CHECK( fimg_fat_add( img, "FILL.BIN", fill, (size_t)FIMG_FAT_DATA_SECTORS * FIMG_SECTOR, 0, NULL ) == 0 );
	CHECK( fimg_fat_add( img, "MORE.BIN", b, 1, 0, NULL ) == -3 );
	fimg_fat_blank( img, NULL );
	for ( i = 0; i < FIMG_FAT_ROOT_ENTRIES; i++ )
	{
		char n[16];

		snprintf( n, sizeof n, "F%03d.E4B", i );
		CHECK( fimg_fat_add( img, n, b, 10, 0, NULL ) == 0 );
	}
	CHECK( fimg_fat_add( img, "ONEMORE.E4B", b, 10, 0, NULL ) == -2 );

	free( a );
	free( b );
	free( fill );
	free( back );
}

/**
  * @brief  Making image files, and what -info says about other images.
  *
  * @retval None.
  */
static void test_make( void )
{
	size_t big = (size_t)FIMG_EOS_DISK_BYTES + 5000;
	uint8_t *bank = make_bank( big );
	static char msg[8192];
	const char *in;
	const char *list[2];
	uint8_t *d = NULL;
	size_t len = 0;
	fimg_info info;
	fjob_set set = { NULL, 0 };
	char path[FPLAT_PATH + 16];

	in = tmp( "bank.e4b" );
	CHECK( fplat_write_file( in, bank, big ) == 0 );
	{
		char inp[FPLAT_PATH];

		snprintf( inp, sizeof inp, "%s", in );
		// in memory first: no file exists until the set is saved
		CHECK( fjob_set_eos( &set, inp, NULL, msg, sizeof msg ) == 2 );
		fjob_disk_path( tmp( "set.img" ), 2, 2, path, sizeof path );
		remove( path );  // from an earlier run of this test
		CHECK( fjob_set_eos( &set, inp, NULL, msg, sizeof msg ) == 2 );
		CHECK( !fplat_is_regular( path, NULL ) );
		CHECK( fjob_set_disk( &set, 2 ) != NULL && fjob_set_disk( &set, 3 ) == NULL && fjob_set_disk( &set, 0 ) == NULL );
		fimg_inspect( fjob_set_disk( &set, 2 ), FIMG_SIZE_144, &info );
		CHECK( info.kind == FIMG_KIND_EOS_BANK && info.disk == 2 && strcmp( info.name, "bank" ) == 0 );
		// and the same set can be saved, too
		CHECK( fjob_set_save( &set, tmp( "set.img" ), msg, sizeof msg ) == 0 );
		CHECK( fplat_read_file( path, &d, &len, FIMG_SIZE_144 ) == 0 && len == FIMG_SIZE_144 );
		CHECK( memcmp( d, fjob_set_disk( &set, 2 ), FIMG_SIZE_144 ) == 0 );
		free( d );
		list[0] = inp;
		CHECK( fjob_set_dos( &set, list, 1, "TEST", msg, sizeof msg ) == -1 );  // too big
		CHECK( set.data == NULL && set.disks == 0 );
	}
	CHECK( fjob_set_eos( &set, NULL, NULL, msg, sizeof msg ) == 1 );
	fimg_inspect( fjob_set_disk( &set, 1 ), FIMG_SIZE_144, &info );
	CHECK( info.kind == FIMG_KIND_EOS_BLANK );
	CHECK( fjob_set_save( &set, tmp( "blank.img" ), msg, sizeof msg ) == 0 );
	CHECK( fplat_read_file( tmp( "blank.img" ), &d, &len, FIMG_SIZE_144 ) == 0 );
	free( d );
	fjob_set_free( &set );
	CHECK( set.data == NULL );

	memset( img, 0, sizeof img );
	img[0] = 0x12; img[1] = 0x34; img[2] = 0x56; img[3] = 0x78;
	fimg_inspect( img, FIMG_SIZE_144, &info );
	CHECK( info.kind == FIMG_KIND_UPDATE );
	img[0] = 0x76; img[1] = 0x54; img[2] = 0x32; img[3] = 0x11;
	memcpy( img + 0x18, "EOS v4.70", 9 );
	fimg_inspect( img, FIMG_SIZE_144, &info );
	CHECK( info.kind == FIMG_KIND_UPDATE && strstr( info.text, "EOS v4.70" ) != NULL );
	fimg_inspect( img, 1000, &info );
	CHECK( info.kind == FIMG_KIND_UNKNOWN );
	free( bank );
}

/**
  * @brief  Make a bank with a table of contents: a multisetup, which is not listed,
  *         two presets, a sequence and a sample.
  *
  * @param  b Receives the bank, BANK_LEN bytes.
  *
  * @retval None.
  */
static void make_listed_bank( uint8_t *b )
{
	static const struct
	{
		const char *tag;
		int         index;
		const char *name;
		int         size;
	} e[LISTED_ENTRIES] =
	{
		{ "E4Ma", 0, "                ", 258 },
		{ "E4P1", 0, "Grand Piano     ", 1394 },
		{ "E4P1", 7, "Strings         ", 2000 },
		{ "E4s1", 0, "Song            ", 132 },
		{ "E3S1", 1, "Piano C3        ", 70000 }
	};
	int i;

	memset( b, 0, BANK_LEN );
	memcpy( b, "FORM", 4 );
	b[6] = (BANK_LEN - 12) >> 8;
	b[7] = (BANK_LEN - 12) & 0xFF;
	memcpy( b + 8, "E4B0", 4 );
	memcpy( b + 12, "TOC1", 4 );
	b[19] = LISTED_ENTRIES * 32;
	for ( i = 0; i < LISTED_ENTRIES; i++ )
	{
		uint8_t *t = b + 20 + i * 32;

		memcpy( t, e[i].tag, 4 );
		t[5] = (uint8_t)( e[i].size >> 16 );
		t[6] = (uint8_t)( e[i].size >> 8 );
		t[7] = (uint8_t)e[i].size;
		t[13] = (uint8_t)e[i].index;
		memcpy( t + 14, e[i].name, 16 );
	}
}

/**
  * @brief  What the read page lists, and the name it suggests.
  *
  * @retval None.
  */
static void test_contents( void )
{
	static fimg_item it[64];
	uint8_t *b = malloc( BANK_LEN );
	uint8_t *big = make_bank( (size_t)FIMG_EOS_DISK_BYTES + 5000 );
	fimg_info info;
	char name[64];
	int n;

	make_listed_bank( b );

	// an EOS floppy: the bank, then what its table of contents lists
	CHECK( fimg_eos_disk( img, "Listed", b, BANK_LEN, 1 ) == 0 );
	n = fimg_list( img, FIMG_SIZE_144, it, 64 );
	CHECK( n == 5 );
	CHECK( strcmp( it[0].name, "Listed" ) == 0 && it[0].size == BANK_LEN && !it[0].inside );
	CHECK( strcmp( it[0].what, "Bank E4B0: 2 presets, 1 sample, 1 sequence" ) == 0 );
	CHECK( strcmp( it[1].name, "P000 Grand Piano" ) == 0 && strcmp( it[1].what, "Preset" ) == 0 );
	CHECK( it[1].size == 1394 && it[1].inside );
	CHECK( strcmp( it[2].name, "P007 Strings" ) == 0 );
	CHECK( strcmp( it[3].name, "s000 Song" ) == 0 && strcmp( it[3].what, "Sequence" ) == 0 );
	CHECK( strcmp( it[4].name, "S001 Piano C3" ) == 0 && strcmp( it[4].what, "Sample" ) == 0 && it[4].size == 70000 );

	// the list stops where it runs out of room, and says so
	n = fimg_list( img, FIMG_SIZE_144, it, 3 );
	CHECK( n == 3 && strcmp( it[2].name, "..." ) == 0 );

	// a bad sector reads as zeros: entries after it are not trusted
	memset( img + 512 + 20 + 2 * 32, 0, 32 );
	n = fimg_list( img, FIMG_SIZE_144, it, 64 );
	CHECK( n == 2 && strcmp( it[0].what, "Bank E4B0: 1 preset, 0 samples" ) == 0 );

	// a table of contents that does not fit is not followed
	CHECK( fimg_eos_disk( img, "Listed", b, BANK_LEN, 1 ) == 0 );
	img[512 + 16] = 0xFF;
	img[512 + 17] = 0xFF;
	img[512 + 18] = 0xFF;
	img[512 + 19] = 0xE0;
	n = fimg_list( img, FIMG_SIZE_144, it, 64 );
	CHECK( n == 1 && strstr( it[0].what, "cannot be read" ) != NULL );

	// a bank that says it is short: its table of contents is cut off with it
	CHECK( fimg_eos_disk( img, "Listed", b, BANK_LEN, 1 ) == 0 );
	img[0x30] = img[0x31] = img[0x32] = 0;
	img[0x33] = 40;
	n = fimg_list( img, FIMG_SIZE_144, it, 64 );
	CHECK( n == 1 && strstr( it[0].what, "cannot be read" ) != NULL );

	// a name with bytes that cannot be shown
	CHECK( fimg_eos_disk( img, "Listed", b, BANK_LEN, 1 ) == 0 );
	img[512 + 20 + 32 + 14] = 0x07;
	CHECK( fimg_list( img, FIMG_SIZE_144, it, 64 ) == 5 && strcmp( it[1].name, "P000 ?rand Piano" ) == 0 );

	// disk 2 of a set has no table of contents
	CHECK( fimg_eos_disk( img, "Set", big, (size_t)FIMG_EOS_DISK_BYTES + 5000, 2 ) == 0 );
	n = fimg_list( img, FIMG_SIZE_144, it, 64 );
	CHECK( n == 1 && strcmp( it[0].what, "Bank E4B0, disk 2 of 2: disk 1 lists it" ) == 0 );
	fimg_inspect( img, FIMG_SIZE_144, &info );
	fimg_image_name( &info, name, sizeof name );
	CHECK( strcmp( name, "Set_2of2.img" ) == 0 );

	// a DOS floppy: every file, and inside the banks
	fimg_fat_blank( img, "SONGS" );
	CHECK( fimg_fat_add( img, "BANK.E4B", b, BANK_LEN, 0, NULL ) == 0 );
	CHECK( fimg_fat_add( img, "NOTE.TXT", big + 100, 300, 0, NULL ) == 0 );
	n = fimg_list( img, FIMG_SIZE_144, it, 64 );
	CHECK( n == 6 );
	CHECK( strcmp( it[0].name, "BANK.E4B" ) == 0 && it[0].size == BANK_LEN && strstr( it[0].what, "2 presets" ) != NULL );
	CHECK( strcmp( it[1].name, "P000 Grand Piano" ) == 0 && it[1].inside );
	CHECK( strcmp( it[5].name, "NOTE.TXT" ) == 0 && strcmp( it[5].what, "File" ) == 0 && it[5].size == 300 && !it[5].inside );
	fimg_inspect( img, FIMG_SIZE_144, &info );
	fimg_image_name( &info, name, sizeof name );
	CHECK( strcmp( name, "SONGS.img" ) == 0 );

	// nothing to list
	fimg_fat_blank( img, NULL );
	CHECK( fimg_list( img, FIMG_SIZE_144, it, 64 ) == 0 );
	fimg_eos_blank( img );
	CHECK( fimg_list( img, FIMG_SIZE_144, it, 64 ) == 0 );
	fimg_inspect( img, FIMG_SIZE_144, &info );
	fimg_image_name( &info, name, sizeof name );
	CHECK( strcmp( name, "floppy.img" ) == 0 );
	CHECK( fimg_list( img, 1000, it, 64 ) == 0 );

	// 720 KB is said, and an image of it is not read past its end
	fimg_eos_blank( img );
	CHECK( fimg_list( img, FIMG_SIZE_720, it, 64 ) == 1 && strstr( it[0].what, "No EOS" ) != NULL );

	// a name off a floppy may hold anything, but not a way out of the folder
	memset( &info, 0, sizeof info );
	info.kind = FIMG_KIND_FAT12;
	snprintf( info.name, sizeof info.name, "..\\a:b*/c" );
	fimg_image_name( &info, name, sizeof name );
	CHECK( strcmp( name, ".._a_b__c.img" ) == 0 );
	snprintf( info.name, sizeof info.name, ".." );
	fimg_image_name( &info, name, sizeof name );
	CHECK( strcmp( name, "floppy.img" ) == 0 );

	free( b );
	free( big );
}

/**
  * @brief  The transfers on the stand-in drive, and what stops them.
  *
  * @retval None.
  */
static void test_jobs( void )
{
	static const int bad[2] = { 5, 2000 };
	static const int flip[1] = { 1234 };
	uint8_t *d = NULL;
	size_t len = 0;
	fdr_drive drv[FDR_MAX_DRIVES];
	fdr_handle *h = NULL;
	char err[256], msg[FPLAT_PATH + 64];
	int i, n;
	char image[FPLAT_PATH], fake[FPLAT_PATH], out[FPLAT_PATH];

	snprintf( image, sizeof image, "%s", tmp( "image.img" ) );
	snprintf( fake, sizeof fake, "%s", tmp( "fake.img" ) );
	snprintf( out, sizeof out, "%s", tmp( "out.img" ) );
	for ( i = 0; i < FIMG_SIZE_144; i++ )
	{
		img[i] = (uint8_t)( i ^ ( i >> 9 ) );
	}
	CHECK( fplat_write_file( image, img, FIMG_SIZE_144 ) == 0 );
	memset( img2, 0xF6, sizeof img2 );
	CHECK( fplat_write_file( fake, img2, FIMG_SIZE_144 ) == 0 );

	// nothing but an ordinary file of a floppy's size can stand in for a drive
	CHECK( fdr_fake_set( tmp( "does-not-exist" ), NULL, 0, NULL, 0 ) == -1 );
	CHECK( fdr_fake_set( folder, NULL, 0, NULL, 0 ) == -1 );

	CHECK( fdr_fake_set( fake, NULL, 0, NULL, 0 ) == 0 );
	n = fdr_list( drv );
	CHECK( n >= 1 && strcmp( drv[n - 1].id, FDR_FAKE_ID ) == 0 );

	// a read-only handle refuses writes
	CHECK( fdr_open( FDR_FAKE_ID, 0, &h, err, sizeof err ) == 0 );
	CHECK( fdr_write( h, 0, 1, img ) == FDR_E_IO );
	fdr_close( h );

	// from memory: what the Make page's Write does
	CHECK( fjob_write_mem( &job, FDR_FAKE_ID, img, FIMG_SIZE_144 ) == FJOB_OK );
	CHECK( fplat_read_file( fake, &d, &len, FIMG_SIZE_144 ) == 0 && memcmp( d, img, FIMG_SIZE_144 ) == 0 );
	free( d );
	CHECK( fplat_write_file( fake, img2, FIMG_SIZE_144 ) == 0 );

	// write and verify
	CHECK( fjob_write( &job, FDR_FAKE_ID, image ) == FJOB_OK );
	CHECK( job.nbad == 0 && job.phase == FJOB_FINISHED && job.done == FIMG_SECTORS_144 );
	CHECK( fplat_read_file( fake, &d, &len, FIMG_SIZE_144 ) == 0 && memcmp( d, img, FIMG_SIZE_144 ) == 0 );
	free( d );

	// read back to a file
	CHECK( fjob_read( &job, FDR_FAKE_ID, out ) == FJOB_OK && job.nbad == 0 );
	CHECK( fplat_read_file( out, &d, &len, FIMG_SIZE_144 ) == 0 && memcmp( d, img, FIMG_SIZE_144 ) == 0 );
	free( d );

	// into memory, what the Read page does: it makes no file, then saves on request
	remove( out );
	CHECK( fjob_read_mem( &job, FDR_FAKE_ID, &d, &len ) == FJOB_OK && job.nbad == 0 );
	CHECK( job.phase == FJOB_FINISHED && job.done == FIMG_SECTORS_144 );
	CHECK( d != NULL && len == FIMG_SIZE_144 && memcmp( d, img, FIMG_SIZE_144 ) == 0 );
	CHECK( !fplat_is_regular( out, NULL ) );
	CHECK( fjob_save( d, len, out, msg, sizeof msg ) == 0 && strstr( msg, "Saved" ) != NULL );
	free( d );
	CHECK( fplat_read_file( out, &d, &len, FIMG_SIZE_144 ) == 0 && memcmp( d, img, FIMG_SIZE_144 ) == 0 );
	free( d );

	// and with bad sectors, the same
	CHECK( fdr_fake_set( fake, bad, 2, NULL, 0 ) == 0 );
	CHECK( fjob_read_mem( &job, FDR_FAKE_ID, &d, &len ) == FJOB_OK && job.nbad == 2 );
	CHECK( d[5 * 512] == 0 && memcmp( d, img, 5 * 512 ) == 0 && strstr( job.message, "2 unreadable" ) != NULL );
	free( d );

	// a read that is cancelled keeps nothing
	job.cancel = 1;
	d = img2;
	CHECK( fjob_read_mem( &job, FDR_FAKE_ID, &d, &len ) == FJOB_E_CANCELLED && d == NULL && len == 0 );
	job.cancel = 0;

	// no drive: nothing, and it says why
	d = img2;
	CHECK( fjob_read_mem( &job, "image.img", &d, &len ) == FJOB_E_DRIVE && d == NULL && job.message[0] != 0 );

	// bad sectors while reading: reported, zeros in the image, the rest intact
	CHECK( fdr_fake_set( fake, bad, 2, NULL, 0 ) == 0 );
	CHECK( fjob_read( &job, FDR_FAKE_ID, out ) == FJOB_OK );
	CHECK( job.nbad == 2 && job.bad[0] == 5 && job.bad[1] == 2000 );
	CHECK( fplat_read_file( out, &d, &len, FIMG_SIZE_144 ) == 0 );
	CHECK( d[5 * 512] == 0 && d[5 * 512 + 100] == 0 );
	CHECK( memcmp( d, img, 5 * 512 ) == 0 && memcmp( d + 6 * 512, img + 6 * 512, 512 ) == 0 );
	free( d );

	// a sector that will not write stops the job and says which
	CHECK( fjob_write( &job, FDR_FAKE_ID, image ) == FJOB_E_WRITE );
	CHECK( strstr( job.message, "sector 5 " ) != NULL );

	// a sector that writes wrongly is caught by the verify
	CHECK( fdr_fake_set( fake, NULL, 0, flip, 1 ) == 0 );
	CHECK( fjob_write( &job, FDR_FAKE_ID, image ) == FJOB_E_VERIFY );
	CHECK( job.nbad == 1 && job.bad[0] == 1234 );

	// an image of the wrong size never reaches the floppy
	CHECK( fdr_fake_set( fake, NULL, 0, NULL, 0 ) == 0 );
	CHECK( fjob_write( &job, FDR_FAKE_ID, image ) == FJOB_OK );
	CHECK( fplat_write_file( out, img, 1000 ) == 0 );
	CHECK( fjob_write( &job, FDR_FAKE_ID, out ) == FJOB_E_IMAGE );
	CHECK( fplat_write_file( out, img, FIMG_SIZE_720 ) == 0 );
	CHECK( fjob_write( &job, FDR_FAKE_ID, out ) == FJOB_E_IMAGE );
	CHECK( fplat_read_file( fake, &d, &len, FIMG_SIZE_144 ) == 0 && memcmp( d, img, FIMG_SIZE_144 ) == 0 );
	free( d );

	// cancelling
	job.cancel = 1;
	CHECK( fjob_read( &job, FDR_FAKE_ID, tmp( "cancelled.img" ) ) == FJOB_E_CANCELLED );
	CHECK( !fplat_is_regular( tmp( "cancelled.img" ), NULL ) );
	job.cancel = 0;
}

/**
  * @brief  Names that are not floppy drives, which every system must refuse.
  *
  * On a machine without a floppy drive these all fail at the first check; the
  * point is that none opens, whatever the machine has. The system drive and the
  * raw disk names are the ones a wrong target would ruin.
  *
  * @retval None.
  */
static void test_refusals( void )
{
	static const char *const ids[] =
	{
		"", "C:", "D:", "c:", "C", "A", "\\\\.\\PhysicalDrive0", "\\\\.\\C:", "\\\\.\\A:",
		"/dev/sda", "/dev/sda1", "/dev/nvme0n1", "/dev/disk0", "/dev/rdisk0", "/dev/mmcblk0",
		"/dev/null", "/dev/zero", "/etc/passwd", "image.img", "AB:"
	};
	size_t i;

	for ( i = 0; i < sizeof ids / sizeof ids[0]; i++ )
	{
		fdr_handle *h = NULL;
		char err[256] = "";
		int rd = fdr_open( ids[i], 0, &h, err, sizeof err );

		CHECK( rd != 0 && h == NULL );
		if ( h != NULL )
		{
			fdr_close( h );
		}
		// the write path must refuse too; a target here would be a disaster, so
		// a successful open is closed at once and writes nothing
		h = NULL;
		rd = fdr_open( ids[i], 1, &h, err, sizeof err );
		CHECK( rd != 0 && h == NULL );
		if ( h != NULL )
		{
			fdr_close( h );
		}
	}
}

/**
  * @brief  What a save may write to: only ever an ordinary file.
  *
  * The names of real disks are only looked at, by the check that opens nothing:
  * if a bug ever let one through, this test must not be what ruins the disk. The
  * writes are tried on devices where a mistake costs nothing.
  *
  * @retval None.
  */
static void test_write_targets( void )
{
	static const char *const looked[] =
	{
#ifdef _WIN32
		"\\\\.\\PhysicalDrive0", "//./PhysicalDrive0", "/\\./PhysicalDrive0", "\\/./PhysicalDrive0",
		"\\\\.\\C:", "//./C:", "\\\\.\\A:", "\\\\?\\PhysicalDrive0", "//?/PhysicalDrive0",
		"\\??\\PhysicalDrive0", "\\??\\C:", "\\\\.\\GLOBALROOT\\Device\\Harddisk0\\DR0",
		"NUL", "nul", "CON", "COM1", "LPT1"
#else
		"/dev/null", "/dev/zero", "/"
#endif
	};
	static const char *const tried[] =
	{
#ifdef _WIN32
		"NUL", "\\\\.\\NUL", "//./NUL", "\\??\\NUL"
#else
		"/dev/null", "/dev/zero"
#endif
	};
	char path[FPLAT_PATH], msg[1024];
	fjob_set set = { NULL, 0 };
	uint8_t *d = NULL;
	size_t i, len = 0;
	uint64_t size = 1;

	for ( i = 0; i < sizeof looked / sizeof looked[0]; i++ )
	{
		CHECK( !fplat_write_target_ok( looked[i] ) );
		CHECK( !fplat_is_regular( looked[i], NULL ) );
	}
	CHECK( fjob_set_eos( &set, NULL, NULL, msg, sizeof msg ) >= 0 );
	for ( i = 0; i < sizeof tried / sizeof tried[0]; i++ )
	{
		CHECK( fplat_write_file( tried[i], img, 16 ) == -2 );
		CHECK( fjob_set_save( &set, tried[i], msg, sizeof msg ) == -1 && strstr( msg, "Refused" ) != NULL );
		CHECK( fjob_save( img, 16, tried[i], msg, sizeof msg ) == -1 && strstr( msg, "Refused" ) != NULL );

		// refused before the drive is touched, not after a long read
		CHECK( fjob_read( &job, FDR_FAKE_ID, tried[i] ) == FJOB_E_OUTPUT );
		CHECK( job.done == 0 && strstr( job.message, "Refused" ) != NULL );
	}
	CHECK( fplat_write_file( folder, img, 16 ) == -2 );

	// an ordinary file is created, and replaced whole: nothing is left of a longer one
	snprintf( path, sizeof path, "%s", tmp( "save-target.img" ) );
	remove( path );
	CHECK( fplat_write_target_ok( path ) );
	CHECK( fplat_write_file( path, img, 1000 ) == 0 );
	CHECK( fplat_read_file( path, &d, &len, FIMG_SIZE_144 ) == 0 && len == 1000 && memcmp( d, img, 1000 ) == 0 );
	free( d );
	CHECK( fplat_write_target_ok( path ) );
	CHECK( fplat_write_file( path, img + 7, 10 ) == 0 );
	CHECK( fplat_read_file( path, &d, &len, FIMG_SIZE_144 ) == 0 && len == 10 && memcmp( d, img + 7, 10 ) == 0 );
	free( d );
	CHECK( fplat_write_file( path, img, 0 ) == 0 );
	CHECK( fplat_is_regular( path, &size ) && size == 0 );
	CHECK( fjob_set_save( &set, path, msg, sizeof msg ) == 0 );
	CHECK( fplat_is_regular( path, &size ) && size == FIMG_SIZE_144 );
	fjob_set_free( &set );

	// a folder that is not there is a failure, not a refusal
	CHECK( fplat_write_file( tmp( "no-such-folder/x.img" ), img, 16 ) == -1 );

#ifndef _WIN32
	// a link to a device, and a FIFO nobody reads, which must not hang the program
	snprintf( path, sizeof path, "%s", tmp( "link-to-device" ) );
	remove( path );
	if ( symlink( "/dev/null", path ) == 0 )
	{
		CHECK( !fplat_write_target_ok( path ) );
		CHECK( fplat_write_file( path, img, 16 ) == -2 );
		remove( path );
	}
	snprintf( path, sizeof path, "%s", tmp( "a-fifo" ) );
	remove( path );
	if ( mkfifo( path, 0600 ) == 0 )
	{
		CHECK( !fplat_write_target_ok( path ) );
		CHECK( fplat_write_file( path, img, 16 ) == -2 );
		remove( path );
	}
#endif
}

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Run the tests.
  *
  * @param  argc Argument count.
  * @param  argv A folder for the test files.
  *
  * @retval 0 if every check passed, 1 if not.
  */
int main( int argc, char **argv )
{
	folder = argc > 1 ? argv[1] : ".";
	test_names();
	test_eos();
	test_order();
	test_fat();
	test_make();
	test_contents();
	test_jobs();
	test_refusals();
	test_write_targets();
	printf( "%d checks, %d failed\n", checks, failures );
	return failures != 0;
}
