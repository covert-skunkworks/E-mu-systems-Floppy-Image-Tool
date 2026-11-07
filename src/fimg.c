//  Description:
//
//		fimg.c -- floppy images for E-mu samplers running EOS.
//
//		EOS floppies come in two formats, and both were worked out by having
//		every EOS we hold write and read them in the emulator (README.md has
//		the table). It is worth knowing what is NOT here: the EMU3 file system
//		of EOS hard disks is not used on floppies (EOS shows such an image as
//		an empty floppy), and no EOS tested reads a 720 KB floppy.
//
//		1. EOS's own format. No file system: one bank per floppy, spilling onto
//		   as many floppies as it takes. Disk > Utils > Format only formats the
//		   tracks, so a formatted floppy is all fill bytes (55); the first save
//		   writes sector 0, then the bank's bytes from sector 1. Big-endian:
//
//		     off size field
//		     00   16  "Emulator 4 Fdisk"
//		     10   16  bank name, space padded
//		     20    4  0
//		     24    4  1
//		     28    4  this disk, from 1
//		     2C    4  disks in the set
//		     30    4  bank size in bytes
//		     34    4  form type, "E4B0" or "E4Br"
//		     38    4  0
//		     3C    4  bank size in sectors, rounded up
//		     40    8  0
//		     48    4  bank size in sectors again
//
//		   The rest of the sector is a stale buffer on EOS's own disks after
//		   the first (the bytes past 4C differ from disk to disk), so nothing
//		   can depend on it; this module writes zeros. Each disk holds 2879
//		   sectors of bank: disk n starts (n - 1) * 2879 * 512 into the file.
//		   What is unwritten past the end of the bank stays fill bytes.
//
//		2. DOS (FAT12). An ordinary 1.44 MB floppy that a PC can read too.
//		   The layout is the one EOS 4.70's Format > FAT writes: 1 sector per
//		   cluster, 2 FATs of 9 sectors, 224 root entries, media F0, a boot
//		   sector with no code. Earlier EOS cannot make such a floppy (their
//		   Format only ever makes the first kind), but they all read and write
//		   on one, and show the volume label as the folder's name.
//
//		   Only 8.3 names: EOS before 4.7 skips long-name entries and lists the
//		   8.3 name beside them (4.70 alone shows the long one), so long names
//		   would use up directory slots for nothing. A file's bank number on
//		   the screen is its place in the directory, whatever its name says:
//		   "B.003-" prefixes mean nothing on a floppy.
//
//		Sample byte order, for both formats. A bank file on a hard disk holds
//		its samples little-endian, but EOS moves floppy data to sound memory
//		as CPU words, with no swap, and its CPUs are big-endian Motorola parts
//		(68020, ColdFire), so on a floppy they are big-endian. EOS does the
//		same when it saves to a floppy. So a bank copied unchanged onto
//		a floppy lists and loads, and every sample plays as noise. Everything
//		else in the bank is the same on both. The swapped range is an E3S1
//		chunk's data, from the lower of its start offsets (parameters 1 and 2,
//		left and right; a mono sample may sit on either side) to the chunk's
//		end. The check that found this:
//		after a load from a floppy of ours, sound memory must equal what it
//		holds after a load from a hard disk.
//

//------------------------------------------------------------------------------
//------------------------------- Includes -------------------------------------
//------------------------------------------------------------------------------

#include "fimg.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

//------------------------------------------------------------------------------
//------------------------ Local macros and constants --------------------------
//------------------------------------------------------------------------------

#define FILL_BYTE       0x55

#define EOS_MAGIC       "Emulator 4 Fdisk"
#define EOS_MAGIC_LEN   16

#define FAT_SECTORS     9
#define FAT1_OFF        ( 1 * FIMG_SECTOR )
#define FAT2_OFF        ( ( 1 + FAT_SECTORS ) * FIMG_SECTOR )
#define ROOT_OFF        ( ( 1 + 2 * FAT_SECTORS ) * FIMG_SECTOR )
#define ROOT_SECTORS    14
#define DATA_OFF        ( ( 1 + 2 * FAT_SECTORS + ROOT_SECTORS ) * FIMG_SECTOR )
#define DIRENT          32
#define FAT_EOC         0xFFF
#define ATTR_ARCHIVE    0x20
#define ATTR_LFN        0x0F
#define ATTR_LABEL      0x08
#define ATTR_DIR        0x10

