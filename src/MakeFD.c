#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <windows.h>

#include "free_dos.h"

#pragma warning( disable : 4996)
#pragma pack(1)

typedef struct
{
    unsigned char  oem_name[8];         // 3  OEM name
    unsigned short bytes_per_sector;    // 11 512
    unsigned char  sectors_per_cluster; // 13 1
    unsigned short reserved_sectors;    // 14 1
    unsigned char  fat_copies;          // 16 2
    unsigned short root_dir_entries;    // 17 224
    unsigned short total_sectors;       // 19 (18*2*80)
    unsigned char  media_descriptor;    // 21 0f9H, 0f0h, 0fAh, etc.
    unsigned short sectors_per_fat;     // 22 9 
    unsigned short sectors_per_track;   // 24 18
    unsigned short heads_per_cylinder;  // 26 2 
    unsigned long  hidden_sectors;      // 28 always zero for floppies
    unsigned long  total_sectors_2;     // 32 actual value if total_sectors = 0
    unsigned short bpb_reserved;        // 36 BPB reserved (
    unsigned char  volume_id;           // 38 29h - marker for volume ID
    unsigned long  serial_number;       // 39 volume unique ID number
    unsigned char  boot_label[11];      // 43 "SOFTPROBE  "
    unsigned char  fat_name[8];         // 54 "FAT12   "
} disk_pbp_t;

typedef struct
{
    char * fmt;
    char * description;
    unsigned char media;
    // track,head,sectors,sec/cls,#fats,#reserved,#root_dir
    unsigned int geo[7];  
} format_info_t;

format_info_t supported_formats[] =
{
    {"288","3.5-inch 2.88M",0xf0,{80,2,36,2,2,0,228}},
    {"144","3.5-inch 1.44M",0xf0,{80,2,18,1,2,0,228}},
    {"720","3.5-inch 720K", 0xf9,{80,2, 9,1,2,0,228}},
    {"120","5.25-inch 1.2M",0xf9,{80,2,15,1,2,0,228}},
    {"360","5.25-inch 360K",0xfd,{40,2, 9,1,2,0,114}},
    {"320","5.25-inch 320K",0xff,{40,2, 8,1,2,0,114}},
    {"180","5.25-inch 180K",0xfc,{40,1, 9,1,2,0,114}},
    {"160","5.25-inch 160K",0xf8,{40,1, 8,1,2,0,114}},
};
// media descriptos: http://support.microsoft.com/kb/140418
                                        // inch  cap  heads sectors
#define MEDIA_2M88              0xF0    // 3.5   2.88M   2    36
#define MEDIA_1M44              0xF0    // 3.5   1.44M   2    18
#define MEDIA_720K              0xF9    // 3.5   720K    2    9
#define MEDIA_1M2               0xF9    // 5.25  1.2M    2    15
#define MEDIA_360K              0xFD    // 5.25  360K    2    9
#define MEDIA_320K              0xFF    // 5.25  320K    2    8
#define MEDIA_180K              0xFC    // 5.25  180K    1    9
#define MEDIA_160K              0xFE    // 5.25  160K    1    8
#define MEDIA_HARDDISK          0xF8

typedef struct
{
    unsigned char  name[8];     // 00
    unsigned char  ext[3];      // 08
    unsigned char  attrib;      // 11
    unsigned char  low_case;    // 12 NT VFAT lower case flag
    unsigned char  time_100;    // 13 100th of seconds of C-time
    unsigned short create_time; // 14 creation time
    unsigned short create_date; // 16 creation date
    unsigned short access_date; // 18 last access date
    unsigned short clust_high;  // 20 high bytes of first cluster
    unsigned short update_time; // 22 modification time
    unsigned short update_date; // 24 modification date
    unsigned short first_cluster; // 26
    unsigned long  file_size;   // 28
} dir_entry_t;


// name[0]: 00: empty, 05 or E5: deleted
#define DELETED_ENTRY   0xE5
// attrib:
#define ATTR_READONLY   0x01
#define ATTR_HIDDEN     0x02
#define ATTR_SYSTEM     0x04
#define ATTR_VOLUME     0x08
#define ATTR_DIR        0x10
#define ATTR_ARCHIVE    0x20
// low_case
#define LCASE_BASE      0x08    // filename in lower case
#define LCASE_EXT       0x10    // extension in lower case

#define CLUST_FREE  0           /* cluster 0 also means a free cluster */
#define CLUST_FIRST 2           /* first legal cluster number */
#define CLUST_RSRVD 0xfffffff0  /* reserved cluster range */
#define CLUST_BAD   0xfffffff7  /* a cluster with a defect */
#define CLUST_EOFS  0xfffffff8  /* start of eof cluster range */
#define CLUST_EOFE  0xffffffff  /* end of eof cluster range */
#define CLUST_END   CLUST_EOFE  /* bigger than any valid cluster */

#define FAT12_MASK  0x00000fff  /* mask for 12 bit cluster numbers */
#define FAT16_MASK  0x0000ffff  /* mask for 16 bit cluster numbers */
#define FAT32_MASK  0x0fffffff  /* mask for FAT32 cluster numbers */

disk_pbp_t BPB;
dir_entry_t * root_dir = NULL;
dir_entry_t * cur_dir = NULL;
char * image_filename = "";
HANDLE image_handle = INVALID_HANDLE_VALUE;

unsigned long total_sectors, total_clusters, cluster_size, base_data_sector;
unsigned long root_dir_sector, root_dir_size, cur_dir_cluster = 0, cached_cluster = 0, dirs_per_cluster;
unsigned char fat_bits, * FAT = NULL, * aux_cluster;
unsigned char boot_sector[512];

enum {I_READ = 1, I_WRITE = 2, I_DELETE = 4 };    // access types
enum {C_FAT = 1, C_ROOT_DIR = 2, C_CUR_DIR = 4 }; // changed items
char image_access = 0, image_changed = 0;

