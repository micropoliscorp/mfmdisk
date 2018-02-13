/*
 * Parse SuperCard Pro SCP flux format.
 *
 * Written in 2014 by Simon Owen, based on code by Keir Fraser
 * Heavily rewritten by (C) 2018 Serge Vakulenko
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. The name of the author may not be used to endorse or promote products
 *      derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include "scp.h"
#include "mfm.h"

#ifdef __APPLE__
#   include <libkern/OSByteOrder.h>
#   define be16toh(x) OSSwapBigToHostInt16(x)
#   define le16toh(x) OSSwapLittleToHostInt16(x)
#   define be32toh(x) OSSwapBigToHostInt32(x)
#   define le32toh(x) OSSwapLittleToHostInt32(x)
#endif

static void read_exact(int fd, void *buf, size_t count)
{
    ssize_t done;
    char *_buf = buf;

    while (count > 0) {
        done = read(fd, _buf, count);
        if (done < 0) {
            if ((errno == EAGAIN) || (errno == EINTR))
                continue;
            err(1, NULL);
        }
        if (done == 0) {
            memset(_buf, 0, count);
            done = count;
        }
        count -= done;
        _buf += done;
    }
}

/*
 * Open the SCP file.
 * Read disk header.
 */
int scp_open(scp_file_t *sf, const char *name)
{
    memset(sf, 0, sizeof(*sf));
    sf->fd = open(name, O_RDONLY);
    if (sf->fd < 0)
        err(1, "%s", name);

    read_exact(sf->fd, &sf->header, sizeof(sf->header));

    if (memcmp(sf->header.sig, "SCP", 3) != 0)
        errx(1, "%s: Not SCP file", name);

    if (sf->header.nr_revolutions == 0 || sf->header.nr_revolutions > REV_MAX)
        errx(1, "%s: Invalid revolution count = %u", name, sf->header.nr_revolutions);

    if (sf->header.cell_width != 0 && sf->header.cell_width != 16)
        errx(1, "%s: Unsupported cell width = %u", name, sf->header.cell_width);

    /* Convert to host byte order. */
    int i;
    for (i = 0; i < TRACK_MAX; i++) {
        sf->header.track_offset[i] = le32toh(sf->header.track_offset[i]);
    }

    return 0;
}

/*
 * Close the SCP file.
 * Free any allocated memory.
 */
void scp_close(scp_file_t *sf)
{
    close(sf->fd);
    if (sf->dat) {
        free(sf->dat);
        sf->dat = 0;
    }
}

/*
 * Select a track by index.
 * Read track header.
 */
int scp_select_track(scp_file_t *sf, unsigned int tn)
{
    /* Track already loaded? */
    if (sf->dat && (sf->track.track_nr == tn))
        return 0;

    /* Free data from previous track. */
    free(sf->dat);
    sf->dat = NULL;
    sf->datsz = 0;

    /* Read track header. */
    unsigned tdh_offset = sf->header.track_offset[tn];
    if (lseek(sf->fd, tdh_offset, SEEK_SET) != tdh_offset)
        return -1;

    read_exact(sf->fd, &sf->track, 4 + 12 * sf->header.nr_revolutions);
    if (memcmp(sf->track.sig, "TRK", 3) != 0)
        return -1;

    if (sf->track.track_nr != tn)
        return -1;

    /* Convert to host byte order.
     * Make offset from the start of the file.
     * Compute total data size. */
    unsigned int rev;
    for (rev = 0; rev < sf->header.nr_revolutions; rev++) {
        sf->track.rev[rev].duration_25ns = le32toh(sf->track.rev[rev].duration_25ns);
        sf->track.rev[rev].nr_samples = le32toh(sf->track.rev[rev].nr_samples);
        sf->track.rev[rev].offset = tdh_offset + le32toh(sf->track.rev[rev].offset);
        sf->datsz += sf->track.rev[rev].nr_samples;
    }

    /* Allocate data. */
    sf->dat = calloc(sf->datsz, sizeof(sf->dat[0]));
    if (! sf->dat)
        err(1, NULL);

    /* Read data. */
    sf->datsz = 0;
    for (rev = 0; rev < sf->header.nr_revolutions; rev++) {
        if (lseek(sf->fd, sf->track.rev[rev].offset, SEEK_SET) != sf->track.rev[rev].offset)
            return -1;
        read_exact(sf->fd, &sf->dat[sf->datsz],
                   sf->track.rev[rev].nr_samples * sizeof(sf->dat[0]));
        sf->datsz += sf->track.rev[rev].nr_samples;
        sf->index_ptr[rev] = sf->datsz;
    }
    return 0;
}

