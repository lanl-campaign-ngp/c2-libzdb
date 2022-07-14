#include "vdev_raidz.h"

#include <sys/vdev_impl.h>
#include <sys/vdev_raidz_impl.h>

#include <stdlib.h>

void
vdev_raidz_map_alloc (zio_t *zio, uint64_t ashift, uint64_t dcols,
                      uint64_t nparity, char **backing)
{
  raidz_map_t *rm;
  /* The starting RAIDZ (parent) vdev sector of the block. */
  uint64_t b = zio->io_offset >> ashift;
  /* The zio's size in units of the vdev's minimum sector size. */
  uint64_t s = zio->io_size >> ashift;
  /* The first column for this stripe. */
  uint64_t f = b % dcols;
  /* The starting byte offset on each child vdev. */
  uint64_t o = (b / dcols) << ashift;
  uint64_t q, r, c, bc, col, acols, scols, coff, devidx, asize, tot;
  uint64_t off = 0;

  /*
   * "Quotient": The number of data sectors for this stripe on all but
   * the "big column" child vdevs that also contain "remainder" data.
   */
  q = s / (dcols - nparity);

  /*
   * "Remainder": The number of partial stripe data sectors in this I/O.
   * This will add a sector to some, but not all, child vdevs.
   */
  r = s - q * (dcols - nparity);

  /* The number of "big columns" - those which contain remainder data. */
  bc = (r == 0 ? 0 : r + nparity);

  /*
   * The total number of data and parity sectors associated with
   * this I/O.
   */
  tot = s + nparity * (q + (r == 0 ? 0 : 1));

  /* acols: The columns that will be accessed. */
  /* scols: The columns that will be accessed or skipped. */
  if (q == 0)
    {
      /* Our I/O request doesn't span all child vdevs. */
      acols = bc;
      scols = MIN (dcols, roundup (bc, nparity + 1));
    }
  else
    {
      acols = dcols;
      scols = dcols;
    }

  ASSERT3U (acols, <=, scols);

  rm = malloc (offsetof (raidz_map_t, rm_col[scols]));

  rm->rm_cols = acols;
  rm->rm_scols = scols;
  rm->rm_bigcols = bc;
  rm->rm_skipstart = bc;
  rm->rm_firstdatacol = nparity;

  asize = 0;

  for (c = 0; c < scols; c++)
    {
      col = f + c;
      coff = o;
      if (col >= dcols)
        {
          col -= dcols;
          coff += 1ULL << ashift;
        }
      rm->rm_col[c].rc_devidx = col;
      rm->rm_col[c].rc_offset = coff;
      rm->rm_col[c].rc_abd = NULL;
      rm->rm_col[c].rc_gdata = NULL;
      rm->rm_col[c].rc_error = 0;
      rm->rm_col[c].rc_tried = 0;
      rm->rm_col[c].rc_skipped = 0;

      if (c >= acols)
        rm->rm_col[c].rc_size = 0;
      else if (c < bc)
        rm->rm_col[c].rc_size = (q + 1) << ashift;
      else
        rm->rm_col[c].rc_size = q << ashift;

      asize += rm->rm_col[c].rc_size;
    }

  ASSERT3U (asize, ==, tot << ashift);
  rm->rm_asize = roundup (asize, (nparity + 1) << ashift);
  rm->rm_nskip = roundup (tot, nparity + 1) - tot;
  ASSERT3U (rm->rm_asize - asize, ==, rm->rm_nskip << ashift);
  ASSERT3U (rm->rm_nskip, <=, nparity);

  /*
   * If all data stored spans all columns, there's a danger that parity
   * will always be on the same device and, since parity isn't read
   * during normal operation, that device's I/O bandwidth won't be
   * used effectively. We therefore switch the parity every 1MB.
   *
   * ... at least that was, ostensibly, the theory. As a practical
   * matter unless we juggle the parity between all devices evenly, we
   * won't see any benefit. Further, occasional writes that aren't a
   * multiple of the LCM of the number of children and the minimum
   * stripe width are sufficient to avoid pessimal behavior.
   * Unfortunately, this decision created an implicit on-disk format
   * requirement that we need to support for all eternity, but only
   * for single-parity RAID-Z.
   *
   * If we intend to skip a sector in the zeroth column for padding
   * we must make sure to note this swap. We will never intend to
   * skip the first column since at least one data and one parity
   * column must appear in each row.
   */
  ASSERT (rm->rm_cols >= 2);
  ASSERT (rm->rm_col[0].rc_size == rm->rm_col[1].rc_size);

  if (rm->rm_firstdatacol == 1 && (zio->io_offset & (1ULL << 20)))
    {
      devidx = rm->rm_col[0].rc_devidx;
      o = rm->rm_col[0].rc_offset;
      rm->rm_col[0].rc_devidx = rm->rm_col[1].rc_devidx;
      rm->rm_col[0].rc_offset = rm->rm_col[1].rc_offset;
      rm->rm_col[1].rc_devidx = devidx;
      rm->rm_col[1].rc_offset = o;

      if (rm->rm_skipstart == 0)
        rm->rm_skipstart = 1;
    }

  for (c = rm->rm_firstdatacol; c < rm->rm_cols; c++)
    {
      raidz_col_t *rc = &rm->rm_col[c];
      rc->rc_offset += VDEV_LABEL_START_SIZE;
      printf ("col=%02ld devidx=%02ld dev=%s offset=%lu size=%lu\n", c,
              rc->rc_devidx, (char *)backing[rc->rc_devidx], rc->rc_offset,
              rc->rc_size);
    }

  free (rm);
}