dir_entry_t * find_dir_entry( char * dos_name );

int error_return( int err )
{
    if( image_filename != NULL )
    {
        CloseHandle( image_handle );
        if( image_access & I_DELETE ) DeleteFile( image_filename );
    }
    exit( err );
    return err;
}

void print_string( char * msg, char * str, int size )
{
    printf( msg );
    fwrite( str, 1, size, stdout );
}

void print_dos_date( char * msg, unsigned int ddate )
{
    printf( "%s %02d-%02d-%04d", msg, 
        (ddate >> 5) & 15, ddate & 31, ((ddate >> 9) & 127) + 1980 );
}

void print_dos_time( unsigned int dtime )
{
    printf( " %02d:%02d", (dtime >> 11) & 31, (dtime >> 5) & 63 );
}

void * aligned_alloc( int size )
{
    char * ptr;
    size = (size + BPB.bytes_per_sector - 1) & ~(BPB.bytes_per_sector - 1);
    ptr = malloc( size );
    if( ptr == NULL )
    {
        printf( "Memory allocation error (%d).\n", size );
        error_return( 4 );
    }
    memset( ptr, 0, size );
    return ptr;
}

#define CLUST2SEC(x) (base_data_sector + (x)*BPB.sectors_per_cluster)

unsigned long get_fat( unsigned int index )
{
    unsigned long i;
    if( index < total_clusters ) switch( fat_bits )
    {
    case 12:
        i = index + (index >> 1);
        i = (FAT[i+1] << 8) | FAT[i];
        if( index & 1 ) i >>= 4;
        i &= 0xfff;
        if( i >= 0xff6 ) i |= ~FAT12_MASK;
        return i;

    case 16:
        i = index << 1;
        i = (FAT[i+1] << 8) | FAT[i];
        if( i >= 0xfff6 ) i |= ~FAT16_MASK;
        return i;
    }
    return CLUST_BAD;
}

void set_fat( unsigned int index, unsigned int value )
{
    unsigned long i;
    if( index < total_clusters ) switch( fat_bits )
    {
    case 12:
        i = index + (index >> 1);
        if( index & 1 )
        {
            FAT[i+1] = (unsigned char)(value >> 4);
            FAT[i] = (unsigned char)(
                (FAT[i] & 0x0f) | ((value << 4) & 0xf0));
        }
        else
        {
            FAT[i++] = (unsigned char)(value);
            FAT[i] = (unsigned char)(FAT[i] & 0xf0) | 
                     (unsigned char)((value >> 8) & 0x0f);
        }
        image_changed |= C_FAT;
        return;

    case 16:
        i = index << 1;
        FAT[i] = (unsigned char)(value);
        FAT[i+1] = (unsigned char)(value >> 8);
        image_changed |= C_FAT;
        return;
    }
    printf( "Invalid cluster %ld\n", index );
    error_return( 5 );
}

unsigned int find_free_cluster( void )
{
    unsigned long i;
    for( i = 2; i < total_clusters; i++ )
    {
        if( get_fat( i ) == 0 )
        {
            set_fat( i, CLUST_EOFS );
            return i;
        }
    }
    printf( "Disk is full.\n" );
    //error_return( 5 );
    return 0;
}

unsigned long calc_free_space( void )
{
    unsigned long i, s;
    for( s = 0, i = 2; i < total_clusters; i++ )
    {
        if( get_fat( i ) == 0 ) s += cluster_size;
    }
    return s;
}

int read_sector( unsigned long sector, int count, unsigned char * buffer )
{
    DWORD n;
    SetFilePointer( image_handle, (LONG)(sector << 9), NULL, FILE_BEGIN );
    if( !ReadFile( image_handle, buffer, (DWORD)(count << 9), &n, NULL ))
    {
        fprintf( stderr, "\nError %d reading sector %ld", GetLastError(), sector );
        return error_return( 5 );
    }
    return 0;
}

int write_sector( unsigned long sector, int count, unsigned char * buffer )
{
    DWORD n;
    if( !(image_access & I_WRITE) ) return error_return( 6 );

    SetFilePointer( image_handle, (LONG)(sector << 9), NULL, FILE_BEGIN );
    if( !WriteFile( image_handle, buffer, (DWORD)(count << 9), &n, NULL ))
    {
        fprintf( stderr, "\nError %d writing sector %ld", GetLastError(), sector );
        return error_return( 7 );
    }
    return 0;
}

int read_cluster( unsigned int cluster, unsigned char * buffer )
{
    return read_sector( CLUST2SEC( cluster ), BPB.sectors_per_cluster, buffer );
}

int write_cluster( unsigned int cluster, unsigned char * buffer )
{
    return write_sector( CLUST2SEC( cluster ), BPB.sectors_per_cluster, buffer );
}

void flush_cached_cluster( void )
{
    if( image_changed & C_CUR_DIR )
    {
        write_cluster( cached_cluster, (unsigned char *)cur_dir );
        image_changed &= ~C_CUR_DIR;
    }
}

void load_cached_cluster( unsigned int new_cluster )
{
    if( new_cluster != cached_cluster )
    {
        flush_cached_cluster();
        cached_cluster = new_cluster;
        read_cluster( cached_cluster, (unsigned char *)cur_dir );
    }
}