/*
 * Reset read pointers.
 */
void scp_reset(scp_file_t *sf)
{
    sf->iter_ptr = 0;
    sf->iter_limit = 0;
}

unsigned scp_next_flux(scp_file_t *sf, unsigned int rev)
{
    unsigned val = 0;

    for (;;) {
        if (sf->iter_ptr >= sf->iter_limit) {
            sf->iter_limit = sf->index_ptr[rev];
            sf->iter_ptr = rev ? sf->index_ptr[rev-1] : 0;
            val = 0;
        }

        unsigned t = be16toh(sf->dat[sf->iter_ptr++]);
        if (t != 0) {
            val += t;
            return val;
        }

        /* overflow */
        val += 0x10000;
    }
}

void scp_print_disk_header(scp_file_t *sf)
{
    printf("Disk Header:\n");
    printf("    Signature: %c%c%c\n", sf->header.sig[0], sf->header.sig[1], sf->header.sig[2]);
    printf("  SCP Version: %d.%d\n", sf->header.version >> 4, sf->header.version & 0xf);

    printf("    Disk Type: ");
    switch (sf->header.disk_type) {
    case 0:  printf("CBM\n");       break;
    case 1:  printf("AMIGA\n");     break;
    case 2:  printf("APPLE II\n");  break;
    case 3:  printf("ATARI ST\n");  break;
    case 4:  printf("ATARI 800\n"); break;
    case 5:  printf("MAC 800\n");   break;
    case 6:  printf("360K/720K\n"); break;
    case 7:  printf("1.44MB\n");    break;
    default: printf("%d\n", sf->header.disk_type); break;
    }

    printf("  Revolutions: %d\n", sf->header.nr_revolutions);
    printf("       Tracks: %d - %d\n", sf->header.start_track, sf->header.end_track);

    printf("        Flags: %x <", sf->header.flags);
    if (sf->header.flags & FLAG_TPI)    printf("96TPI");  else printf("48TPI");
    if (sf->header.flags & FLAG_RPM)    printf(" 360RPM"); else printf(" 300RPM");
    if (sf->header.flags & FLAG_INDEX)  printf(" Index");
    if (sf->header.flags & FLAG_TYPE)   printf(" Normalized");
    if (sf->header.flags & FLAG_MODE)   printf(" Writeable");
    if (sf->header.flags & FLAG_FOOTER) printf(" Footer");
    printf(">\n");

    printf("   Cell Width: %d\n", sf->header.cell_width ? sf->header.cell_width : 16);

    printf("        Sides: ");
    switch (sf->header.sides) {
    case SIDE_BOTH:   printf("Both\n");        break;
    case SIDE_BOTTOM: printf("Bottom only\n"); break;
    case SIDE_TOP:    printf("Top only\n");    break;
    default: printf("%d\n", sf->header.sides); break;
    }

    printf("     Checksum: %08x\n", sf->header.checksum);

    printf("Track Offsets:");
    int i;
    for (i = 0; i < TRACK_MAX; i++) {
        printf(" %d", sf->header.track_offset[i]);
        if (i % 10 == 9)
            printf("\n              ");
    }
    printf("\n");
}

void scp_print_track(scp_file_t *sf)
{
    int rev;

    printf("Track %d:\n", sf->track.track_nr);
    for (rev = 0; rev < sf->header.nr_revolutions; rev++) {
        scp_reset(sf);
        unsigned f1 = scp_next_flux(sf, rev);
        unsigned f2 = scp_next_flux(sf, rev);
        unsigned f3 = scp_next_flux(sf, rev);
        unsigned f4 = scp_next_flux(sf, rev);

        printf("  Revolution %d: %u samples, %f msec, offset %u, data %u-%u-%u-%u...\n",
            rev, sf->track.rev[rev].nr_samples,
            sf->track.rev[rev].duration_25ns * 0.000025,
            sf->track.rev[rev].offset, f1, f2, f3, f4);
    }
}

/*
 * Flux-based streams
 */
