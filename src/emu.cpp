
/* Copyright (c) 2020, Peter Barrett
**
** Permission to use, copy, modify, and/or distribute this software for
** any purpose with or without fee is hereby granted, provided that the
** above copyright notice and this permission notice appear in all copies.
**
** THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
** WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
** WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR
** BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES
** OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
** WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
** ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
** SOFTWARE.
*/

#include "emu.h"
#include "math.h"
using namespace std;

// set false by mount_filesystem() in esp_8_bit.cpp if SPIFFS failed to mount
bool _spiffs_mounted = true;

// Map files into memory for carts bigger than physical RAM
// Handly for NES/SMS carts
// Uses app1 as a cache with a crappy FS on top - default arduino config gives 1280k

#ifdef ESP_PLATFORM
#include <esp_spi_flash.h>
#include <esp_attr.h>
#include <esp_partition.h>

// only map 1 file at a time
spi_flash_mmap_handle_t _file_handle = 0;

static void print_part(const esp_partition_t *pPart)
{
    printf("main: partition type = %d.\n", pPart->type);
    printf("main: partition subtype = %d.\n", pPart->subtype);
    printf("main: partition starting address = %x.\n", pPart->address);
    printf("main: partition size = %x.\n", pPart->size);
    printf("main: partition label = %s.\n", pPart->label);
    printf("main: partition encrypted = %d.\n", pPart->encrypted);
    printf("\n");
}

static void print_parts(esp_partition_iterator_t it)
{
    while (it)
    {
        print_part((esp_partition_t *) esp_partition_get(it));
        it = esp_partition_next(it);
    }
}

typedef struct {
    uint32_t sig;
    uint32_t offset;
    uint32_t len;
    uint32_t flags;
    char name[128-16];
} FlashFile;

#define FSIG ('F' | ('I' << 8) | ('L' << 16) | ('E' << 24))

class CrapFS {
public:
    #define DIR_BLOCK_SIZE 0x4000   // 16k or 128 entries
    // skip 64K at start of partition

    const esp_partition_t* _part;
    uint8_t *_buf;
    FlashFile* _dir;
    int _count;

    CrapFS() : _buf(0),_count(0)
    {
        _part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "app1");
        if (!_part) {
            printf("CrapFS::CrapFS app1 not found\n");
            return;
        }
        print_part(_part);

        _buf = new uint8_t[DIR_BLOCK_SIZE];
        if (!_buf)
            return;
        _dir = (FlashFile*)_buf;
        esp_err_t err = esp_partition_read(_part, 0, _buf, DIR_BLOCK_SIZE);
        if (err) {
            printf("CrapFS::CrapFS dir read failed %d\n",err);
            return;
        }

        // init directory if required
        _count = DIR_BLOCK_SIZE/sizeof(FlashFile);
        if (_dir[0].sig != FSIG)
            reformat();

        // dump dir
        for (int i = 0; i < _count; i++) {
            if (_dir[i].sig == FSIG)
                printf("%08X %08X %s\n",_dir[i].offset,_dir[i].len,_dir[i].name);
        }
    }

    ~CrapFS()
    {
        delete _buf;
    }

    uint32_t align(uint32_t n)
    {
        return (n + 0xFFFF) & 0xFFFF0000;
    }

    void reformat()
    {
        esp_partition_erase_range(_part,0,_part->size); // erase entire partition
        memset(_buf,0xFF,sizeof(DIR_BLOCK_SIZE));       // only use the first 16k of first block
    }

    uint8_t* mmap(const FlashFile* file)
    {
        if (!file)
            return 0;
        printf("CrapFS::mmap mapping %s offset:%08X len:%d\n",file->name,file->offset,file->len);
        void* data = 0;
        if (esp_partition_mmap(_part, file->offset, file->len, SPI_FLASH_MMAP_DATA, (const void**)&data, &_file_handle) == 0)
        {
            printf("CrapFS::mmap mapped to %08X\n",data);
            return (uint8_t*)data;
        }
        return 0;
    }

    // see if the file exists
    FlashFile* find(const std::string& path)
    {
        for (int i = 0; i < _count; i++) {
            if (_dir[i].sig == FSIG && strcmp(_dir[i].name,path.c_str()) == 0)
                return _dir + i;
        }
        return NULL;
    }

    int copy(const std::string& path, int offset, int len)
    {
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
            return -1;
        #define BUF_SIZE 4096
        uint8_t* buf = new uint8_t[BUF_SIZE];
        esp_err_t err;
        int i = 0;
        while (i < len) {
            int n = len-i;
            if (n > BUF_SIZE)
                n = BUF_SIZE;
            fread(buf,1,n,f);
            printf("CrapFS::copy writing %d of %d\n",i,len);
            err = esp_partition_write(_part, i + offset, buf, n);
            if (err)
                break;
            i += n;
        }
        fclose(f);
        delete buf;
        return err;
    }

    //
    FlashFile* create(const std::string& path, int len)
    {
        uint32_t start = 0x10000;
        for (int i = 0; i < _count; i++) {
            if (_dir[i].sig != FSIG) {
                _dir[i].sig = FSIG;
                _dir[i].offset = start;
                _dir[i].len = len;  //
                strcpy(_dir[i].name,path.c_str());
                printf("CrapFS::created %s %08X %d\n",_dir[i].name,start,len);
                esp_err_t err = esp_partition_erase_range(_part,0, DIR_BLOCK_SIZE);     // erase dir
                if (err == 0)
                    err = esp_partition_write(_part, 0, _buf, DIR_BLOCK_SIZE);    // update dir
                if (err) {
                    printf("CrapFS::create dir write failed %d\n",err);
                    return NULL;
                }
                if (copy(path,start,len))
                    return NULL;
                return _dir+i;
            }
            start = align(_dir[i].offset + _dir[i].len);
        }
        // create failed. might want to invoke nuclear option
        return NULL;
    }
};

