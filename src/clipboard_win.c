// Native Win32 clipboard image reader.
// Returns a malloc'd PNG byte buffer and sets *out_len, or NULL if the
// clipboard has no image. Caller owns the returned buffer and must free().

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

// Implemented by raylib's rtextures.c (which defines STB_IMAGE_WRITE_IMPLEMENTATION).
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels,
                                            int stride_bytes,
                                            int x, int y, int n,
                                            int *out_len);

static unsigned char *dib_to_png(const void *dib, int *out_len) {
    const BITMAPINFOHEADER *bih = (const BITMAPINFOHEADER *)dib;
    if (bih->biSize < sizeof(BITMAPINFOHEADER)) return NULL;
    if (bih->biCompression != BI_RGB && bih->biCompression != BI_BITFIELDS)
        return NULL;

    int w = bih->biWidth;
    int h_raw = bih->biHeight;
    int h = h_raw < 0 ? -h_raw : h_raw;
    int top_down = h_raw < 0;
    int bpp = bih->biBitCount;
    if (bpp != 24 && bpp != 32) return NULL;
    if (w <= 0 || h <= 0) return NULL;

    int palette_bytes = 0;
    if (bih->biCompression == BI_BITFIELDS) palette_bytes = 12;
    const unsigned char *pix = (const unsigned char *)dib
                             + bih->biSize + palette_bytes;

    int src_stride = ((w * bpp + 31) / 32) * 4;
    int channels = 4;
    unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * channels);
    if (!rgba) return NULL;

    for (int y = 0; y < h; y++) {
        int src_y = top_down ? y : (h - 1 - y);
        const unsigned char *row = pix + (size_t)src_y * src_stride;
        unsigned char *dst = rgba + (size_t)y * w * channels;
        if (bpp == 32) {
            for (int x = 0; x < w; x++) {
                dst[0] = row[2];
                dst[1] = row[1];
                dst[2] = row[0];
                dst[3] = row[3];
                dst += 4; row += 4;
            }
        } else { // 24
            for (int x = 0; x < w; x++) {
                dst[0] = row[2];
                dst[1] = row[1];
                dst[2] = row[0];
                dst[3] = 255;
                dst += 4; row += 3;
            }
        }
    }

    unsigned char *png = stbi_write_png_to_mem(rgba, w * channels,
                                               w, h, channels, out_len);
    free(rgba);
    return png;
}

unsigned char *clipboard_image_png(int *out_len) {
    *out_len = 0;
    if (!OpenClipboard(NULL)) return NULL;

    unsigned char *result = NULL;

    UINT png_fmt = RegisterClipboardFormatA("PNG");
    if (png_fmt && IsClipboardFormatAvailable(png_fmt)) {
        HANDLE h = GetClipboardData(png_fmt);
        if (h) {
            SIZE_T sz = GlobalSize(h);
            void *p = GlobalLock(h);
            if (p && sz > 0) {
                result = (unsigned char *)malloc(sz);
                if (result) {
                    memcpy(result, p, sz);
                    *out_len = (int)sz;
                }
            }
            if (p) GlobalUnlock(h);
        }
    }

    if (!result && IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE h = GetClipboardData(CF_DIB);
        if (h) {
            void *p = GlobalLock(h);
            if (p) result = dib_to_png(p, out_len);
            if (p) GlobalUnlock(h);
        }
    }

    CloseClipboard();
    return result;
}
