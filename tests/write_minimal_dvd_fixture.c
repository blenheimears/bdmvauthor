#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_be16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)((v >> 8) & 0xffU);
    p[1] = (unsigned char)(v & 0xffU);
}

static void put_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xffUL);
    p[1] = (unsigned char)((v >> 16) & 0xffUL);
    p[2] = (unsigned char)((v >> 8) & 0xffUL);
    p[3] = (unsigned char)(v & 0xffUL);
}

int main(int argc, char **argv)
{
    unsigned char image[4096];
    FILE *fp;

    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT.IFO\n", argc > 0 ? argv[0] : "fixture-writer");
        return 2;
    }

    memset(image, 0, sizeof(image));
    memcpy(image, "DVDVIDEO-VMG", 12);
    put_be32(image + 12, 3UL);     /* VMG last sector: four 2048-byte sectors. */
    put_be32(image + 28, 1UL);     /* VMGI last sector: two sectors. */
    put_be16(image + 62, 0U);      /* No title sets in this structural fixture. */
    put_be32(image + 192, 0UL);    /* No VMGM VOB. */
    put_be32(image + 196, 1UL);    /* TT_SRPT begins at sector 1. */
    put_be16(image + 2048, 0U);    /* Empty TT_SRPT title count. */

    fp = fopen(argv[1], "wb");
    if (fp == NULL) {
        perror(argv[1]);
        return 1;
    }
    if (fwrite(image, 1, sizeof(image), fp) != sizeof(image)) {
        perror("write fixture");
        fclose(fp);
        return 1;
    }
    if (fclose(fp) != 0) {
        perror("close fixture");
        return 1;
    }
    return 0;
}