// In an E3S1 chunk's data: u16 index, name[16], then the u32 parameters.
#define E3S1_PARAMS     18

//------------------------------------------------------------------------------
//------------------------------- Local functions ------------------------------
//------------------------------------------------------------------------------

/**
  * @brief  Store a big-endian u32.
  *
  * @param  p Where.
  * @param  v The value.
  *
  * @retval None.
  */
static void wr32be( uint8_t *p, uint32_t v )
{
	p[0] = (uint8_t)( v >> 24 ); p[1] = (uint8_t)( v >> 16 );
	p[2] = (uint8_t)( v >> 8 );  p[3] = (uint8_t)v;
}

/**
  * @brief  Fetch a big-endian u32.
  *
  * @param  p Where.
  *
  * @retval The value.
  */
static uint32_t rd32be( const uint8_t *p )
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/**
  * @brief  Store a little-endian u16.
  *
  * @param  p Where.
  * @param  v The value.
  *
  * @retval None.
  */
static void wr16le( uint8_t *p, uint32_t v )
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)( v >> 8 );
}

/**
  * @brief  Fetch a little-endian u16.
  *
  * @param  p Where.
  *
  * @retval The value.
  */
static uint32_t rd16le( const uint8_t *p )
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/**
  * @brief  Fetch a little-endian u32.
  *
  * @param  p Where.
  *
  * @retval The value.
  */
static uint32_t rd32le( const uint8_t *p )
{
	return rd16le( p ) | rd16le( p + 2 ) << 16;
}

/**
  * @brief  Find the sample data of the next E3S1 chunk of a bank.
  *
  * A chunk length that runs off the end stops the walk: banks can carry junk
  * after their last chunk.
  *
  * @param  bank The bank file.
  * @param  len Its size.
  * @param  pos The walk's position: start with 12, the first chunk.
  * @param  start Receives the offset of the sample data.
  * @param  end Receives the offset past it.
  *
  * @retval 1 if a sample was found, 0 at the end of the bank.
  */
static int next_sample( const uint8_t *bank, size_t len, size_t *pos, size_t *start, size_t *end )
{
	while ( *pos + 8 <= len )
	{
		size_t c = *pos;
		uint32_t l = rd32be( bank + c + 4 );

		if ( l > len - c - 8 )
		{
			return 0;
		}
		*pos = c + 8 + l + ( l & 1 );
		if ( memcmp( bank + c, "E3S1", 4 ) == 0 && l >= E3S1_PARAMS + 12 )
		{
			// Parameters 1 and 2 are the left and right starts. A mono sample may
			// sit on either side, with the other start 0, so the data begins at
			// the lower of those that are set.
			uint32_t left = rd32le( bank + c + 8 + E3S1_PARAMS + 4 );
			uint32_t right = rd32le( bank + c + 8 + E3S1_PARAMS + 8 );
			uint32_t first = left != 0 && ( right == 0 || left < right ) ? left : right;

			if ( first >= E3S1_PARAMS && first < l )
			{
				*start = c + 8 + first;
				*end = c + 8 + l;
				return 1;
			}
		}
	}
	return 0;
}

/**
  * @brief  Read a FAT12 entry.
  *
  * @param  fat The FAT.
  * @param  n Cluster number.
  *
  * @retval The entry.
  */
static uint32_t fat_get( const uint8_t *fat, uint32_t n )
{
	uint32_t o = n + n / 2;
	uint32_t v = (uint32_t)fat[o] | (uint32_t)fat[o + 1] << 8;

	return n & 1 ? v >> 4 : v & 0xFFF;
}

/**
  * @brief  Write a FAT12 entry.
  *
  * @param  fat The FAT.
  * @param  n Cluster number.
  * @param  v The entry.
  *
  * @retval None.
  */
static void fat_set( uint8_t *fat, uint32_t n, uint32_t v )
{
	uint32_t o = n + n / 2;

	if ( n & 1 )
	{
		fat[o] = (uint8_t)( ( fat[o] & 0x0F ) | ( ( v << 4 ) & 0xF0 ) );
		fat[o + 1] = (uint8_t)( v >> 4 );
	}
	else
	{
		fat[o] = (uint8_t)v;
		fat[o + 1] = (uint8_t)( ( fat[o + 1] & 0xF0 ) | ( ( v >> 8 ) & 0x0F ) );
	}
}