int mount_image( unsigned char * boot_sec )
{
    disk_pbp_t * pbp = (disk_pbp_t *)&boot_sec[3];

    if( boot_sec[510] != 0x55 || boot_sec[511] != 0xAA ||
        pbp->bytes_per_sector != 512 )
    {
        printf( "Invalid boot record.\n" );
        return error_return( 2 );
    }

    memcpy( &BPB, pbp, sizeof(BPB) );
    root_dir_sector  = BPB.reserved_sectors + BPB.fat_copies * BPB.sectors_per_fat;
    root_dir_size    = (BPB.root_dir_entries + 15) >> 4; // size in sectors
    base_data_sector = root_dir_sector + root_dir_size - 2*BPB.sectors_per_cluster; // clusters 0 & 1 are reserved
    total_sectors    = BPB.total_sectors;
    if( total_sectors == 0 ) total_sectors = BPB.total_sectors_2;
    total_clusters   = (total_sectors - base_data_sector)/BPB.sectors_per_cluster;
    cluster_size     = BPB.sectors_per_cluster*BPB.bytes_per_sector;
    dirs_per_cluster = cluster_size >> 5;

    switch( pbp->media_descriptor )
    {
    case MEDIA_1M44:    // 0xF0  3.5   1.44M/2.88M  2    18/36
    case MEDIA_720K:    // 0xF9  3.5   720K    2    9
    case MEDIA_360K:    // 0xFD  5.25  360K    2    9
    case MEDIA_320K:    // 0xFF  5.25  320K    2    8
    case MEDIA_180K:    // 0xFC  5.25  180K    1    9
    case MEDIA_160K:    // 0xFE  5.25  160K    1    8
    case MEDIA_HARDDISK:// 0xF8
        if( total_clusters >= 0xff0 )
        {
            if( total_clusters >= 0xfff0 )
            {
                printf( "Unsupported media format.\n" );
                return error_return( 2 );
            }
            fat_bits = 16;
        }
        else
            fat_bits = 12;
        break;

    default:
        printf( "Unsupported media type (%02X)\n", pbp->media_descriptor );
        return error_return( 3 );
    }

    if( FAT       != NULL ) free( FAT );
    if( root_dir  != NULL ) free( root_dir );
    if( cur_dir   != NULL ) free( cur_dir );
    if( aux_cluster != NULL ) free( aux_cluster );

    FAT       = aligned_alloc( (total_clusters * fat_bits + 7) >> 3 );
    root_dir  = aligned_alloc( BPB.root_dir_entries * sizeof(dir_entry_t) );
    cur_dir   = aligned_alloc( cluster_size );
    aux_cluster = aligned_alloc( cluster_size );

    cur_dir_cluster = 0;    // 0 means root dir selected
    cached_cluster = 0;     // cache empty
    return 0;
}

int close_media( void )
{
    unsigned char i;
    if( image_access & I_WRITE )
    {
        if( image_changed & C_FAT ) for( i = 0; i < BPB.fat_copies; i++ )
        {
            write_sector( BPB.reserved_sectors + i*BPB.sectors_per_fat, 
                          BPB.sectors_per_fat, 
                          FAT );
        }
        if( image_changed & C_ROOT_DIR )
            write_sector( root_dir_sector, root_dir_size, (unsigned char *)root_dir );

        flush_cached_cluster();
    }
    CloseHandle( image_handle );
    image_handle = INVALID_HANDLE_VALUE;
    image_changed = 0;
    image_access = 0;
    return 0;
}

dir_entry_t * alloc_new_dir_entry( void )
{
    unsigned int c;

    if( cur_dir_cluster == 0 )
    {   // we are on root dir
        for( c = 0; c < BPB.root_dir_entries; c++ )
        {
            if( root_dir[c].name[0] == DELETED_ENTRY || root_dir[c].name[0] == 0x00 )
            {
                memset( &root_dir[c], 0, sizeof(dir_entry_t) );
                root_dir[c].name[0] = DELETED_ENTRY;            // reserve entry
                image_changed |= C_ROOT_DIR;
                return &root_dir[c];
            }
        }
        printf( "No more room left on root dir.\n" );
        return NULL;
    }

    // we are on a non-root DIR
    c = cur_dir_cluster;

    do
    {
        load_cached_cluster( c );
        for( c = 0; c < dirs_per_cluster; c++ )
        {
            if( cur_dir[c].name[0] == DELETED_ENTRY || cur_dir[c].name[0] == 0x00 )
            {
                memset( &cur_dir[c], 0, sizeof(dir_entry_t) );
                cur_dir[c].name[0] = DELETED_ENTRY;
                image_changed |= C_CUR_DIR;
                return &cur_dir[c];
            }
        }

        // current cluster is full, get next
        c = get_fat( cached_cluster );
    } while( c < CLUST_RSRVD );

    // end of chain, allocate new cluster
    c = find_free_cluster();
    if( c == 0 ) return NULL;       // disk full ?
    set_fat( cached_cluster, c );
    flush_cached_cluster();
    cached_cluster = c;
    memset( cur_dir, 0, cluster_size );
    cur_dir[0].name[0] = DELETED_ENTRY;
    image_changed |= C_CUR_DIR;
    return &cur_dir[0];
}

char * get_dos_name( char * name, char * dos_name )
{
    int i = 0;
    while( i < 8 )
    {
        if( *name == '\0' || *name == '.' || *name == '\\' ) break;
        dos_name[i++] = toupper( *name++ );
    }
    while( i < 8 ) dos_name[i++] = ' ';
    while( *name != '\0' && *name != '.' && *name != '\\' ) name++;
    if( *name == '.' ) 
    {
        name++;
        while( i < 11 )
        {
            if( *name == '\0' || *name == '\\' ) break;
            dos_name[i++] = toupper( *name++ );
        }
    }
    while( i < 11 ) dos_name[i++] = ' ';

    return name;
}

void get_dos_file_data( dir_entry_t * pde, WIN32_FIND_DATA * ffd )
{
    char * fname = ffd->cFileName;

    if( ffd->cAlternateFileName[0] > ' ' ) fname = ffd->cAlternateFileName;
    get_dos_name( fname, pde->name );

    FileTimeToDosDateTime( &ffd->ftLastAccessTime, &pde->access_date, &pde->update_time );
    FileTimeToDosDateTime( &ffd->ftLastWriteTime, &pde->update_date, &pde->update_time );
    FileTimeToDosDateTime( &ffd->ftCreationTime, &pde->create_date, &pde->create_time );
}