uint8_t* map_file(const char* path, int len)
{
    CrapFS _fs;
    FlashFile* file = _fs.find(path);   // already copied?
    if (!file)
        file = _fs.create(path,len);    // need to create a new file
    if (!file) {
        _fs.reformat();
        file = _fs.create(path,len);    // need to create a new file after reformatting the cache
    }
    return _fs.mmap(file);
}

void unmap_file(uint8_t* ptr)
{
    if (_file_handle)
        spi_flash_munmap(_file_handle);
    _file_handle = 0;
}

#else
#include <sys/stat.h>

uint8_t* map_file(const char* path, int len)
{
    uint8_t* d;
    Emu::load(path,&d,&len);
    return d;
}

void unmap_file(uint8_t* ptr)
{
    delete ptr;
}

#endif

// map one bit array to another
uint32_t generic_map(uint32_t bits, const uint32_t* m)
{
    uint32_t b = 0;
    for (int i = 0; i < 16; i++) {
        if ((0x8000 >> i) & bits)
            b |= m[i];
    }
    return b;
}

Emu::Emu(const char* n,int w,int h, int st, int aformat, int cc, int f) :
    name(n),width(w),height(h),standard(st),audio_format(aformat),cc_width(cc),flavor(f)
{
    //audio_frequency = 15625; // requires fixed point sampler
    audio_frequency = standard == 1 ? 15720 : 15600;
    audio_frame_samples = standard ? (audio_frequency << 16)/60 : (audio_frequency << 16)/50;   // fixed point sampler
    audio_fraction = 0;
}

Emu::~Emu()
{
}

int Emu::frame_sample_count()
{
    int n = audio_frame_samples + audio_fraction;
    audio_fraction = n & 0xFFFF;
    return n >> 16;
}

int Emu::insert(const std::string& path, int flags, int disk_index)
{
    return -1;
}

const uint32_t* Emu::composite_palette()
{
    return standard ? ntsc_palette() : pal_palette();
}

// determine file type
int Emu::head(const std::string& path, uint8_t* data, int len)
{
    FILE *f = fopen(path.c_str() , "rb");
    if (!f)
        return -1;
    fread(data,1,len,f);
    fseek(f, 0, SEEK_END);
    int flen =(int)ftell(f);
    fclose(f);
    return flen;
}

int Emu::load(const std::string& path, uint8_t** data, int* len)
{
    *data = 0;
    *len = 0;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        printf("Emu::load failed for %s\n",path.c_str());
        return -1;
    }
    fseek(f, 0, SEEK_END);
    int fsize = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = new uint8_t[fsize];
    if (!d) {
        printf("Emu::load failed for %s (out of memory)\n",path.c_str());
        fclose(f);
        return -1;
    }
    fread(d, 1, fsize, f);
    fclose(f);

    printf("Emu::load %d bytes %s\n",fsize,path.c_str());
    *data = d;
    *len = fsize;
    return 0;
}

// dev/debug helper: dumps a generated NTSC/PAL YUV phase-table for an RGB
// palette to stdout (used to hand-generate the const tables baked into each
// emu_*.cpp). Shared by all three backends -- moved here from
// emu_atari800.cpp so it stays linkable regardless of which backend's
// build_src_filter is active.
void make_yuv_palette(const char* name, const uint32_t* rgb, int len)
{
    uint32_t pal[256*2];
    uint32_t* even = pal;
    uint32_t* odd = pal + len;

    float chroma_scale = BLANKING_LEVEL/2/256;
    //chroma_scale /= 127;  // looks a little washed out
    chroma_scale /= 80;
    for (int i = 0; i < len; i++) {
        uint8_t r = rgb[i] >> 16;
        uint8_t g = rgb[i] >> 8;
        uint8_t b = rgb[i];

        float y = 0.299 * r + 0.587*g + 0.114 * b;
        float u = -0.147407 * r - 0.289391 * g + 0.436798 * b;
        float v =  0.614777 * r - 0.514799 * g - 0.099978 * b;
        y /= 255.0;
        y = (y*(WHITE_LEVEL-BLACK_LEVEL) + BLACK_LEVEL)/256;

        uint32_t e = 0;
        uint32_t o = 0;
        for (int i = 0; i < 4; i++) {
            float p = 2*M_PI*i/4 + M_PI;
            float s = sin(p)*chroma_scale;
            float c = cos(p)*chroma_scale;
            uint8_t e0 = round(y + (s*u) + (c*v));
            uint8_t o0 = round(y + (s*u) - (c*v));
            e = (e << 8) | e0;
            o = (o << 8) | o0;
        }
        *even++ = e;
        *odd++ = o;
    }

    printf("uint32_t %s_4_phase_pal[] = {\n",name);
    for (int i = 0; i < len*2; i++) {  // start with luminance map
        printf("0x%08X,",pal[i]);
        if ((i & 7) == 7)
            printf("\n");
        if (i == (len-1)) {
            printf("//odd\n");
        }
    }
    printf("};\n");
}