#define CLOCK_CENTRE    2000   /* 2000ns = 2us */
#define CLOCK_MAX_ADJ   10     /* +/- 10% adjustment */
#define CLOCK_MIN(_c)   (((_c) * (100 - CLOCK_MAX_ADJ)) / 100)
#define CLOCK_MAX(_c)   (((_c) * (100 + CLOCK_MAX_ADJ)) / 100)

/*
 * Amount to adjust phase/period of our clock based on each observed flux.
 */
#define PERIOD_ADJ_PCT  5
#define PHASE_ADJ_PCT   60

typedef struct {
    scp_file_t *sf;
    int rev;
    int clock;          /* nsec */
    int flux;           /* nsec */
    int time;           /* nsec */
    int clocked_zeros;
} pll_t;

/*
 * Initialize PLL.
 */
static void pll_init(pll_t *pll, scp_file_t *sf, int rev)
{
    memset(pll, 0, sizeof(*pll));
    pll->sf = sf;
    pll->rev = rev;
    pll->clock = CLOCK_CENTRE;
}

/*
 * Decode and return next bit from the flux input stream.
 * Implement PLL in software.
 * The routine was ported from keirf/Disk-Utilities project.
 */
static int pll_next_bit(pll_t *pll)
{
    while (pll->flux < pll->clock/2) {
        pll->flux += 25 * scp_next_flux(pll->sf, pll->rev);
    }

    pll->time += pll->clock;
    pll->flux -= pll->clock;

    if (pll->flux >= pll->clock/2) {
        pll->clocked_zeros++;
        return 0;
    }

    /* PLL: Adjust clock frequency according to phase mismatch.
     * eg. PERIOD_ADJ_PCT=0% -> timing-window centre freq. never changes */
    if (pll->clocked_zeros <= 3) {

        /* In sync: adjust base clock by a fraction of phase mismatch. */
        pll->clock += pll->flux * PERIOD_ADJ_PCT / 100;
    } else {
        /* Out of sync: adjust base clock towards centre. */
        pll->clock += (CLOCK_CENTRE - pll->clock) * PERIOD_ADJ_PCT / 100;
    }

    /* Clamp the clock's adjustment range. */
    if (pll->clock < CLOCK_MIN(CLOCK_CENTRE))
        pll->clock = CLOCK_MIN(CLOCK_CENTRE);
    if (pll->clock > CLOCK_MAX(CLOCK_CENTRE))
        pll->clock = CLOCK_MAX(CLOCK_CENTRE);

    /* PLL: Adjust clock phase according to mismatch.
     * eg. PHASE_ADJ_PCT=100% -> timing window snaps to observed flux. */
    int new_flux = pll->flux * (100 - PHASE_ADJ_PCT) / 100;
    pll->time += pll->flux - new_flux;
    pll->flux = new_flux;

    pll->clocked_zeros = 0;
    return 1;
}

/*
 * Decode MFM data from SCP file, for given revolution.
 */
void scp_write_mfm(const char *name, FILE *fout, int rev)
{
    /* Open the image file. */
    scp_file_t sf;
    scp_open(&sf, name);

    mfm_writer_t writer;

    if (rev >= sf.header.nr_revolutions)
        errx(1, "Revolution %d out of range 0...%d\n", rev, sf.header.nr_revolutions-1);

    int tn;
    for (tn = 0; tn < 160; tn++) {
        int n;

        /* Start new track. */
        mfm_write_reset(&writer, fout);

        if (tn < sf.header.start_track ||
            tn >= sf.header.end_track ||
            scp_select_track(&sf, tn) < 0)
        {
            /* Produce empty track. */
            for (n=0; n<6400; n++)
                mfm_write_byte(&writer, 0);
        } else {
            /* Decode flux data of this revolution. */
            pll_t pll;

            scp_reset(&sf);
            pll_init(&pll, &sf, rev);
            pll_next_bit(&pll); /* Ignore first half-bit. */
            n = 0;
            do {
                int halfbit = pll_next_bit(&pll);
                mfm_write_halfbit(&writer, halfbit);
                n++;
            } while (sf.iter_ptr < sf.iter_limit);

            /* Fill the rest of track. */
            while (n++ < 12800*8) {
                mfm_write_halfbit(&writer, !writer.last);
                if (n++ < 12800*8)
                    mfm_write_halfbit(&writer, !writer.last);
            }
        }
    }
    scp_close(&sf);
}