int add_new_directory( char * dir_path, int attr )
{
    dir_entry_t de = {0};
    dir_entry_t * pde;
    char * dir_name = dir_path;

    printf( "Creating directory %s ... ", dir_path );
    de.attrib = (unsigned char)attr;

    do
    {
        if( dir_name[0] == '\\' ) dir_name++;
        dir_name = get_dos_name( dir_name, (char *)&de.name[0] );
        if( de.name[0] == ' ' && de.ext[0] == ' ' ) break;

        pde = find_dir_entry( &de.name[0] );
        if( pde != NULL )
        {   // dir exists, change dir
            cur_dir_cluster = pde->first_cluster;
        }
        else
        {   // dir entry does not exists, create it
            pde = alloc_new_dir_entry();
            if( pde == NULL ) return 3;
            de.first_cluster = find_free_cluster();
            *pde = de;
            flush_cached_cluster();

            memset( cur_dir, 0, cluster_size );
            // Create . and ..
            memcpy( cur_dir[0].name, ".          ", 11 );
            memcpy( cur_dir[1].name, "..         ", 11 );
            cur_dir[0].attrib = ATTR_DIR;
            cur_dir[1].attrib = ATTR_DIR;
            cur_dir[0].first_cluster = de.first_cluster;
            cur_dir[1].first_cluster = (unsigned short)cur_dir_cluster; // parent dir

            // select newly created dir
            cur_dir_cluster = cached_cluster = de.first_cluster;
            image_changed |= C_CUR_DIR;
        }
    } while( dir_name[0] == '\\' );

    //write_cluster( de.first_cluster, aux_cluster );
    printf( "done\n" );
    return 0;
}

int add_new_file( char * filename, int attr, WIN32_FIND_DATA * ffd );
int add_new_files( char * filename, int attr )
{
    int rc = 0;
    HANDLE hFind;
    WIN32_FIND_DATA ffd;
    char full_path[MAX_PATH], * fname = NULL;

    GetFullPathName( filename, sizeof(full_path), full_path, &fname );
    if( fname == NULL )
    {
        fprintf( stderr, "Invalid filespec.\n" );
        return 1;
    }

    hFind = FindFirstFile( (LPCSTR)full_path, &ffd );
    if( hFind == INVALID_HANDLE_VALUE )
    {
        fprintf( stderr, "No file was found.\n" );
        return 1;
    }
    do
    {
        if( strlen( ffd.cFileName ) > 2 || ffd.cFileName[0] != '.' )
        {
            strcpy( fname, ffd.cFileName );
            rc = add_new_file( full_path, attr, &ffd );
        }
    } while( rc == 0 && FindNextFile( hFind, &ffd ));
    FindClose( hFind );
    return rc;
}