/**
  * @brief  Check that an image has exactly the FAT layout this module makes.
  *
  * @param  img The image.
  *
  * @retval 1 if it has, 0 if not.
  */
static int fat_layout_ok( const uint8_t *img )
{
	return rd16le( img + 11 ) == FIMG_SECTOR && img[13] == 1 &&
	       rd16le( img + 14 ) == 1 && img[16] == 2 &&
	       rd16le( img + 17 ) == FIMG_FAT_ROOT_ENTRIES &&
	       rd16le( img + 19 ) == FIMG_SECTORS_144 &&
	       rd16le( img + 22 ) == FAT_SECTORS;
}

/**
  * @brief  Turn a character into one a short name can hold.
  *
  * @param  c The character.
  *
  * @retval The upper-case character, or '_' for one DOS does not allow.
  */
static uint8_t short_char( int c )
{
	c = toupper( c & 0xFF );
	if ( isalnum( c ) || ( c != 0 && strchr( "$%'-_@~`!(){}^#&", c ) != NULL ) )
	{
		return (uint8_t)c;
	}
	return '_';
}

/**
  * @brief  Whether a directory entry's name is in use in the root.
  *
  * @param  img The image.
  * @param  sfn The eleven characters.
  *
  * @retval 1 if it is, 0 if not.
  */
static int name_taken( const uint8_t *img, const uint8_t *sfn )
{
	const uint8_t *root = img + ROOT_OFF;
	int i;

	for ( i = 0; i < FIMG_FAT_ROOT_ENTRIES; i++ )
	{
		const uint8_t *e = root + i * DIRENT;

		if ( e[0] == 0 )
		{
			break;
		}
		if ( e[0] != 0xE5 && e[11] != ATTR_LFN && !( e[11] & ATTR_LABEL ) &&
		     memcmp( e, sfn, 11 ) == 0 )
		{
			return 1;
		}
	}
	return 0;
}

/**
  * @brief  Put a numeric tail on a short name, keeping it within 8 characters.
  *
  * @param  sfn The eleven characters; only the first eight change.
  * @param  n The number, 1 to 99.
  *
  * @retval None.
  */
static void short_tail( uint8_t *sfn, int n )
{
	char t[5];
	int tl = snprintf( t, sizeof t, "~%d", n );
	int len = 0;

	while ( len < 8 && sfn[len] != ' ' && sfn[len] != '~' )
	{
		len++;
	}
	if ( len > 8 - tl )
	{
		len = 8 - tl;
	}
	memset( sfn + len, ' ', (size_t)( 8 - len ) );
	memcpy( sfn + len, t, (size_t)tl );
}

/**
  * @brief  Turn a directory entry's name into NAME.EXT.
  *
  * @param  e The entry's eleven characters.
  * @param  name Receives the text.
  *
  * @retval None.
  */
static void short_text( const uint8_t *e, char name[13] )
{
	int i, k = 0;

	for ( i = 0; i < 8 && e[i] != ' '; i++ )
	{
		name[k++] = (char)e[i];
	}
	if ( e[8] != ' ' )
	{
		name[k++] = '.';
		for ( i = 8; i < 11 && e[i] != ' '; i++ )
		{
			name[k++] = (char)e[i];
		}
	}
	name[k] = 0;
}

/**
  * @brief  Find a root entry by the order the files are listed in.
  *
  * @param  img The image.
  * @param  index Which file, from 0.
  *
  * @retval The entry, or NULL if there are fewer files.
  */
static const uint8_t *root_file( const uint8_t *img, int index )
{
	const uint8_t *root = img + ROOT_OFF;
	int i;

	for ( i = 0; i < FIMG_FAT_ROOT_ENTRIES; i++ )
	{
		const uint8_t *e = root + i * DIRENT;

		if ( e[0] == 0 )
		{
			return NULL;
		}
		if ( e[0] == 0xE5 || e[11] == ATTR_LFN || ( e[11] & ATTR_LABEL ) )
		{
			continue;
		}
		if ( index-- == 0 )
		{
			return e;
		}
	}
	return NULL;
}

