/*
* Simd Library (http://ermig1979.github.io/Simd).
*
* Copyright (c) 2011-2021 Yermalayeu Ihar.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#include "Simd/SimdMemory.h"
#include "Simd/SimdImageSave.h"
#include "Simd/SimdBase.h"

namespace Simd
{        
    namespace Base
    {
#ifndef PNG_MALLOC
#define PNG_MALLOC(sz)        malloc(sz)
#define PNG_REALLOC(p,newsz)  realloc(p,newsz)
#define PNG_FREE(p)           free(p)
#endif

#ifndef PNG_REALLOC_SIZED
#define PNG_REALLOC_SIZED(p,oldsz,newsz) PNG_REALLOC(p,newsz)
#endif

#ifndef PNG_MEMMOVE
#define PNG_MEMMOVE(a,b,sz) memmove(a,b,sz)
#endif

#define PNG_UCHAR(x) (unsigned char) ((x) & 0xff)

#define png__sbraw(a) ((int *) (void *) (a) - 2)
#define png__sbm(a)   png__sbraw(a)[0]
#define png__sbn(a)   png__sbraw(a)[1]

#define png__sbneedgrow(a,n)  ((a)==0 || png__sbn(a)+n >= png__sbm(a))
#define png__sbmaybegrow(a,n) (png__sbneedgrow(a,(n)) ? png__sbgrow(a,n) : 0)
#define png__sbgrow(a,n)  png__sbgrowf((void **) &(a), (n), sizeof(*(a)))

#define png__sbpush(a, v)      (png__sbmaybegrow(a,1), (a)[png__sbn(a)++] = (v))
#define png__sbcount(a)        ((a) ? png__sbn(a) : 0)
#define png__sbfree(a)         ((a) ? PNG_FREE(png__sbraw(a)),0 : 0)

        static void* png__sbgrowf(void** arr, int increment, int itemsize)
        {
            int m = *arr ? 2 * png__sbm(*arr) + increment : increment + 1;
            void* p = PNG_REALLOC_SIZED(*arr ? png__sbraw(*arr) : 0, *arr ? (png__sbm(*arr) * itemsize + sizeof(int) * 2) : 0, itemsize * m + sizeof(int) * 2);
            assert(p);
            if (p) {
                if (!*arr) ((int*)p)[1] = 0;
                *arr = (void*)((int*)p + 2);
                png__sbm(*arr) = m;
            }
            return *arr;
        }

        static unsigned char* png__zlib_flushf(unsigned char* data, unsigned int* bitbuffer, int* bitcount)
        {
            while (*bitcount >= 8) {
                png__sbpush(data, PNG_UCHAR(*bitbuffer));
                *bitbuffer >>= 8;
                *bitcount -= 8;
            }
            return data;
        }

        static int png__zlib_bitrev(int code, int codebits)
        {
            int res = 0;
            while (codebits--) {
                res = (res << 1) | (code & 1);
                code >>= 1;
            }
            return res;
        }

        static unsigned int png__zlib_countm(unsigned char* a, unsigned char* b, int limit)
        {
            int i;
            for (i = 0; i < limit && i < 258; ++i)
                if (a[i] != b[i]) break;
            return i;
        }

        static unsigned int png__zhash(unsigned char* data)
        {
            uint32_t hash = data[0] + (data[1] << 8) + (data[2] << 16);
            hash ^= hash << 3;
            hash += hash >> 5;
            hash ^= hash << 4;
            hash += hash >> 17;
            hash ^= hash << 25;
            hash += hash >> 6;
            return hash;
        }

#define png__zlib_flush() (out = png__zlib_flushf(out, &bitbuf, &bitcount))
#define png__zlib_add(code,codebits) \
      (bitbuf |= (code) << bitcount, bitcount += (codebits), png__zlib_flush())
#define png__zlib_huffa(b,c)  png__zlib_add(png__zlib_bitrev(b,c),c)
        // default huffman tables
#define png__zlib_huff1(n)  png__zlib_huffa(0x30 + (n), 8)
#define png__zlib_huff2(n)  png__zlib_huffa(0x190 + (n)-144, 9)
#define png__zlib_huff3(n)  png__zlib_huffa(0 + (n)-256,7)
#define png__zlib_huff4(n)  png__zlib_huffa(0xc0 + (n)-280,8)
#define png__zlib_huff(n)  ((n) <= 143 ? png__zlib_huff1(n) : (n) <= 255 ? png__zlib_huff2(n) : (n) <= 279 ? png__zlib_huff3(n) : png__zlib_huff4(n))
#define png__zlib_huffb(n) ((n) <= 143 ? png__zlib_huff1(n) : png__zlib_huff2(n))

#define png__ZHASH   16384

        static unsigned char* png_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality)
        {
            static unsigned short lengthc[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258, 259 };
            static unsigned char  lengtheb[] = { 0,0,0,0,0,0,0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,  4,  5,  5,  5,  5,  0 };
            static unsigned short distc[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577, 32768 };
            static unsigned char  disteb[] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
            unsigned int bitbuf = 0;
            int i, j, bitcount = 0;
            unsigned char* out = NULL;
            unsigned char*** hash_table = (unsigned char***)PNG_MALLOC(png__ZHASH * sizeof(unsigned char**));
            if (hash_table == NULL)
                return NULL;
            if (quality < 5) quality = 5;

            png__sbpush(out, 0x78);   // DEFLATE 32K window
            png__sbpush(out, 0x5e);   // FLEVEL = 1
            png__zlib_add(1, 1);  // BFINAL = 1
            png__zlib_add(1, 2);  // BTYPE = 1 -- fixed huffman

            for (i = 0; i < png__ZHASH; ++i)
                hash_table[i] = NULL;

            i = 0;
            while (i < data_len - 3) {
                // hash next 3 bytes of data to be compressed
                int h = png__zhash(data + i) & (png__ZHASH - 1), best = 3;
                unsigned char* bestloc = 0;
                unsigned char** hlist = hash_table[h];
                int n = png__sbcount(hlist);
                for (j = 0; j < n; ++j) {
                    if (hlist[j] - data > i - 32768) { // if entry lies within window
                        int d = png__zlib_countm(hlist[j], data + i, data_len - i);
                        if (d >= best) { best = d; bestloc = hlist[j]; }
                    }
                }
                // when hash table entry is too long, delete half the entries
                if (hash_table[h] && png__sbn(hash_table[h]) == 2 * quality) {
                    PNG_MEMMOVE(hash_table[h], hash_table[h] + quality, sizeof(hash_table[h][0]) * quality);
                    png__sbn(hash_table[h]) = quality;
                }
                png__sbpush(hash_table[h], data + i);

                if (bestloc) {
                    // "lazy matching" - check match at *next* byte, and if it's better, do cur byte as literal
                    h = png__zhash(data + i + 1) & (png__ZHASH - 1);
                    hlist = hash_table[h];
                    n = png__sbcount(hlist);
                    for (j = 0; j < n; ++j) {
                        if (hlist[j] - data > i - 32767) {
                            int e = png__zlib_countm(hlist[j], data + i + 1, data_len - i - 1);
                            if (e > best) { // if next match is better, bail on current match
                                bestloc = NULL;
                                break;
                            }
                        }
                    }
                }

                if (bestloc) {
                    int d = (int)(data + i - bestloc); // distance back
                    assert(d <= 32767 && best <= 258);
                    for (j = 0; best > lengthc[j + 1] - 1; ++j);
                    png__zlib_huff(j + 257);
                    if (lengtheb[j]) png__zlib_add(best - lengthc[j], lengtheb[j]);
                    for (j = 0; d > distc[j + 1] - 1; ++j);
                    png__zlib_add(png__zlib_bitrev(j, 5), 5);
                    if (disteb[j]) png__zlib_add(d - distc[j], disteb[j]);
                    i += best;
                }
                else {
                    png__zlib_huffb(data[i]);
                    ++i;
                }
            }
            // write out final bytes
            for (; i < data_len; ++i)
                png__zlib_huffb(data[i]);
            png__zlib_huff(256); // end of block
            // pad with 0 bits to byte boundary
            while (bitcount)
                png__zlib_add(0, 1);

            for (i = 0; i < png__ZHASH; ++i)
                (void)png__sbfree(hash_table[i]);
            PNG_FREE(hash_table);

            {
                // compute adler32 on input
                unsigned int s1 = 1, s2 = 0;
                int blocklen = (int)(data_len % 5552);
                j = 0;
                while (j < data_len) {
                    for (i = 0; i < blocklen; ++i) { s1 += data[j + i]; s2 += s1; }
                    s1 %= 65521; s2 %= 65521;
                    j += blocklen;
                    blocklen = 5552;
                }
                png__sbpush(out, PNG_UCHAR(s2 >> 8));
                png__sbpush(out, PNG_UCHAR(s2));
                png__sbpush(out, PNG_UCHAR(s1 >> 8));
                png__sbpush(out, PNG_UCHAR(s1));
            }
            *out_len = png__sbn(out);
            // make returned pointer freeable
            PNG_MEMMOVE(png__sbraw(out), out, *out_len);
            return (unsigned char*)png__sbraw(out);
        }

        SIMD_INLINE void WriteCrc32(OutputMemoryStream & stream, size_t size)
        {
            stream.WriteBE(Base::Crc32(stream.Current() - size - 4, size + 4));
        }

        static unsigned char png__paeth(int a, int b, int c)
        {
            int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
            if (pa <= pb && pa <= pc) return PNG_UCHAR(a);
            if (pb <= pc) return PNG_UCHAR(b);
            return PNG_UCHAR(c);
        }

        int png_write_png_compression_level = 8;
        int png_write_force_png_filter = -1;
        int png__flip_vertically_on_write = 0;

        // @OPTIMIZE: provide an option that always forces left-predict or paeth predict
        static void png__encode_png_line(const unsigned char* pixels, int stride_bytes, int width, int height, int y, int n, int filter_type, signed char* line_buffer)
        {
            static int mapping[] = { 0,1,2,3,4 };
            static int firstmap[] = { 0,1,0,5,6 };
            int* mymap = (y != 0) ? mapping : firstmap;
            int i;
            int type = mymap[filter_type];
            const unsigned char* z = pixels + stride_bytes * (png__flip_vertically_on_write ? height - 1 - y : y);
            int signed_stride = png__flip_vertically_on_write ? -stride_bytes : stride_bytes;

            if (type == 0) {
                memcpy(line_buffer, z, width * n);
                return;
            }

            // first loop isn't optimized since it's just one pixel
            for (i = 0; i < n; ++i) {
                switch (type) {
                case 1: line_buffer[i] = z[i]; break;
                case 2: line_buffer[i] = z[i] - z[i - signed_stride]; break;
                case 3: line_buffer[i] = z[i] - (z[i - signed_stride] >> 1); break;
                case 4: line_buffer[i] = (signed char)(z[i] - png__paeth(0, z[i - signed_stride], 0)); break;
                case 5: line_buffer[i] = z[i]; break;
                case 6: line_buffer[i] = z[i]; break;
                }
            }
            switch (type) {
            case 1: for (i = n; i < width * n; ++i) line_buffer[i] = z[i] - z[i - n]; break;
            case 2: for (i = n; i < width * n; ++i) line_buffer[i] = z[i] - z[i - signed_stride]; break;
            case 3: for (i = n; i < width * n; ++i) line_buffer[i] = z[i] - ((z[i - n] + z[i - signed_stride]) >> 1); break;
            case 4: for (i = n; i < width * n; ++i) line_buffer[i] = z[i] - png__paeth(z[i - n], z[i - signed_stride], z[i - signed_stride - n]); break;
            case 5: for (i = n; i < width * n; ++i) line_buffer[i] = z[i] - (z[i - n] >> 1); break;
            case 6: for (i = n; i < width * n; ++i) line_buffer[i] = z[i] - png__paeth(z[i - n], 0, 0); break;
            }
        }

        ImagePngSaver::ImagePngSaver(const ImageSaverParam& param)
            : ImageSaver(param)
            , _channels(0)
            , _size(0)
            , _convert(NULL)
        {
            switch (_param.format)
            {
            case SimdPixelFormatGray8: 
                _channels = 1; 
                break;
            case SimdPixelFormatBgr24: 
                _channels = 3; 
                break;
            case SimdPixelFormatBgra32: 
                _channels = 4; 
                break;
            case SimdPixelFormatRgb24:
                _channels = 3; 
                break;
            }
            _size = _param.width * _channels;
            if (_param.format == SimdPixelFormatRgb24)
            {
                _convert = Base::BgrToRgb;
                _bgr.Resize(_param.height * _size);
            }
            _filt.Resize((_size + 1) * _param.height);
            _line.Resize(_size);
        }

        bool ImagePngSaver::ToStream(const uint8_t* src, size_t stride)
        {
            if (_param.format == SimdPixelFormatRgb24)
            {
                _convert(src, _param.width, _param.height, stride, _bgr.data, _size);
                src = _bgr.data;
                stride = _size;
            }
            int force_filter = png_write_force_png_filter;
            int ctype[5] = { -1, 0, 4, 2, 6 };
            unsigned char sig[8] = { 137,80,78,71,13,10,26,10 };
            int zlen;

            if (force_filter >= 5)
                force_filter = -1;

            for (size_t j = 0; j < _param.height; ++j)
            {
                int filter_type;
                if (force_filter > -1)
                {
                    filter_type = force_filter;
                    png__encode_png_line(src, stride, _param.width, _param.height, j, _channels, force_filter, _line.data);
                }
                else
                { // Estimate the best filter by running through all of them:
                    int best_filter = 0, best_filter_val = 0x7fffffff, est, i;
                    for (filter_type = 0; filter_type < 5; filter_type++)
                    {
                        png__encode_png_line(src, stride, _param.width, _param.height, j, _channels, filter_type, _line.data);

                        // Estimate the entropy of the line using this filter; the less, the better.
                        est = 0;
                        for (i = 0; i < _size; ++i)
                            est += abs(_line[i]);
                        if (est < best_filter_val)
                        {
                            best_filter_val = est;
                            best_filter = filter_type;
                        }
                    }
                    if (filter_type != best_filter)
                    {  // If the last iteration already got us the best filter, don't redo it
                        png__encode_png_line(src, stride, _param.width, _param.height, j, _channels, best_filter, _line.data);
                        filter_type = best_filter;
                    }
                }
                // when we get here, filter_type contains the filter type, and line_buffer contains the data
                _filt[j * (_size + 1)] = (unsigned char)filter_type;
                PNG_MEMMOVE(_filt.data + j * (_size + 1) + 1, _line.data, _size);
            }
            uint8_t* zlib = png_zlib_compress(_filt.data, _filt.size, &zlen, png_write_png_compression_level);
            if (!zlib)
                return false;

            _stream.Reserve(8 + 12 + 13 + 12 + zlen + 12);
            _stream.Write(sig, 8);
            _stream.WriteBE(13);
            _stream.Write("IHDR", 4);
            _stream.WriteBE(_param.width);
            _stream.WriteBE(_param.height);
            _stream.Write<uint8_t>(8);
            _stream.Write<uint8_t>(PNG_UCHAR(ctype[_channels]));
            _stream.Write<uint8_t>(0);
            _stream.Write<uint8_t>(0);
            _stream.Write<uint8_t>(0);
            WriteCrc32(_stream, 13);
            _stream.WriteBE(zlen);
            _stream.Write("IDAT", 4);
            _stream.Write(zlib, zlen);
            WriteCrc32(_stream, zlen);
            _stream.WriteBE(0);
            _stream.Write("IEND", 4);
            WriteCrc32(_stream, 0);

            PNG_FREE(zlib);

            return true;
        }
    }
}