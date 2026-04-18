// Native macOS clipboard image reader.
// Returns a malloc'd PNG byte buffer and sets *out_len, or NULL if the
// clipboard has no image. Caller owns the returned buffer and must free().

#import <Cocoa/Cocoa.h>
#include <stdlib.h>
#include <string.h>

unsigned char *clipboard_image_png(int *out_len) {
    *out_len = 0;
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSArray *classes = @[[NSImage class]];
        NSDictionary *options = @{};
        if (![pb canReadObjectForClasses:classes options:options]) {
            return NULL;
        }
        NSArray *objects = [pb readObjectsForClasses:classes options:options];
        if ([objects count] == 0) return NULL;
        NSImage *img = objects[0];

        // Encode to PNG via NSBitmapImageRep
        NSData *tiff = [img TIFFRepresentation];
        if (!tiff) return NULL;
        NSBitmapImageRep *bitmap = [NSBitmapImageRep imageRepWithData:tiff];
        if (!bitmap) return NULL;
        NSData *png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                           properties:@{}];
        if (!png) return NULL;

        NSUInteger len = [png length];
        unsigned char *buf = malloc(len);
        if (!buf) return NULL;
        memcpy(buf, [png bytes], len);
        *out_len = (int)len;
        return buf;
    }
}