/**
  * @brief  Free clusters in the first FAT.
  *
  * @param  img The image.
  *
  * @retval The count.
  */
static uint32_t fat_free( const uint8_t *img )
{
	uint32_t c, n = 0;

	for ( c = 2; c < FIMG_FAT_DATA_SECTORS + 2; c++ )
	{
		if ( fat_get( img + FAT1_OFF, c ) == 0 )
		{
			n++;
		}
	}
	return n;
}

/**
  * @brief  Describe a DOS floppy for fimg_inspect.
  *
  * Any 1.44 MB FAT12 BPB counts, since PC-formatted floppies differ in small
  * ways from ours. The file list is only given for our own layout, which is
  * the one this module knows how to walk.
  *
  * @param  img The image.
  * @param  out Receives the result.
  *
  * @retval None.
  */
static void describe_fat( const uint8_t *img, fimg_info *out )
{
	size_t k;
	int i;
	char label[FIMG_LABEL_LEN + 1];

	out->kind = FIMG_KIND_FAT12;
	memcpy( label, img + 43, FIMG_LABEL_LEN );
	label[FIMG_LABEL_LEN] = 0;
	for ( i = FIMG_LABEL_LEN - 1; i >= 0 && label[i] == ' '; i-- )
	{
		label[i] = 0;
	}
	snprintf( out->name, sizeof out->name, "%s", label );

	if ( !fat_layout_ok( img ) )
	{
		snprintf( out->text, sizeof out->text,
		          "DOS (FAT12) floppy, label \"%s\", a layout other than EOS's own.",
		          label );
		return;
	}
	out->free_bytes = fat_free( img ) * FIMG_SECTOR;
	k = (size_t)snprintf( out->text, sizeof out->text,
	                      "DOS (FAT12) floppy, label \"%s\", %u KB free.\n",
	                      label, (unsigned)( out->free_bytes / 1024 ) );
	for ( i = 0; ; i++ )
	{
		char name[13];
		uint32_t size;

		if ( fimg_fat_entry( img, i, name, &size ) != 0 )
		{
			break;
		}
		out->files++;
		if ( k + 40 < sizeof out->text )
		{
			k += (size_t)snprintf( out->text + k, sizeof out->text - k,
			                       "%s  %u bytes\n", name, (unsigned)size );
		}
	}
}

//------------------------------------------------------------------------------
//--------------------------------- Public API ---------------------------------
//------------------------------------------------------------------------------

int fimg_is_bank( const uint8_t *data, size_t len, char form[5] )
{
	if ( len < 12 || memcmp( data, "FORM", 4 ) != 0 ||
	     ( memcmp( data + 8, "E4B0", 4 ) != 0 && memcmp( data + 8, "E4Br", 4 ) != 0 ) )
	{
		return 0;
	}
	if ( form != NULL )
	{
		memcpy( form, data + 8, 4 );
		form[4] = 0;
	}
	return 1;
}

void fimg_bank_name( const char *path, char out[FIMG_BANK_NAME_LEN + 1] )
{
	const char *base = path, *p, *dot;
	size_t n;

	for ( p = path; *p; p++ )
	{
		if ( *p == '/' || *p == '\\' )
		{
			base = p + 1;
		}
	}
	dot = strrchr( base, '.' );
	if ( ( base[0] == 'B' || base[0] == 'b' ) && base[1] == '.' &&
	     isdigit( (unsigned char)base[2] ) )
	{
		const char *dash = strchr( base, '-' );

		if ( dash != NULL && ( dot == NULL || dash < dot || dot == base + 1 ) )
		{
			base = dash + 1;
			dot = strrchr( base, '.' );
		}
	}
	n = dot != NULL && dot > base ? (size_t)( dot - base ) : strlen( base );
	if ( n > FIMG_BANK_NAME_LEN )
	{
		n = FIMG_BANK_NAME_LEN;
	}
	memcpy( out, base, n );
	out[n] = 0;
}