int add_new_file( char * filename, int attr, WIN32_FIND_DATA * ffd )
{
    // ffd.ftCreationTime, ffd.nFileSizeLow, ffd.cAlternateFileName
    dir_entry_t de = {0};
    dir_entry_t * pde;
    unsigned int c1, c2;
    unsigned long file_size, free_space, n;
    HANDLE file_handle;

    printf( "Adding file %s ... ", ffd->cFileName );
    file_handle = CreateFile( filename, GENERIC_READ, FILE_SHARE_READ, 
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if( file_handle == INVALID_HANDLE_VALUE )
    {
        fprintf( stderr, "Cannot open file %s\n", filename );
        return 1;
    }

    // copy dos filename and dates
    get_dos_file_data( &de, ffd );
    if( find_dir_entry( &de.name[0] ) != NULL )   // check for duplicates
    {
        printf( "File %s already exists.\n", ffd->cFileName );
        return 1;
    }

    pde = alloc_new_dir_entry();
    if( pde == NULL ) return 1;

    de.file_size = file_size = ffd->nFileSizeLow;
    de.attrib = (unsigned char)attr;
    free_space = calc_free_space();
    if( ffd->nFileSizeHigh != 0 || de.file_size > free_space )
    {
        printf( "Not enough space left (%lu available %lu needed)\n", 
            free_space, de.file_size );
        CloseHandle( file_handle );
        return 1;
    }
    if( file_size > 0 )
    {
        c1 = 0;
        while( file_size > 0 )
        {
            c2 = find_free_cluster();
            n = (file_size > cluster_size) ? cluster_size : file_size;
            file_size -= n;
            if( !ReadFile( file_handle, aux_cluster, n, &n, NULL ))
            {
                printf( "Error %d reading file %s\n", GetLastError(), filename );
                CloseHandle( file_handle );
                return 1;
            }
            memset( aux_cluster+n, 0, cluster_size - n );
            write_cluster( c2, aux_cluster );
            if( c1 == 0 )
                de.first_cluster = c2;
            else
                set_fat( c1, c2 );
            c1 = c2;
        }
        set_fat( c1, CLUST_EOFS );
    }
    CloseHandle( file_handle );

    // now add de to current DIR
    *pde = de;
    printf( "done.\n" );

    return 0;
}

int open_media( int access )
{
    unsigned char buff[512];

    if( image_handle != INVALID_HANDLE_VALUE ) close_media();
    image_access = access;
    image_changed = 0;

    image_handle = CreateFile( image_filename, 
        (access & I_WRITE) ? (GENERIC_WRITE | GENERIC_READ) : GENERIC_READ, 
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if( image_handle == INVALID_HANDLE_VALUE )
    {
        fprintf( stderr, "Cannot open file: %s\n", image_filename );
        return error_return( 1 );
    }

    BPB.bytes_per_sector = 512; // default size for boot sector
    read_sector( 0, 1, buff );

    mount_image( buff );
    read_sector( BPB.reserved_sectors, BPB.sectors_per_fat, FAT );
    read_sector( root_dir_sector, root_dir_size, (unsigned char *)root_dir );
    return 0;
}

#define n_fmts (sizeof(supported_formats)/sizeof(supported_formats[0]))

int create_media( char * fmt, char * boot_file, int overwrite )
{
    int n;
    unsigned int i;
    disk_pbp_t * pbp;
    HANDLE file_handle = INVALID_HANDLE_VALUE;
    
    if( boot_file )
    {   // boot sector file defined, use it
        file_handle = CreateFile( boot_file, GENERIC_READ, FILE_SHARE_READ, 
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
        if( file_handle == INVALID_HANDLE_VALUE )
        {
            printf( "\nError %d openning file %s", GetLastError(), boot_file );
            return error_return( 5 );
        }
        if( !ReadFile( file_handle, boot_sector, 512, &i, NULL ))
        {
            printf( "\nError %d reading file %s", GetLastError(), boot_file );
            CloseHandle( file_handle );
            return error_return( 5 );
        }
    }
    else
    {   // no template defined, use freedos
        memcpy( boot_sector, freedos_boot_sec, 512 );
    }
    if( fmt[0] )
    {
        format_info_t format;
        for( i = 0; i < n_fmts && strcmp( fmt, supported_formats[i].fmt ) != 0; i++ );
        if( i < n_fmts )
        {   // one of standard formats selected
            format = supported_formats[i];
        }
        else
        {   // it should be a custom format (c,h,s,...)
            if( *fmt != '(' )
            {
                fprintf( stderr, "Unknown media format: %s\n", fmt );
                return error_return( 1 );
            }
            format = supported_formats[1];  // use 1.44M as default
            format.fmt = "Custom";
            format.description = "Custom format";
            for( i = 0; i < 7; i++ )
            {
                fmt++;
                if( *fmt >= '0' && *fmt <= '9' )
                {
                    format.geo[i] = 0; 
                    do
                    {
                        format.geo[i] = 10*format.geo[i] + (*fmt++) - '0'; 
                    } while( *fmt >= '0' && *fmt <= '9' );
                }
                if( *fmt == ')' ) break;
                if( *fmt != ',' )
                {
                    fprintf( stderr, "Invalid custom format\n" );
                    if( file_handle != INVALID_HANDLE_VALUE ) CloseHandle( file_handle );
                    return error_return( 1 );
                }
            }
            // validate custom format (range valid for INT 13, ah=2)
            if( format.geo[0] < 1 || format.geo[0] > 1024   ||  // #tracks
                format.geo[1] < 1 || format.geo[1] > 255    ||  // heads/track
                format.geo[2] < 1 || format.geo[2] > 63     ||  // sectors/track
                format.geo[3] < 1 || format.geo[3] > 255    ||  // sectors/cluster
                format.geo[4] < 1 || format.geo[4] > 2      ||  // fat copies
                format.geo[5] > 1023 )                          // reserved sectors
            {
                if( file_handle != INVALID_HANDLE_VALUE ) CloseHandle( file_handle );
                fprintf( stderr, "Invalid custom data.\n" );
                return error_return( 1 );
            }
        }

        // apply the format
        pbp = (disk_pbp_t *)&boot_sector[3];
        total_sectors = format.geo[0]*format.geo[1]*format.geo[2]; // tracks*heads*sectors
        pbp->bytes_per_sector    = 512;
        pbp->heads_per_cylinder  = (unsigned short)format.geo[1];
        pbp->sectors_per_track   = (unsigned short)format.geo[2];
        pbp->sectors_per_cluster = (unsigned char)format.geo[3];
        pbp->fat_copies          = (unsigned char)format.geo[4];
        pbp->reserved_sectors    = (unsigned short)(format.geo[5] + 1);
        pbp->root_dir_entries    = (format.geo[6] + 15) & ~15;

        if( total_sectors < 0x10000 )
        {
            pbp->total_sectors = (unsigned short)total_sectors;
            pbp->total_sectors_2 = 0;
        }
        else
        {
            pbp->total_sectors = 0;
            pbp->total_sectors_2 = total_sectors;
        }
        pbp->sectors_per_fat = 0;
        do
        {
            pbp->sectors_per_fat++;
            // calculate approx number of clusters
            n = ((total_sectors - pbp->reserved_sectors - (pbp->root_dir_entries >> 4) - 
                pbp->fat_copies*pbp->sectors_per_fat)/pbp->sectors_per_cluster) + 2;
            if( n < 100 )
            {
                if( file_handle != INVALID_HANDLE_VALUE ) CloseHandle( file_handle );
                fprintf( stderr, "Invalid format definition.\n" );
                return error_return( 1 );
            }
            if( n >= 0xff0 )
                n *= 4; // 16-bit fat
            else
                n *= 3; // 12-bit fat
            i = (n + 1023) >> 10;   // number of sectors needed for it
        } while( i > pbp->sectors_per_fat );
    }

    image_handle = CreateFile( image_filename, GENERIC_WRITE, 
        FILE_SHARE_READ, NULL, 
        overwrite ? CREATE_ALWAYS : CREATE_NEW, 
        FILE_ATTRIBUTE_NORMAL, NULL );
    if( image_handle == INVALID_HANDLE_VALUE )
    {
        if( file_handle != INVALID_HANDLE_VALUE ) CloseHandle( file_handle );
        fprintf( stderr, "Error creating file: %s\n", image_filename );
        return error_return( 1 );
    }

    image_access = I_READ | I_WRITE | I_DELETE; // delete on error
    image_changed = C_FAT | C_ROOT_DIR;
    mount_image( boot_sector );
    memcpy( &boot_sector[54], "FAT12   ", 8 ); // fat_name[8];  
    if( fat_bits == 16 ) boot_sector[54+4] = '6';

    // first two entries are reserved
    set_fat( 0, CLUST_RSRVD );                  // fff0
    set_fat( 1, CLUST_END );                    // ffff

    write_sector( 0, 1, boot_sector );          // write boot sector
    for( i = 1; i < BPB.reserved_sectors; i++ )
    {
        if( file_handle != INVALID_HANDLE_VALUE )
            ReadFile( file_handle, aux_cluster, 512, &n, NULL );
        write_sector( i, 1, aux_cluster );      // write 00's as boot reserved sectors
    }
    for( i = 0; i < BPB.fat_copies; i++ )
    {
        write_sector( BPB.reserved_sectors + i*BPB.sectors_per_fat, 
                      BPB.sectors_per_fat, 
                      FAT );
    }

    write_sector( root_dir_sector, root_dir_size, (unsigned char *)root_dir );

    for( i = 2; i < total_clusters; i++ )
        write_cluster( i, (unsigned char *)aux_cluster );

    if( file_handle != INVALID_HANDLE_VALUE ) CloseHandle( file_handle );
    CloseHandle( image_handle );
    image_handle = INVALID_HANDLE_VALUE;
    image_changed = 0;
    image_access = 0;
    return 0;
}

int set_volume_label( char * label )
{
    dir_entry_t * pde;
    unsigned int n, saved_dir;

    if( root_dir[0].name[0] != DELETED_ENTRY && 
        root_dir[0].name[0] != 0x00 &&
        (root_dir[0].attrib & ATTR_VOLUME) == 0 )  // is label currently there?
    {
        saved_dir = cur_dir_cluster;
        cur_dir_cluster = 0;    // force root dir
        pde = alloc_new_dir_entry();
        cur_dir_cluster = saved_dir;
        if( pde == NULL )return 1;
        *pde = root_dir[0];
    }
    n = (unsigned int)strlen( label );
    if( n > 11 ) n = 11;
    memset( &root_dir[0], 0, sizeof(root_dir[0]) );
    strncpy( root_dir[0].name, label, n );
    while( n < 11 ) root_dir[0].name[n++] = ' ';
    root_dir[0].attrib = ATTR_VOLUME;
    image_changed |= C_ROOT_DIR;

    return 0;
}

dir_entry_t * find_dir_entry( char * dos_name )
{
    unsigned int c;

    if( cur_dir_cluster == 0 )
    {   // we are on root dir
        for( c = 0; c < BPB.root_dir_entries; c++ )
        {
            if( root_dir[c].name[0] == DELETED_ENTRY ) continue;
            if( root_dir[c].name[0] == 0x00 ) return NULL;
            if( memcmp( root_dir[c].name, dos_name, 11 ) == 0 ) return &root_dir[c];
        }
        return NULL;
    }

    // we are on a non-root DIR
    c = cur_dir_cluster;
    do
    {
        load_cached_cluster( c );
        for( c = 0; c < dirs_per_cluster; c++ )
        {
            if( cur_dir[c].name[0] == DELETED_ENTRY ) continue;
            if( cur_dir[c].name[0] == 0x00 ) return NULL;
            if( memcmp( cur_dir[c].name, dos_name, 11 ) == 0 ) return &cur_dir[c];
        }
        c = get_fat( cached_cluster );
    } while( c < CLUST_RSRVD );
    return NULL;     // end of chain
}

int extract_file( dir_entry_t * pde, char * dst_path, char * buffer )
{
    char fname[MAX_PATH+12], *p;
    unsigned int i, n;
    int rc = 0;
    HANDLE file_handle;
    DWORD nret;

    if( pde->attrib & (ATTR_VOLUME | ATTR_DIR) ) return 0;
    strncpy( fname, dst_path, MAX_PATH );
    i = (unsigned int)strlen( fname );
    if( fname[i-1] != '\\' ) fname[i++] = '\\';
    for( n = 0, p = pde->name; n < 8 && *p != ' '; n++ ) fname[i++] = *p++;
    fname[i++] = '.';
    for( n = 0, p = pde->ext; n < 3 && *p != ' '; n++ ) fname[i++] = *p++;
    fname[i] = 0;
    printf( "Extracting %s\n", fname );
    file_handle = CreateFile( fname, GENERIC_READ | GENERIC_WRITE, 
        FILE_SHARE_READ | FILE_SHARE_WRITE, 
        NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
    if( file_handle == INVALID_HANDLE_VALUE )
    {
        fprintf( stderr, "File creation failed (%d)\n", GetLastError() );
        return 1;
    }

    i = pde->first_cluster;
    n = pde->file_size;
    while( n > 0 )
    {
        read_cluster( i, buffer );

        if( n <= cluster_size ) 
        {
            if( !WriteFile( file_handle, buffer, n, &nret, NULL ) || nret != n )
            {
                CloseHandle( file_handle );
                printf( "Error %d wrting file %s\n", GetLastError(), fname );
                error_return( 6 );
            }
            break;
        }
        if( !WriteFile( file_handle, buffer, cluster_size, &nret, NULL ) || nret != cluster_size )
        {
            CloseHandle( file_handle );
            printf( "Error %d wrting file %s\n", GetLastError(), fname );
            error_return( 6 );
        }
        n -= cluster_size;
        i = get_fat( i );
    }
    CloseHandle( file_handle );
    return rc;
}

int extract_files( char * dst_path )
{
    int rc = 0;
    unsigned int c;
    char * buff = malloc( cluster_size );
    if( buff == NULL )
    {
        printf( "memory allocation error\n" );
        return 1;
    }

    if( cur_dir_cluster == 0 )
    {   // we are on root dir
        for( c = 0; c < BPB.root_dir_entries; c++ )
        {
            if( root_dir[c].name[0] == DELETED_ENTRY ) continue;
            if( root_dir[c].name[0] == 0x00 ) break;
            rc = extract_file( &root_dir[c], dst_path, buff );
            if( rc ) break;
        }
        free( buff );
        return rc;
    }

    // we are on a non-root DIR
    c = cur_dir_cluster;
    do
    {
        load_cached_cluster( c );
        for( c = 0; c < dirs_per_cluster; c++ )
        {
            if( cur_dir[c].name[0] == DELETED_ENTRY ) continue;
            if( cur_dir[c].name[0] == 0x00 ) return 0;
            rc = extract_file( &cur_dir[c], dst_path, buff );
            if( rc )
            {
                free( buff );
                return rc;
            }
        }
        c = get_fat( cached_cluster );
    } while( c < CLUST_RSRVD );
    free( buff );
    return 0;     // end of chain
}

int select_dir( char * dir )
{
    char dos_name[16];
    dir_entry_t * pde;

    if( *dir == '\\' )
    {
        cur_dir_cluster = 0;    // start from root
        dir++;
    }

    while( *dir )
    {
        dir = get_dos_name( dir, dos_name );
        if( dos_name[0] == ' ' ) break;
        pde = find_dir_entry( dos_name );
        if( pde == NULL ) return 1;         // path not found
        if( !(pde->attrib & ATTR_DIR) ) return 2; // target is file not dir
        cur_dir_cluster = pde->first_cluster;

        if( *dir != '\\' ) break;
        dir++;
    }
    return 0;   // success
}

void print_number( unsigned long val )
{
    unsigned long n = 1000000000;
    while( val < n && n > 1000 )
    {
        printf( "    " );
        n /= 1000;
    }
    n /= 1000;
    printf( "%3d", val/n );
    val %= n;
    while( n > 1 )
    {
        n /= 1000;
        printf( ",%03d", val/n );
    }
}


int print_file_info( dir_entry_t * pde )
{
    int i;
    char buff[16];
    if( pde->name[0] == DELETED_ENTRY ) return 2;
    if( pde->name[0] == 0x00 ) return 1;
    strncpy( buff, pde->name, 8 );
    strncpy( buff+9, pde->ext, 3 ); buff[12] = 0;
    for( i = 0; i < 12; i++ ) if( buff[i] < 0x20 ) buff[i] = '?';
    buff[8] = 0;
    
/*    printf( "\n %04X%04X %04X%04X %04X %02X %04X %s", 
        pde->create_date, pde->create_time, 
        pde->update_date, pde->update_time, pde->access_date, 
        pde->attrib, pde->first_cluster, buff );
        */
    print_dos_date( "\n", pde->create_date );
    print_dos_time( pde->create_time );
    print_dos_date( " ", pde->update_date );
    print_dos_time( pde->update_time );
    //print_dos_date( " ", pde->access_date );

    printf( "  %02X  %04X  %s", 
        pde->attrib, pde->first_cluster, buff );
    if( pde->attrib & ATTR_VOLUME )
        printf( "%s      <LABEL>", buff+9 );
    else
        if( pde->attrib & ATTR_DIR )
            printf( " %s       <DIR>", buff+9 );
        else
        {
            printf( " %s  %10lu", buff+9, pde->file_size );
        }
    return 0;
}

int list_files( char * dir )
{
    unsigned long nfiles = 0, ndirs = 0, total_used = 0;

    int c = select_dir( dir );
    if( c != 0 ) return c;

    printf( "\n Image name ........ : %s", image_filename );
    if( root_dir[0].attrib & ATTR_VOLUME ) print_string(
            "\n Volume label ...... : ", root_dir[0].name, 11 );
    print_string( 
            "\n Boot label ........ : ", BPB.boot_label, 11 );
    printf( "\n Serial number ..... : %08X\n", BPB.serial_number );

    printf( 
        "\n ----------------  ---------------- ---- ----  -------- ---  ----------" 
        "\n Creation Time     Last Update Time Attr Clst  Name     Ext  Size/Type"
        "\n ----------------  ---------------- ---- ----  -------- ---  ----------" );

    if( cur_dir_cluster == 0 )
    {
        for( c = 0; c < BPB.root_dir_entries; c++ )
        {
            if( root_dir[c].attrib & ATTR_VOLUME ) continue;
            if( print_file_info( &root_dir[c] ) == 1 ) goto end;
            if( root_dir[c].attrib & ATTR_DIR )
                ndirs++;
            else
            {
                nfiles++;
                total_used += root_dir[c].file_size;
            }
        }
    }
    else
    {
        c = cur_dir_cluster;
        do
        {
            load_cached_cluster( c );
            for( c = 0; (unsigned int)c < dirs_per_cluster; c++ )
            {
                if( cur_dir[c].attrib & ATTR_VOLUME ) continue;
                if( print_file_info( &cur_dir[c] ) == 1 ) goto end;
                if( cur_dir[c].attrib & ATTR_DIR )
                    ndirs++;
                else 
                {
                    nfiles++;
                    total_used += cur_dir[c].file_size;
                }
            }
            c = get_fat( cached_cluster );
        } while( c < CLUST_RSRVD );
    }
end:
    printf( "\n\n %5lu file(s)      %10lu bytes\n", nfiles, total_used );
    printf( " %5lu dir(s)       %10lu bytes free\n", ndirs, calc_free_space() );
    return 0;
}

int Usage( void )
{
    printf( 
        "\nImageFD version 1.2 by Mehdi Sotoodeh   mehdisotoodeh@gmail.com"
        "\nUsage: ImageFD IMAGE <cmd> [<options>] [<args>]"
        "\nSupported commands are:"
        "\n  I                          Display IMAGE info"
        "\n  L [DIR]                    List files in selected DIR"
        "\n  A [-a<attr>] [-d DIR] FILE [FILE ...]"
        "\n                             Add files, directories or label"
//      "\n  D [-d DIR] FILE [FILE ...] Delete file(s)"
        "\n  X [-d DIR] [DEST]          Extract files from DIR to DEST"
        "\n  F<fmt> [-o] [BOOTSEC]      Format IMAGE"
        "\n"
        "\n <fmt> options:              <attr> options:"
        "\n  288   2.88M 3.5-inch         r    read-only"
        "\n  144   1.44M 3.5-inch         h    hidden"
        "\n  720   720K 3.5-inch          s    system"
        "\n  120   1.2M 5.25-inch         a    archive"
        "\n  360   360K 5.25-inch         d    directory"
        "\n  320   320K 5.25-inch         v    volume label"
        "\n  180   180K 5.25-inch"
        "\n  160   160K 5.25-inch"
        "\n  (c,h,s,...) Custom format" // tracks,heads,secs/track,sec/cls,#fats,#reserved,#root_dir
        "\n" );
    return 1;
}

//unsigned char program_full_path[MAX_PATH], * program_name;

int main( int argc, char ** argv )
{
    int rc, i, a;

    if( argc < 3 ) return Usage();

    //GetFullPathName( argv[0], sizeof(program_full_path), &program_full_path[0], &program_name );
    argv++; argc--;

    image_filename = argv[0];
    argv++; argc--;             // IMAGE

    switch( argv[0][0] )        // <cmd>
    {
    case 'F':   // F<fmt> [-o] [BOOTSEC] // format new image
        if( argc > 1 && argv[1][0] == '-' && argv[1][1] == 'o' )
            return create_media( argv[0]+1, (argc > 2) ? argv[2] : NULL, 1 );

        return create_media( argv[0]+1, (argc > 1) ? argv[1] : NULL, 0 );

    case 'A':   // a [-a<attr>] [-d DIR] FILE    // add file(s) to IMAGE
        if( argc < 2 ) return Usage();
        open_media( I_READ | I_WRITE );
        a = rc = 0;
        while( argc > 1 )
        {
            argc--; argv++;
            if( argv[0][0] == '-' )
            {
                switch( argv[0][1] )
                {
                case 'a':   // [-a<attr>]
                    for( a = 0, i = 2; argv[0][i]; i++ ) switch( tolower( argv[0][i] ))
                    {
                        case 'r': a |= ATTR_READONLY; break;
                        case 'a': a |= ATTR_ARCHIVE;  break;
                        case 's': a |= ATTR_SYSTEM;   break;
                        case 'h': a |= ATTR_HIDDEN;   break;
                        case 'd': a |= ATTR_DIR;      break;
                        case 'v': a |= ATTR_VOLUME;   break;

                        default:
                            printf( "Unsupported attribute (%c).\n", argv[0][i] );
                            error_return( 1 );
                    }
                    break;
                case 'd':   // [-d DIR]
                    if( argc < 2 ) return Usage();
                    argv++; argc--;
                    select_dir( argv[0] );
                    break;
                default:
                    printf( "Unknown option (%c).\n", argv[0][1] );
                    error_return( 1 );
                }
            }
            else
            {
                if( a & ATTR_VOLUME )   rc = set_volume_label( argv[0] );
                else if( a & ATTR_DIR ) rc = add_new_directory( argv[0], a );
                else                    rc = add_new_files( argv[0], a );  // accepts wildcards
                if( rc != 0 ) break;
            }
        }

        i = close_media();
        return rc ? rc : i;

    case 'L':   // l [DIR]
        open_media( I_READ );
        return list_files( ( argc > 1 ) ? argv[1] : "\\" );

//    case 'D':   // D [-d DIR] FILE
    case 'X':   // X [-d DIR] [DEST]
        open_media( I_READ );
        while( argc > 1 )
        {
            argc--; argv++;
            if( argv[0][0] != '-' ) break;
            switch( argv[0][1] )
            {
            case 'd':   // [-d DIR]
                if( argc < 2 ) return Usage();
                argv++; argc--;
                select_dir( argv[0] );
                break;
            default:
                printf( "Unknown option (%c).\n", argv[0][1] );
                error_return( 1 );
            }
        }
        rc = extract_files( ( argc > 1 ) ? argv[1] : ".\\" );
        i = close_media();
        return rc ? rc : i;

    case 'I':   // imahe info
        open_media( I_READ );
        printf( "\n Image name ....................... : %s", image_filename );
        if( root_dir[0].attrib & ATTR_VOLUME )
            print_string( "\n Volume label ..................... : ", 
                root_dir[0].name, 11 );
        print_string( "\n Boot label ....................... : ", 
            BPB.boot_label, 11 );
        printf( "\n Volume serial number ............. : %08X", 
            BPB.serial_number );
        printf( "\n Media descriptor ................. : %02X", 
            BPB.media_descriptor );
        printf( "\n Tracks/heads/sectors ............. : %d/%d/%d", 
            total_sectors/(BPB.heads_per_cylinder*BPB.sectors_per_track), 
            BPB.heads_per_cylinder, BPB.sectors_per_track );
        printf( "\n Bytes per sector ................. : %d", 
            BPB.bytes_per_sector );
        printf( "\n Total sectors .................... : %d", 
            total_sectors );
        printf( "\n Usable sectors ................... : %d", 
            total_sectors - base_data_sector - 2*BPB.sectors_per_cluster );
        printf( "\n Image size ....................... : %d", 
            total_sectors*BPB.bytes_per_sector );

        printf( "\n Reserved sectors ................. : %d", 
            BPB.reserved_sectors );
        printf( "\n Root directory entries ........... : %d (%d sectors)", 
            BPB.root_dir_entries, root_dir_size );
        printf( "\n Number of FATs ................... : %d, %d sectors/FAT", 
            BPB.fat_copies, BPB.sectors_per_fat );
        printf( "\n FAT width ........................ : %d-bits", fat_bits );
        print_string( ", ", BPB.fat_name, 8 );
        printf( "\n Cluster size ..................... : %d sector(s), %d bytes", 
            BPB.sectors_per_cluster, cluster_size );
        printf( "\n Total clusters ................... : %d, %d bytes\n", 
            total_clusters-2, (total_clusters-2)*cluster_size );
        return 0;
    }

    return Usage();
}