int fimg_bank_to_floppy_order( uint8_t *bank, size_t len, int *already )
{
	size_t pos = 12, start, end, i;
	uint64_t jump_le = 0, jump_be = 0;
	int samples = 0;

	if ( already != NULL )
	{
		*already = 0;
	}
	// Audio moves in small steps from one sample to the next, so the wrong byte
	// order shows as far larger jumps. A bank off a floppy must not be swapped back.
	while ( next_sample( bank, len, &pos, &start, &end ) )
	{
		for ( i = start; i + 4 <= end; i += 2 )
		{
			int le0 = (int16_t)( bank[i] | bank[i + 1] << 8 );
			int le1 = (int16_t)( bank[i + 2] | bank[i + 3] << 8 );
			int be0 = (int16_t)( bank[i] << 8 | bank[i + 1] );
			int be1 = (int16_t)( bank[i + 2] << 8 | bank[i + 3] );

			jump_le += (uint64_t)( le1 > le0 ? le1 - le0 : le0 - le1 );
			jump_be += (uint64_t)( be1 > be0 ? be1 - be0 : be0 - be1 );
		}
	}
	if ( jump_be * 2 < jump_le )
	{
		if ( already != NULL )
		{
			*already = 1;
		}
		return 0;
	}
	pos = 12;
	while ( next_sample( bank, len, &pos, &start, &end ) )
	{
		for ( i = start; i + 2 <= end; i += 2 )
		{
			uint8_t t = bank[i];

			bank[i] = bank[i + 1];
			bank[i + 1] = t;
		}
		samples++;
	}
	return samples;
}

void fimg_eos_blank( uint8_t *img )
{
	memset( img, FILL_BYTE, FIMG_SIZE_144 );
}

int fimg_eos_disks( size_t bank_len )
{
	size_t n;

	if ( bank_len == 0 )
	{
		return -1;
	}
	n = ( bank_len + FIMG_EOS_DISK_BYTES - 1 ) / FIMG_EOS_DISK_BYTES;
	return n > FIMG_EOS_MAX_DISKS ? -1 : (int)n;
}

int fimg_eos_disk( uint8_t *img, const char *name, const uint8_t *bank,
                   size_t bank_len, int disk )
{
	char form[5];
	int disks = fimg_eos_disks( bank_len );
	uint32_t sectors = (uint32_t)( ( bank_len + FIMG_SECTOR - 1 ) / FIMG_SECTOR );
	size_t off, n, name_len;

	if ( !fimg_is_bank( bank, bank_len, form ) )
	{
		return -1;
	}
	if ( disks < 0 || disk < 1 || disk > disks )
	{
		return -2;
	}
	fimg_eos_blank( img );
	memset( img, 0, FIMG_SECTOR );
	memcpy( img, EOS_MAGIC, EOS_MAGIC_LEN );
	memset( img + 0x10, ' ', FIMG_BANK_NAME_LEN );
	name_len = strlen( name );
	memcpy( img + 0x10, name, name_len < FIMG_BANK_NAME_LEN ? name_len : FIMG_BANK_NAME_LEN );
	wr32be( img + 0x24, 1 );
	wr32be( img + 0x28, (uint32_t)disk );
	wr32be( img + 0x2C, (uint32_t)disks );
	wr32be( img + 0x30, (uint32_t)bank_len );
	memcpy( img + 0x34, form, 4 );
	wr32be( img + 0x3C, sectors );
	wr32be( img + 0x48, sectors );
	off = (size_t)( disk - 1 ) * FIMG_EOS_DISK_BYTES;
	n = bank_len - off < FIMG_EOS_DISK_BYTES ? bank_len - off : FIMG_EOS_DISK_BYTES;
	memcpy( img + FIMG_SECTOR, bank + off, n );
	return 0;
}

void fimg_fat_blank( uint8_t *img, const char *label )
{
	static const uint8_t boot[43] =
	{
		0xE9, 0x00, 0x00, 'E', '-', 'M', 'U', ' ', 'S', 'Y', 'S',
		0x00, 0x02, 0x01, 0x01, 0x00, 0x02, 0xE0, 0x00, 0x40, 0x0B, 0xF0,
		0x09, 0x00, 0x12, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0x78, 0x56, 0x34, 0x12
	};
	const char *l = label != NULL && label[0] ? label : "E-MU";
	int i;
	int k;

	memset( img, FILL_BYTE, FIMG_SIZE_144 );
	memset( img, 0, DATA_OFF );
	memcpy( img, boot, sizeof boot );
	memset( img + 43, ' ', FIMG_LABEL_LEN );
	for ( i = 0; i < FIMG_LABEL_LEN && l[i]; i++ )
	{
		img[43 + i] = (uint8_t)toupper( (unsigned char)l[i] );
	}
	img[510] = 0x55;
	img[511] = 0xAA;
	for ( k = 0; k < 2; k++ )
	{
		uint8_t *fat = img + ( k ? FAT2_OFF : FAT1_OFF );

		fat[0] = 0xF0;
		fat[1] = 0xFF;
		fat[2] = 0xFF;
	}
}

void fimg_short_name( const char *name, uint8_t out[11] )
{
	const char *dot = strrchr( name, '.' );
	const char *p;
	int i = 0;

	memset( out, ' ', 11 );
	if ( dot == name )
	{
		dot = NULL;
	}
	for ( p = name; *p && p != dot && i < 8; p++ )
	{
		if ( *p != ' ' && *p != '.' )
		{
			out[i++] = short_char( *p );
		}
	}
	if ( i == 0 )
	{
		memcpy( out, "FILE", 4 );
	}
	if ( dot != NULL )
	{
		for ( p = dot + 1, i = 8; *p && i < 11; p++ )
		{
			if ( *p != ' ' )
			{
				out[i++] = short_char( *p );
			}
		}
	}
}

int fimg_fat_add( uint8_t *img, const char *name, const uint8_t *data,
                  size_t len, uint32_t dos_time, char stored[13] )
{
	uint8_t sfn[11];
	uint8_t *root = img + ROOT_OFF;
	uint8_t *fat = img + FAT1_OFF;
	uint8_t *slot = NULL;
	uint32_t need = fimg_fat_sectors( len );
	uint32_t first = 0, prev = 0, c, got = 0;
	int i, tail;

	if ( !fat_layout_ok( img ) )
	{
		return -1;
	}
	fimg_short_name( name, sfn );
	for ( i = 0; i < FIMG_FAT_ROOT_ENTRIES; i++ )
	{
		uint8_t e0 = root[i * DIRENT];

		if ( e0 == 0 || e0 == 0xE5 )
		{
			slot = root + i * DIRENT;
			break;
		}
	}
	if ( slot == NULL )
	{
		return -2;
	}
	for ( tail = 0; name_taken( img, sfn ); tail++ )
	{
		if ( tail == 99 )
		{
			return -4;
		}
		short_tail( sfn, tail + 1 );
	}
	if ( fat_free( img ) < need )
	{
		return -3;
	}
	for ( c = 2; c < FIMG_FAT_DATA_SECTORS + 2 && got < need; c++ )
	{
		size_t off = (size_t)got * FIMG_SECTOR;
		size_t n = len - off < FIMG_SECTOR ? len - off : FIMG_SECTOR;
		uint8_t *cl = img + DATA_OFF + (size_t)( c - 2 ) * FIMG_SECTOR;

		if ( fat_get( fat, c ) != 0 )
		{
			continue;
		}
		memcpy( cl, data + off, n );
		if ( n < FIMG_SECTOR )
		{
			memset( cl + n, 0, FIMG_SECTOR - n );
		}
		if ( prev != 0 )
		{
			fat_set( fat, prev, c );
		}
		else
		{
			first = c;
		}
		fat_set( fat, c, FAT_EOC );
		prev = c;
		got++;
	}
	memcpy( img + FAT2_OFF, fat, (size_t)FAT_SECTORS * FIMG_SECTOR );
	memset( slot, 0, DIRENT );
	memcpy( slot, sfn, 11 );
	slot[11] = ATTR_ARCHIVE;
	wr16le( slot + 14, dos_time & 0xFFFF );
	wr16le( slot + 16, dos_time >> 16 );
	wr16le( slot + 18, dos_time >> 16 );
	wr16le( slot + 22, dos_time & 0xFFFF );
	wr16le( slot + 24, dos_time >> 16 );
	wr16le( slot + 26, first );
	slot[28] = (uint8_t)len;
	slot[29] = (uint8_t)( len >> 8 );
	slot[30] = (uint8_t)( len >> 16 );
	slot[31] = (uint8_t)( len >> 24 );
	if ( stored != NULL )
	{
		short_text( sfn, stored );
	}
	return 0;
}

int fimg_fat_read( const uint8_t *img, int index, uint8_t *out, size_t len )
{
	const uint8_t *e = root_file( img, index );
	const uint8_t *fat = img + FAT1_OFF;
	uint32_t c, guard = 0;
	size_t got = 0;

	if ( e == NULL )
	{
		return -1;
	}
	c = rd16le( e + 26 );
	while ( got < len )
	{
		size_t n = len - got < FIMG_SECTOR ? len - got : FIMG_SECTOR;

		if ( c < 2 || c >= FIMG_FAT_DATA_SECTORS + 2 || guard++ > FIMG_FAT_DATA_SECTORS )
		{
			return -1;
		}
		memcpy( out + got, img + DATA_OFF + (size_t)( c - 2 ) * FIMG_SECTOR, n );
		got += n;
		c = fat_get( fat, c );
	}
	return 0;
}

int fimg_fat_entry( const uint8_t *img, int index, char name[13], uint32_t *size )
{
	const uint8_t *e = root_file( img, index );

	if ( e == NULL )
	{
		return -1;
	}
	short_text( e, name );
	*size = e[11] & ATTR_DIR ? 0 : rd32le( e + 28 );
	return 0;
}

uint32_t fimg_fat_sectors( size_t file_len )
{
	return (uint32_t)( ( file_len + FIMG_SECTOR - 1 ) / FIMG_SECTOR );
}

void fimg_inspect( const uint8_t *img, size_t size, fimg_info *out )
{
	uint32_t magic;

	memset( out, 0, sizeof *out );
	if ( size != FIMG_SIZE_144 && size != FIMG_SIZE_720 )
	{
		snprintf( out->text, sizeof out->text,
		          "Not a floppy image: %lu bytes (1.44 MB is 1474560, 720 KB is 737280).",
		          (unsigned long)size );
		return;
	}
	magic = rd32be( img );
	if ( memcmp( img, EOS_MAGIC, EOS_MAGIC_LEN ) == 0 )
	{
		int i;

		out->kind = FIMG_KIND_EOS_BANK;
		memcpy( out->name, img + 0x10, FIMG_BANK_NAME_LEN );
		out->name[FIMG_BANK_NAME_LEN] = 0;
		for ( i = FIMG_BANK_NAME_LEN - 1; i >= 0 && out->name[i] == ' '; i-- )
		{
			out->name[i] = 0;
		}
		memcpy( out->form, img + 0x34, 4 );
		out->form[4] = 0;
		out->disk = (int)rd32be( img + 0x28 );
		out->disks = (int)rd32be( img + 0x2C );
		out->bank_bytes = rd32be( img + 0x30 );
		snprintf( out->text, sizeof out->text,
		          "EOS floppy: bank \"%s\" (%s, %u bytes), disk %d of %d.",
		          out->name, out->form, (unsigned)out->bank_bytes, out->disk, out->disks );
	}
	else if ( magic == 0x12345678 )
	{
		out->kind = FIMG_KIND_UPDATE;
		snprintf( out->text, sizeof out->text,
		          "OS update floppy for the classic boot loader (68020 machines)." );
	}
	else if ( magic == 0x76543210 || magic == 0x76543211 )
	{
		out->kind = FIMG_KIND_UPDATE;
		snprintf( out->text, sizeof out->text,
		          "OS update floppy for the Ultra boot loader: \"%.32s\".",
		          (const char *)img + 0x18 );
	}
	else if ( ( img[0] == 0xE9 || img[0] == 0xEB ) && rd16le( img + 11 ) == FIMG_SECTOR &&
	          rd16le( img + 19 ) == size / FIMG_SECTOR )
	{
		describe_fat( img, out );
	}
	else
	{
		int i, fill = 1;

		for ( i = 0; i < FIMG_SECTOR && fill; i++ )
		{
			fill = img[i] == FILL_BYTE || img[i] == 0;
		}
		if ( fill )
		{
			out->kind = FIMG_KIND_EOS_BLANK;
			snprintf( out->text, sizeof out->text,
			          "EOS floppy, formatted and empty: EOS lists it as \"Empty Bank\"." );
		}
		else
		{
			snprintf( out->text, sizeof out->text,
			          "A floppy image of a kind this program does not know." );
		}
	}
	if ( size == FIMG_SIZE_720 )
	{
		size_t k = strlen( out->text );

		snprintf( out->text + k, sizeof out->text - k,
		          "\n720 KB: no EOS tested reads this size." );
	}
}
