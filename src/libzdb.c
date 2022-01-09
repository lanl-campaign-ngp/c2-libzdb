#include <sys/dbuf.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/dsl_dir.h>
#include <sys/sa_impl.h>
#include <sys/spa_impl.h>
#include <sys/stat.h>
#include <sys/zap.h>
#include <sys/zfs_acl.h>
#include <sys/zfs_context.h>
#include <sys/zfs_sa.h>
#include <sys/zfs_znode.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>

#include <libnvpair.h>
#include <libzutil.h>

#define ZDB_COMPRESS_NAME(idx)                                                \
  ((idx) < ZIO_COMPRESS_FUNCTIONS ? zio_compress_table[(idx)].ci_name         \
                                  : "UNKNOWN")
#define ZDB_CHECKSUM_NAME(idx)                                                \
  ((idx) < ZIO_CHECKSUM_FUNCTIONS ? zio_checksum_table[(idx)].ci_name         \
                                  : "UNKNOWN")
#define ZDB_OT_TYPE(idx)                                                      \
  ((idx) < DMU_OT_NUMTYPES ? (idx)                                            \
   : (idx) == DMU_OTN_ZAP_DATA || (idx) == DMU_OTN_ZAP_METADATA               \
       ? DMU_OT_ZAP_OTHER                                                     \
   : (idx) == DMU_OTN_UINT64_DATA || (idx) == DMU_OTN_UINT64_METADATA         \
       ? DMU_OT_UINT64_OTHER                                                  \
       : DMU_OT_NUMTYPES)

static char *
zdb_ot_name (dmu_object_type_t type)
{
  if (type < DMU_OT_NUMTYPES)
    return dmu_ot[type].ot_name;
  else if ((type & DMU_OT_NEWTYPE)
           && ((type & DMU_OT_BYTESWAP_MASK) < DMU_BSWAP_NUMFUNCS))
    return dmu_ot_byteswap[type & DMU_OT_BYTESWAP_MASK].ob_name;
  else
    return "UNKNOWN";
}

static avl_tree_t idx_tree;
static avl_tree_t domain_tree;
static boolean_t fuid_table_loaded;
static objset_t *sa_os = NULL;
static sa_attr_type_t *sa_attr_table = NULL;
static char curpath[PATH_MAX];

static const char cmdname[] = "zdb";
static uint8_t dump_opt[256];

static int leaked_objects = 0;

static int
open_objset (const char *path, dmu_objset_type_t type, void *tag,
             objset_t **osp)
{
  int err;
  uint64_t sa_attrs = 0;
  uint64_t version = 0;

  VERIFY3P (sa_os, ==, NULL);
  err = dmu_objset_own (path, type, B_TRUE, B_FALSE, tag, osp);
  if (err != 0)
    {
      fprintf (stderr, "failed to own dataset '%s': %s\n", path,
               strerror (err));
      return err;
    }

  if (dmu_objset_type (*osp) == DMU_OST_ZFS && !(*osp)->os_encrypted)
    {
      zap_lookup (*osp, MASTER_NODE_OBJ, ZPL_VERSION_STR, 8, 1, &version);
      if (version >= ZPL_VERSION_SA)
        {
          zap_lookup (*osp, MASTER_NODE_OBJ, ZFS_SA_ATTRS, 8, 1, &sa_attrs);
        }
      err = sa_setup (*osp, sa_attrs, zfs_attr_table, ZPL_END, &sa_attr_table);
      if (err != 0)
        {
          fprintf (stderr, "sa_setup failed: %s\n", strerror (err));
          dmu_objset_disown (*osp, B_FALSE, tag);
          *osp = NULL;
        }
    }

  sa_os = *osp;

  return 0;
}

static void
close_objset (objset_t *os, void *tag)
{
  VERIFY3P (os, ==, sa_os);
  if (os->os_sa != NULL)
    sa_tear_down (os);
  dmu_objset_disown (os, B_FALSE, tag);
  sa_attr_table = NULL;
  sa_os = NULL;
}

static void
dump_packed_nvlist (objset_t *os, uint64_t object, void *data, size_t size)
{
  nvlist_t *nv;
  size_t nvsize = *(uint64_t *)data;
  char *packed = umem_alloc (nvsize, UMEM_NOFAIL);

  VERIFY (0 == dmu_read (os, object, 0, nvsize, packed, DMU_READ_PREFETCH));
  VERIFY (nvlist_unpack (packed, nvsize, &nv, 0) == 0);

  umem_free (packed, nvsize);
  dump_nvlist (nv, 8);
  nvlist_free (nv);
}

static void
dump_history_offsets (objset_t *os, uint64_t object, void *data, size_t size)
{
  spa_history_phys_t *shp = data;

  if (shp == NULL)
    return;

  printf ("\t\tpool_create_len = %llu\n",
          (u_longlong_t)shp->sh_pool_create_len);
  printf ("\t\tphys_max_off = %llu\n", (u_longlong_t)shp->sh_phys_max_off);
  printf ("\t\tbof = %llu\n", (u_longlong_t)shp->sh_bof);
  printf ("\t\teof = %llu\n", (u_longlong_t)shp->sh_eof);
  printf ("\t\trecords_lost = %llu\n", (u_longlong_t)shp->sh_records_lost);
}

static void
zdb_nicenum (uint64_t num, char *buf, size_t buflen)
{
  if (dump_opt['P'])
    snprintf (buf, buflen, "%llu", (longlong_t)num);
  else
    nicenum (num, buf, sizeof (buf));
}

static void
snprintf_blkptr_compact (char *blkbuf, size_t buflen, const blkptr_t *bp)
{
  const dva_t *dva = bp->blk_dva;
  int ndvas = dump_opt['d'] > 5 ? BP_GET_NDVAS (bp) : 1;
  int i;

  if (dump_opt['b'] >= 6)
    {
      snprintf_blkptr (blkbuf, buflen, bp);
      return;
    }

  if (BP_IS_EMBEDDED (bp))
    {
      sprintf (blkbuf, "EMBEDDED et=%u %llxL/%llxP B=%llu",
               (int)BPE_GET_ETYPE (bp), (u_longlong_t)BPE_GET_LSIZE (bp),
               (u_longlong_t)BPE_GET_PSIZE (bp), (u_longlong_t)bp->blk_birth);
      return;
    }

  blkbuf[0] = '\0';

  for (i = 0; i < ndvas; i++)
    snprintf (blkbuf + strlen (blkbuf), buflen - strlen (blkbuf),
              "%llu:%llx:%llx ", (u_longlong_t)DVA_GET_VDEV (&dva[i]),
              (u_longlong_t)DVA_GET_OFFSET (&dva[i]),
              (u_longlong_t)DVA_GET_ASIZE (&dva[i]));

  if (BP_IS_HOLE (bp))
    {
      snprintf (blkbuf + strlen (blkbuf), buflen - strlen (blkbuf),
                "%llxL B=%llu", (u_longlong_t)BP_GET_LSIZE (bp),
                (u_longlong_t)bp->blk_birth);
    }
  else
    {
      snprintf (
          blkbuf + strlen (blkbuf), buflen - strlen (blkbuf),
          "%llxL/%llxP F=%llu B=%llu/%llu", (u_longlong_t)BP_GET_LSIZE (bp),
          (u_longlong_t)BP_GET_PSIZE (bp), (u_longlong_t)BP_GET_FILL (bp),
          (u_longlong_t)bp->blk_birth, (u_longlong_t)BP_PHYSICAL_BIRTH (bp));
      snprintf (blkbuf + strlen (blkbuf), buflen - strlen (blkbuf),
                " cksum=%llx:%llx:%llx:%llx",
                (u_longlong_t)bp->blk_cksum.zc_word[0],
                (u_longlong_t)bp->blk_cksum.zc_word[1],
                (u_longlong_t)bp->blk_cksum.zc_word[2],
                (u_longlong_t)bp->blk_cksum.zc_word[3]);
    }
}

static const char histo_stars[] = "****************************************";
static const uint64_t histo_width = sizeof (histo_stars) - 1;

static void
dump_histogram (const uint64_t *histo, int size, int offset)
{
  int i;
  int minidx = size - 1;
  int maxidx = 0;
  uint64_t max = 0;

  for (i = 0; i < size; i++)
    {
      if (histo[i] > max)
        max = histo[i];
      if (histo[i] > 0 && i > maxidx)
        maxidx = i;
      if (histo[i] > 0 && i < minidx)
        minidx = i;
    }

  if (max < histo_width)
    max = histo_width;

  for (i = minidx; i <= maxidx; i++)
    {
      printf ("\t\t\t%3u: %6llu %s\n", i + offset, (u_longlong_t)histo[i],
              &histo_stars[(max - histo[i]) * histo_width / max]);
    }
}

static void
dump_zap_stats (objset_t *os, uint64_t object)
{
  int error;
  zap_stats_t zs;

  error = zap_get_stats (os, object, &zs);
  if (error)
    return;

  if (zs.zs_ptrtbl_len == 0)
    {
      ASSERT (zs.zs_num_blocks == 1);
      printf ("\tmicrozap: %llu bytes, %llu entries\n",
              (u_longlong_t)zs.zs_blocksize, (u_longlong_t)zs.zs_num_entries);
      return;
    }

  printf ("\tFat ZAP stats:\n");

  printf ("\t\tPointer table:\n");
  printf ("\t\t\t%llu elements\n", (u_longlong_t)zs.zs_ptrtbl_len);
  printf ("\t\t\tzt_blk: %llu\n", (u_longlong_t)zs.zs_ptrtbl_zt_blk);
  printf ("\t\t\tzt_numblks: %llu\n", (u_longlong_t)zs.zs_ptrtbl_zt_numblks);
  printf ("\t\t\tzt_shift: %llu\n", (u_longlong_t)zs.zs_ptrtbl_zt_shift);
  printf ("\t\t\tzt_blks_copied: %llu\n",
          (u_longlong_t)zs.zs_ptrtbl_blks_copied);
  printf ("\t\t\tzt_nextblk: %llu\n", (u_longlong_t)zs.zs_ptrtbl_nextblk);

  printf ("\t\tZAP entries: %llu\n", (u_longlong_t)zs.zs_num_entries);
  printf ("\t\tLeaf blocks: %llu\n", (u_longlong_t)zs.zs_num_leafs);
  printf ("\t\tTotal blocks: %llu\n", (u_longlong_t)zs.zs_num_blocks);
  printf ("\t\tzap_block_type: 0x%llx\n", (u_longlong_t)zs.zs_block_type);
  printf ("\t\tzap_magic: 0x%llx\n", (u_longlong_t)zs.zs_magic);
  printf ("\t\tzap_salt: 0x%llx\n", (u_longlong_t)zs.zs_salt);

  printf ("\t\tLeafs with 2^n pointers:\n");
  dump_histogram (zs.zs_leafs_with_2n_pointers, ZAP_HISTOGRAM_SIZE, 0);

  printf ("\t\tBlocks with n*5 entries:\n");
  dump_histogram (zs.zs_blocks_with_n5_entries, ZAP_HISTOGRAM_SIZE, 0);

  printf ("\t\tBlocks n/10 full:\n");
  dump_histogram (zs.zs_blocks_n_tenths_full, ZAP_HISTOGRAM_SIZE, 0);

  printf ("\t\tEntries with n chunks:\n");
  dump_histogram (zs.zs_entries_using_n_chunks, ZAP_HISTOGRAM_SIZE, 0);

  printf ("\t\tBuckets with n entries:\n");
  dump_histogram (zs.zs_buckets_with_n_entries, ZAP_HISTOGRAM_SIZE, 0);
}

static void
dump_none (objset_t *os, uint64_t object, void *data, size_t size)
{
}

static void
dump_unknown (objset_t *os, uint64_t object, void *data, size_t size)
{
  printf ("\tUNKNOWN OBJECT TYPE\n");
}

static void
dump_uint8 (objset_t *os, uint64_t object, void *data, size_t size)
{
}

static void
dump_uint64 (objset_t *os, uint64_t object, void *data, size_t size)
{
}

static void
dump_zap (objset_t *os, uint64_t object, void *data, size_t size)
{
  zap_cursor_t zc;
  zap_attribute_t attr;
  void *prop;
  unsigned i;

  dump_zap_stats (os, object);
  printf ("\n");

  for (zap_cursor_init (&zc, os, object);
       zap_cursor_retrieve (&zc, &attr) == 0; zap_cursor_advance (&zc))
    {
      printf ("\t\t%s = ", attr.za_name);
      if (attr.za_num_integers == 0)
        {
          printf ("\n");
          continue;
        }
      prop = umem_zalloc (attr.za_num_integers * attr.za_integer_length,
                          UMEM_NOFAIL);
      zap_lookup (os, object, attr.za_name, attr.za_integer_length,
                  attr.za_num_integers, prop);
      if (attr.za_integer_length == 1)
        {
          printf ("%s", (char *)prop);
        }
      else
        {
          for (i = 0; i < attr.za_num_integers; i++)
            {
              switch (attr.za_integer_length)
                {
                case 2:
                  printf ("%u ", ((uint16_t *)prop)[i]);
                  break;
                case 4:
                  printf ("%u ", ((uint32_t *)prop)[i]);
                  break;
                case 8:
                  printf ("%lld ", (u_longlong_t)((int64_t *)prop)[i]);
                  break;
                }
            }
        }
      printf ("\n");
      umem_free (prop, attr.za_num_integers * attr.za_integer_length);
    }
  zap_cursor_fini (&zc);
}

static void
dump_bpobj (objset_t *os, uint64_t object, void *data, size_t size)
{
  bpobj_phys_t *bpop = data;
  uint64_t i;
  char bytes[32], comp[32], uncomp[32];

  /* make sure the output won't get truncated */
  CTASSERT (sizeof (bytes) >= NN_NUMBUF_SZ);
  CTASSERT (sizeof (comp) >= NN_NUMBUF_SZ);
  CTASSERT (sizeof (uncomp) >= NN_NUMBUF_SZ);

  if (bpop == NULL)
    return;

  zdb_nicenum (bpop->bpo_bytes, bytes, sizeof (bytes));
  zdb_nicenum (bpop->bpo_comp, comp, sizeof (comp));
  zdb_nicenum (bpop->bpo_uncomp, uncomp, sizeof (uncomp));

  printf ("\t\tnum_blkptrs = %llu\n", (u_longlong_t)bpop->bpo_num_blkptrs);
  printf ("\t\tbytes = %s\n", bytes);
  if (size >= BPOBJ_SIZE_V1)
    {
      printf ("\t\tcomp = %s\n", comp);
      printf ("\t\tuncomp = %s\n", uncomp);
    }
  if (size >= sizeof (*bpop))
    {
      printf ("\t\tsubobjs = %llu\n", (u_longlong_t)bpop->bpo_subobjs);
      printf ("\t\tnum_subobjs = %llu\n", (u_longlong_t)bpop->bpo_num_subobjs);
    }

  if (dump_opt['d'] < 5)
    return;

  for (i = 0; i < bpop->bpo_num_blkptrs; i++)
    {
      char blkbuf[BP_SPRINTF_LEN];
      blkptr_t bp;

      int err = dmu_read (os, object, i * sizeof (bp), sizeof (bp), &bp, 0);
      if (err != 0)
        {
          printf ("got error %u from dmu_read\n", err);
          break;
        }
      snprintf_blkptr_compact (blkbuf, sizeof (blkbuf), &bp);
      printf ("\t%s\n", blkbuf);
    }
}

static void
dump_bpobj_subobjs (objset_t *os, uint64_t object, void *data, size_t size)
{
  dmu_object_info_t doi;
  int64_t i;

  VERIFY0 (dmu_object_info (os, object, &doi));
  uint64_t *subobjs = kmem_alloc (doi.doi_max_offset, KM_SLEEP);

  int err = dmu_read (os, object, 0, doi.doi_max_offset, subobjs, 0);
  if (err != 0)
    {
      printf ("got error %u from dmu_read\n", err);
      kmem_free (subobjs, doi.doi_max_offset);
      return;
    }

  int64_t last_nonzero = -1;
  for (i = 0; i < doi.doi_max_offset / 8; i++)
    {
      if (subobjs[i] != 0)
        last_nonzero = i;
    }

  for (i = 0; i <= last_nonzero; i++)
    {
      printf ("\t%llu\n", (u_longlong_t)subobjs[i]);
    }
  kmem_free (subobjs, doi.doi_max_offset);
}

static void
dump_ddt_zap (objset_t *os, uint64_t object, void *data, size_t size)
{
  dump_zap_stats (os, object);
  /* contents are printed elsewhere, properly decoded */
}

static void
dump_sa_attrs (objset_t *os, uint64_t object, void *data, size_t size)
{
  zap_cursor_t zc;
  zap_attribute_t attr;

  dump_zap_stats (os, object);
  printf ("\n");

  for (zap_cursor_init (&zc, os, object);
       zap_cursor_retrieve (&zc, &attr) == 0; zap_cursor_advance (&zc))
    {
      printf ("\t\t%s = ", attr.za_name);
      if (attr.za_num_integers == 0)
        {
          printf ("\n");
          continue;
        }
      printf (" %llx : [%d:%d:%d]\n", (u_longlong_t)attr.za_first_integer,
              (int)ATTR_LENGTH (attr.za_first_integer),
              (int)ATTR_BSWAP (attr.za_first_integer),
              (int)ATTR_NUM (attr.za_first_integer));
    }
  zap_cursor_fini (&zc);
}

static void
dump_sa_layouts (objset_t *os, uint64_t object, void *data, size_t size)
{
  zap_cursor_t zc;
  zap_attribute_t attr;
  uint16_t *layout_attrs;
  unsigned i;

  dump_zap_stats (os, object);
  printf ("\n");

  for (zap_cursor_init (&zc, os, object);
       zap_cursor_retrieve (&zc, &attr) == 0; zap_cursor_advance (&zc))
    {
      printf ("\t\t%s = [", attr.za_name);
      if (attr.za_num_integers == 0)
        {
          printf ("\n");
          continue;
        }

      VERIFY (attr.za_integer_length == 2);
      layout_attrs = umem_zalloc (
          attr.za_num_integers * attr.za_integer_length, UMEM_NOFAIL);

      VERIFY (zap_lookup (os, object, attr.za_name, attr.za_integer_length,
                          attr.za_num_integers, layout_attrs)
              == 0);

      for (i = 0; i != attr.za_num_integers; i++)
        printf (" %d ", (int)layout_attrs[i]);
      printf ("]\n");
      umem_free (layout_attrs, attr.za_num_integers * attr.za_integer_length);
    }
  zap_cursor_fini (&zc);
}

static void
dump_zpldir (objset_t *os, uint64_t object, void *data, size_t size)
{
  zap_cursor_t zc;
  zap_attribute_t attr;
  const char *typenames[] = {
    /* 0 */ "not specified",
    /* 1 */ "FIFO",
    /* 2 */ "Character Device",
    /* 3 */ "3 (invalid)",
    /* 4 */ "Directory",
    /* 5 */ "5 (invalid)",
    /* 6 */ "Block Device",
    /* 7 */ "7 (invalid)",
    /* 8 */ "Regular File",
    /* 9 */ "9 (invalid)",
    /* 10 */ "Symbolic Link",
    /* 11 */ "11 (invalid)",
    /* 12 */ "Socket",
    /* 13 */ "Door",
    /* 14 */ "Event Port",
    /* 15 */ "15 (invalid)",
  };

  dump_zap_stats (os, object);
  printf ("\n");

  for (zap_cursor_init (&zc, os, object);
       zap_cursor_retrieve (&zc, &attr) == 0; zap_cursor_advance (&zc))
    {
      printf ("\t\t%s = %lld (type: %s)\n", attr.za_name,
              ZFS_DIRENT_OBJ (attr.za_first_integer),
              typenames[ZFS_DIRENT_TYPE (attr.za_first_integer)]);
    }
  zap_cursor_fini (&zc);
}

static uint64_t
blkid2offset (const dnode_phys_t *dnp, const blkptr_t *bp,
              const zbookmark_phys_t *zb)
{
  if (dnp == NULL)
    {
      ASSERT (zb->zb_level < 0);
      if (zb->zb_object == 0)
        return (zb->zb_blkid);
      return (zb->zb_blkid * BP_GET_LSIZE (bp));
    }

  ASSERT (zb->zb_level >= 0);

  return ((zb->zb_blkid << (zb->zb_level
                            * (dnp->dn_indblkshift - SPA_BLKPTRSHIFT)))
              * dnp->dn_datablkszsec
          << SPA_MINBLOCKSHIFT);
}

static void
print_indirect (blkptr_t *bp, const zbookmark_phys_t *zb,
                const dnode_phys_t *dnp)
{
  char blkbuf[BP_SPRINTF_LEN];
  int l;

  if (!BP_IS_EMBEDDED (bp))
    {
      ASSERT3U (BP_GET_TYPE (bp), ==, dnp->dn_type);
      ASSERT3U (BP_GET_LEVEL (bp), ==, zb->zb_level);
    }

  printf ("%16llx ", (u_longlong_t)blkid2offset (dnp, bp, zb));

  ASSERT (zb->zb_level >= 0);

  for (l = dnp->dn_nlevels - 1; l >= -1; l--)
    {
      if (l == zb->zb_level)
        {
          printf ("L%llx", (u_longlong_t)zb->zb_level);
        }
      else
        {
          printf (" ");
        }
    }

  snprintf_blkptr_compact (blkbuf, sizeof (blkbuf), bp);
  printf ("%s\n", blkbuf);
}

static int
visit_indirect (spa_t *spa, const dnode_phys_t *dnp, blkptr_t *bp,
                const zbookmark_phys_t *zb)
{
  int err = 0;

  if (bp->blk_birth == 0)
    return (0);

  print_indirect (bp, zb, dnp);

  if (BP_GET_LEVEL (bp) > 0 && !BP_IS_HOLE (bp))
    {
      arc_flags_t flags = ARC_FLAG_WAIT;
      int i;
      blkptr_t *cbp;
      int epb = BP_GET_LSIZE (bp) >> SPA_BLKPTRSHIFT;
      arc_buf_t *buf;
      uint64_t fill = 0;

      err = arc_read (NULL, spa, bp, arc_getbuf_func, &buf,
                      ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL, &flags, zb);
      if (err)
        return (err);
      ASSERT (buf->b_data);

      /* recursively visit blocks below this */
      cbp = buf->b_data;
      for (i = 0; i < epb; i++, cbp++)
        {
          zbookmark_phys_t czb;

          SET_BOOKMARK (&czb, zb->zb_objset, zb->zb_object, zb->zb_level - 1,
                        zb->zb_blkid * epb + i);
          err = visit_indirect (spa, dnp, cbp, &czb);
          if (err)
            break;
          fill += BP_GET_FILL (cbp);
        }
      if (!err)
        ASSERT3U (fill, ==, BP_GET_FILL (bp));
      arc_buf_destroy (buf, &buf);
    }

  return (err);
}

static void
dump_indirect (dnode_t *dn)
{
  dnode_phys_t *dnp = dn->dn_phys;
  int j;
  zbookmark_phys_t czb;

  printf ("Indirect blocks:\n");

  SET_BOOKMARK (&czb, dmu_objset_id (dn->dn_objset), dn->dn_object,
                dnp->dn_nlevels - 1, 0);
  for (j = 0; j < dnp->dn_nblkptr; j++)
    {
      czb.zb_blkid = j;
      visit_indirect (dmu_objset_spa (dn->dn_objset), dnp, &dnp->dn_blkptr[j],
                      &czb);
    }

  printf ("\n");
}

static void
dump_dsl_dir (objset_t *os, uint64_t object, void *data, size_t size)
{
  dsl_dir_phys_t *dd = data;
  time_t crtime;
  char nice[32];

  /* make sure nicenum has enough space */
  CTASSERT (sizeof (nice) >= NN_NUMBUF_SZ);

  if (dd == NULL)
    return;

  ASSERT3U (size, >=, sizeof (dsl_dir_phys_t));

  crtime = dd->dd_creation_time;
  printf ("\t\tcreation_time = %s", ctime (&crtime));
  printf ("\t\thead_dataset_obj = %llu\n",
          (u_longlong_t)dd->dd_head_dataset_obj);
  printf ("\t\tparent_dir_obj = %llu\n", (u_longlong_t)dd->dd_parent_obj);
  printf ("\t\torigin_obj = %llu\n", (u_longlong_t)dd->dd_origin_obj);
  printf ("\t\tchild_dir_zapobj = %llu\n",
          (u_longlong_t)dd->dd_child_dir_zapobj);
  zdb_nicenum (dd->dd_used_bytes, nice, sizeof (nice));
  printf ("\t\tused_bytes = %s\n", nice);
  zdb_nicenum (dd->dd_compressed_bytes, nice, sizeof (nice));
  printf ("\t\tcompressed_bytes = %s\n", nice);
  zdb_nicenum (dd->dd_uncompressed_bytes, nice, sizeof (nice));
  printf ("\t\tuncompressed_bytes = %s\n", nice);
  zdb_nicenum (dd->dd_quota, nice, sizeof (nice));
  printf ("\t\tquota = %s\n", nice);
  zdb_nicenum (dd->dd_reserved, nice, sizeof (nice));
  printf ("\t\treserved = %s\n", nice);
  printf ("\t\tprops_zapobj = %llu\n", (u_longlong_t)dd->dd_props_zapobj);
  printf ("\t\tdeleg_zapobj = %llu\n", (u_longlong_t)dd->dd_deleg_zapobj);
  printf ("\t\tflags = %llx\n", (u_longlong_t)dd->dd_flags);

#define DO(which)                                                             \
  zdb_nicenum (dd->dd_used_breakdown[DD_USED_##which], nice, sizeof (nice));  \
  printf ("\t\tused_breakdown[" #which "] = %s\n", nice)
  DO (HEAD);
  DO (SNAP);
  DO (CHILD);
  DO (CHILD_RSRV);
  DO (REFRSRV);
#undef DO
  printf ("\t\tclones = %llu\n", (u_longlong_t)dd->dd_clones);
}

static void
dump_dsl_dataset (objset_t *os, uint64_t object, void *data, size_t size)
{
  dsl_dataset_phys_t *ds = data;
  time_t crtime;
  char used[32], compressed[32], uncompressed[32], unique[32];
  char blkbuf[BP_SPRINTF_LEN];

  /* make sure nicenum has enough space */
  CTASSERT (sizeof (used) >= NN_NUMBUF_SZ);
  CTASSERT (sizeof (compressed) >= NN_NUMBUF_SZ);
  CTASSERT (sizeof (uncompressed) >= NN_NUMBUF_SZ);
  CTASSERT (sizeof (unique) >= NN_NUMBUF_SZ);

  if (ds == NULL)
    return;

  ASSERT (size == sizeof (*ds));
  crtime = ds->ds_creation_time;
  zdb_nicenum (ds->ds_referenced_bytes, used, sizeof (used));
  zdb_nicenum (ds->ds_compressed_bytes, compressed, sizeof (compressed));
  zdb_nicenum (ds->ds_uncompressed_bytes, uncompressed, sizeof (uncompressed));
  zdb_nicenum (ds->ds_unique_bytes, unique, sizeof (unique));
  snprintf_blkptr (blkbuf, sizeof (blkbuf), &ds->ds_bp);

  printf ("\t\tdir_obj = %llu\n", (u_longlong_t)ds->ds_dir_obj);
  printf ("\t\tprev_snap_obj = %llu\n", (u_longlong_t)ds->ds_prev_snap_obj);
  printf ("\t\tprev_snap_txg = %llu\n", (u_longlong_t)ds->ds_prev_snap_txg);
  printf ("\t\tnext_snap_obj = %llu\n", (u_longlong_t)ds->ds_next_snap_obj);
  printf ("\t\tsnapnames_zapobj = %llu\n",
          (u_longlong_t)ds->ds_snapnames_zapobj);
  printf ("\t\tnum_children = %llu\n", (u_longlong_t)ds->ds_num_children);
  printf ("\t\tuserrefs_obj = %llu\n", (u_longlong_t)ds->ds_userrefs_obj);
  printf ("\t\tcreation_time = %s", ctime (&crtime));
  printf ("\t\tcreation_txg = %llu\n", (u_longlong_t)ds->ds_creation_txg);
  printf ("\t\tdeadlist_obj = %llu\n", (u_longlong_t)ds->ds_deadlist_obj);
  printf ("\t\tused_bytes = %s\n", used);
  printf ("\t\tcompressed_bytes = %s\n", compressed);
  printf ("\t\tuncompressed_bytes = %s\n", uncompressed);
  printf ("\t\tunique = %s\n", unique);
  printf ("\t\tfsid_guid = %llu\n", (u_longlong_t)ds->ds_fsid_guid);
  printf ("\t\tguid = %llu\n", (u_longlong_t)ds->ds_guid);
  printf ("\t\tflags = %llx\n", (u_longlong_t)ds->ds_flags);
  printf ("\t\tnext_clones_obj = %llu\n",
          (u_longlong_t)ds->ds_next_clones_obj);
  printf ("\t\tprops_obj = %llu\n", (u_longlong_t)ds->ds_props_obj);
  printf ("\t\tbp = %s\n", blkbuf);
}

static void
dump_dnode (objset_t *os, uint64_t object, void *data, size_t size)
{
}

static void
print_idstr (uint64_t id, const char *id_type)
{
  if (FUID_INDEX (id))
    {
      char *domain;

      domain = zfs_fuid_idx_domain (&idx_tree, FUID_INDEX (id));
      printf ("\t%s     %llx [%s-%d]\n", id_type, (u_longlong_t)id, domain,
              (int)FUID_RID (id));
    }
  else
    {
      printf ("\t%s     %llu\n", id_type, (u_longlong_t)id);
    }
}

static void
dump_uidgid (objset_t *os, uint64_t uid, uint64_t gid)
{
  uint32_t uid_idx, gid_idx;

  uid_idx = FUID_INDEX (uid);
  gid_idx = FUID_INDEX (gid);

  /* Load domain table, if not already loaded */
  if (!fuid_table_loaded && (uid_idx || gid_idx))
    {
      uint64_t fuid_obj;

      /* first find the fuid object.  It lives in the master node */
      VERIFY (
          zap_lookup (os, MASTER_NODE_OBJ, ZFS_FUID_TABLES, 8, 1, &fuid_obj)
          == 0);
      zfs_fuid_avl_tree_create (&idx_tree, &domain_tree);
      zfs_fuid_table_load (os, fuid_obj, &idx_tree, &domain_tree);
      fuid_table_loaded = B_TRUE;
    }

  print_idstr (uid, "uid");
  print_idstr (gid, "gid");
}

static void
dump_znode_sa_xattr (sa_handle_t *hdl)
{
  nvlist_t *sa_xattr;
  nvpair_t *elem = NULL;
  int sa_xattr_size = 0;
  int sa_xattr_entries = 0;
  int error;
  char *sa_xattr_packed;

  error = sa_size (hdl, sa_attr_table[ZPL_DXATTR], &sa_xattr_size);
  if (error || sa_xattr_size == 0)
    return;

  sa_xattr_packed = malloc (sa_xattr_size);
  if (sa_xattr_packed == NULL)
    return;

  error = sa_lookup (hdl, sa_attr_table[ZPL_DXATTR], sa_xattr_packed,
                     sa_xattr_size);
  if (error)
    {
      free (sa_xattr_packed);
      return;
    }

  error = nvlist_unpack (sa_xattr_packed, sa_xattr_size, &sa_xattr, 0);
  if (error)
    {
      free (sa_xattr_packed);
      return;
    }

  while ((elem = nvlist_next_nvpair (sa_xattr, elem)) != NULL)
    sa_xattr_entries++;

  printf ("\tSA xattrs: %d bytes, %d entries\n\n", sa_xattr_size,
          sa_xattr_entries);
  while ((elem = nvlist_next_nvpair (sa_xattr, elem)) != NULL)
    {
      uchar_t *value;
      uint_t cnt, idx;

      printf ("\t\t%s = ", nvpair_name (elem));
      nvpair_value_byte_array (elem, &value, &cnt);
      for (idx = 0; idx < cnt; ++idx)
        {
          if (isprint (value[idx]))
            putchar (value[idx]);
          else
            printf ("\\%3.3o", value[idx]);
        }
      putchar ('\n');
    }

  nvlist_free (sa_xattr);
  free (sa_xattr_packed);
}

static void
dump_znode (objset_t *os, uint64_t object, void *data, size_t size)
{
  char path[MAXPATHLEN * 2]; /* allow for xattr and failure prefix */
  sa_handle_t *hdl;
  uint64_t xattr, rdev, gen;
  uint64_t uid, gid, mode, fsize, parent, links;
  uint64_t pflags;
  uint64_t acctm[2], modtm[2], chgtm[2], crtm[2];
  time_t z_crtime, z_atime, z_mtime, z_ctime;
  sa_bulk_attr_t bulk[12];
  int idx = 0;
  int error;

  VERIFY3P (os, ==, sa_os);
  if (sa_handle_get (os, object, NULL, SA_HDL_PRIVATE, &hdl))
    {
      printf ("Failed to get handle for SA znode\n");
      return;
    }

  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_UID], NULL, &uid, 8);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_GID], NULL, &gid, 8);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_LINKS], NULL, &links, 8);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_GEN], NULL, &gen, 8);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_MODE], NULL, &mode, 8);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_PARENT], NULL, &parent, 8);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_SIZE], NULL, &fsize, 8);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_ATIME], NULL, acctm, 16);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_MTIME], NULL, modtm, 16);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_CRTIME], NULL, crtm, 16);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_CTIME], NULL, chgtm, 16);
  SA_ADD_BULK_ATTR (bulk, idx, sa_attr_table[ZPL_FLAGS], NULL, &pflags, 8);

  if (sa_bulk_lookup (hdl, bulk, idx))
    {
      sa_handle_destroy (hdl);
      return;
    }

  z_crtime = (time_t)crtm[0];
  z_atime = (time_t)acctm[0];
  z_mtime = (time_t)modtm[0];
  z_ctime = (time_t)chgtm[0];

  if (dump_opt['d'] > 4)
    {
      error = zfs_obj_to_path (os, object, path, sizeof (path));
      if (error == ESTALE)
        {
          snprintf (path, sizeof (path), "on delete queue");
        }
      else if (error != 0)
        {
          leaked_objects++;
          snprintf (path, sizeof (path), "path not found, possibly leaked");
        }
      printf ("\tpath	%s\n", path);
    }
  dump_uidgid (os, uid, gid);
  printf ("\tatime	%s", ctime (&z_atime));
  printf ("\tmtime	%s", ctime (&z_mtime));
  printf ("\tctime	%s", ctime (&z_ctime));
  printf ("\tcrtime	%s", ctime (&z_crtime));
  printf ("\tgen	%llu\n", (u_longlong_t)gen);
  printf ("\tmode	%llo\n", (u_longlong_t)mode);
  printf ("\tsize	%llu\n", (u_longlong_t)fsize);
  printf ("\tparent	%llu\n", (u_longlong_t)parent);
  printf ("\tlinks	%llu\n", (u_longlong_t)links);
  printf ("\tpflags	%llx\n", (u_longlong_t)pflags);
  if (dmu_objset_projectquota_enabled (os) && (pflags & ZFS_PROJID))
    {
      uint64_t projid;

      if (sa_lookup (hdl, sa_attr_table[ZPL_PROJID], &projid,
                     sizeof (uint64_t))
          == 0)
        printf ("\tprojid	%llu\n", (u_longlong_t)projid);
    }
  if (sa_lookup (hdl, sa_attr_table[ZPL_XATTR], &xattr, sizeof (uint64_t))
      == 0)
    printf ("\txattr	%llu\n", (u_longlong_t)xattr);
  if (sa_lookup (hdl, sa_attr_table[ZPL_RDEV], &rdev, sizeof (uint64_t)) == 0)
    printf ("\trdev	0x%016llx\n", (u_longlong_t)rdev);
  dump_znode_sa_xattr (hdl);
  sa_handle_destroy (hdl);
}

static void
dump_acl (objset_t *os, uint64_t object, void *data, size_t size)
{
}

static void
dump_dmu_objset (objset_t *os, uint64_t object, void *data, size_t size)
{
}

static void
dump_debug_buffer ()
{
  if (dump_opt['G'])
    {
      printf ("\n");
      fflush (stdout);
      zfs_dbgmsg_print ("zdb");
    }
}

static void
fatal (const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  fprintf (stderr, "%s: ", cmdname);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fprintf (stderr, "\n");

  dump_debug_buffer ();

  exit (EXIT_FAILURE);
}

typedef void object_viewer_t (objset_t *, uint64_t, void *data, size_t size);

static object_viewer_t *object_viewer[DMU_OT_NUMTYPES + 1] = {
  dump_none,            /* unallocated			*/
  dump_zap,             /* object directory		*/
  dump_uint64,          /* object array			*/
  dump_none,            /* packed nvlist		*/
  dump_packed_nvlist,   /* packed nvlist size		*/
  dump_none,            /* bpobj			*/
  dump_bpobj,           /* bpobj header			*/
  dump_none,            /* SPA space map header		*/
  dump_none,            /* SPA space map		*/
  dump_none,            /* ZIL intent log		*/
  dump_dnode,           /* DMU dnode			*/
  dump_dmu_objset,      /* DMU objset			*/
  dump_dsl_dir,         /* DSL directory		*/
  dump_zap,             /* DSL directory child map	*/
  dump_zap,             /* DSL dataset snap map		*/
  dump_zap,             /* DSL props			*/
  dump_dsl_dataset,     /* DSL dataset			*/
  dump_znode,           /* ZFS znode			*/
  dump_acl,             /* ZFS V0 ACL			*/
  dump_uint8,           /* ZFS plain file		*/
  dump_zpldir,          /* ZFS directory		*/
  dump_zap,             /* ZFS master node		*/
  dump_zap,             /* ZFS delete queue		*/
  dump_uint8,           /* zvol object			*/
  dump_zap,             /* zvol prop			*/
  dump_uint8,           /* other uint8[]		*/
  dump_uint64,          /* other uint64[]		*/
  dump_zap,             /* other ZAP			*/
  dump_zap,             /* persistent error log		*/
  dump_uint8,           /* SPA history			*/
  dump_history_offsets, /* SPA history offsets		*/
  dump_zap,             /* Pool properties		*/
  dump_zap,             /* DSL permissions		*/
  dump_acl,             /* ZFS ACL			*/
  dump_uint8,           /* ZFS SYSACL			*/
  dump_none,            /* FUID nvlist			*/
  dump_packed_nvlist,   /* FUID nvlist size		*/
  dump_zap,             /* DSL dataset next clones	*/
  dump_zap,             /* DSL scrub queue		*/
  dump_zap,             /* ZFS user/group/project used	*/
  dump_zap,             /* ZFS user/group/project quota	*/
  dump_zap,             /* snapshot refcount tags	*/
  dump_ddt_zap,         /* DDT ZAP object		*/
  dump_zap,             /* DDT statistics		*/
  dump_znode,           /* SA object			*/
  dump_zap,             /* SA Master Node		*/
  dump_sa_attrs,        /* SA attribute registration	*/
  dump_sa_layouts,      /* SA attribute layouts		*/
  dump_zap,             /* DSL scrub translations	*/
  dump_none,            /* fake dedup BP		*/
  dump_zap,             /* deadlist			*/
  dump_none,            /* deadlist hdr			*/
  dump_zap,             /* dsl clones			*/
  dump_bpobj_subobjs,   /* bpobj subobjs		*/
  dump_unknown,         /* Unknown type, must be last	*/
};

static void
dump_object (objset_t *os, uint64_t object, int verbosity, int *print_header,
             uint64_t *dnode_slots_used)
{
  dmu_buf_t *db = NULL;
  dmu_object_info_t doi;
  dnode_t *dn;
  boolean_t dnode_held = B_FALSE;
  void *bonus = NULL;
  size_t bsize = 0;
  char iblk[32], dblk[32], lsize[32], asize[32], fill[32], dnsize[32];
  char bonus_size[32];
  char aux[50];
  int error;

  /* make sure nicenum has enough space */
  CTASSERT (sizeof (iblk) >= NN_NUMBUF_SZ);
  CTASSERT (sizeof (dblk) >= NN_NUMBUF_SZ);
  CTASSERT (sizeof (lsize) >= NN_NUMBUF_SZ);
  CTASSERT (sizeof (asize) >= NN_NUMBUF_SZ);
  CTASSERT (sizeof (bonus_size) >= NN_NUMBUF_SZ);

  if (*print_header)
    {
      printf ("\n%10s  %3s  %5s  %5s  %5s  %6s  %5s  %6s  %s\n", "Object",
              "lvl", "iblk", "dblk", "dsize", "dnsize", "lsize", "%full",
              "type");
      *print_header = 0;
    }

  if (object == 0)
    {
      dn = DMU_META_DNODE (os);
      dmu_object_info_from_dnode (dn, &doi);
    }
  else
    {
      /*
       * Encrypted datasets will have sensitive bonus buffers
       * encrypted. Therefore we cannot hold the bonus buffer and
       * must hold the dnode itself instead.
       */
      error = dmu_object_info (os, object, &doi);
      if (error)
        fatal ("dmu_object_info() failed, errno %u", error);

      if (os->os_encrypted && DMU_OT_IS_ENCRYPTED (doi.doi_bonus_type))
        {
          error = dnode_hold (os, object, FTAG, &dn);
          if (error)
            fatal ("dnode_hold() failed, errno %u", error);
          dnode_held = B_TRUE;
        }
      else
        {
          error = dmu_bonus_hold (os, object, FTAG, &db);
          if (error)
            fatal ("dmu_bonus_hold(%llu) failed, errno %u", object, error);
          bonus = db->db_data;
          bsize = db->db_size;
          dn = DB_DNODE ((dmu_buf_impl_t *)db);
        }
    }

  if (dnode_slots_used)
    *dnode_slots_used = doi.doi_dnodesize / DNODE_MIN_SIZE;

  zdb_nicenum (doi.doi_metadata_block_size, iblk, sizeof (iblk));
  zdb_nicenum (doi.doi_data_block_size, dblk, sizeof (dblk));
  zdb_nicenum (doi.doi_max_offset, lsize, sizeof (lsize));
  zdb_nicenum (doi.doi_physical_blocks_512 << 9, asize, sizeof (asize));
  zdb_nicenum (doi.doi_bonus_size, bonus_size, sizeof (bonus_size));
  zdb_nicenum (doi.doi_dnodesize, dnsize, sizeof (dnsize));
  sprintf (fill, "%6.2f",
           100.0 * doi.doi_fill_count * doi.doi_data_block_size
               / (object == 0 ? DNODES_PER_BLOCK : 1) / doi.doi_max_offset);

  aux[0] = '\0';

  if (doi.doi_checksum != ZIO_CHECKSUM_INHERIT || verbosity >= 6)
    {
      snprintf (aux + strlen (aux), sizeof (aux) - strlen (aux), " (K=%s)",
                ZDB_CHECKSUM_NAME (doi.doi_checksum));
    }

  if (doi.doi_compress != ZIO_COMPRESS_INHERIT || verbosity >= 6)
    {
      snprintf (aux + strlen (aux), sizeof (aux) - strlen (aux), " (Z=%s)",
                ZDB_COMPRESS_NAME (doi.doi_compress));
    }

  printf ("%10lld  %3u  %5s  %5s  %5s  %6s  %5s  %6s  %s%s\n",
          (u_longlong_t)object, doi.doi_indirection, iblk, dblk, asize, dnsize,
          lsize, fill, zdb_ot_name (doi.doi_type), aux);

  if (doi.doi_bonus_type != DMU_OT_NONE && verbosity > 3)
    {
      printf ("%10s  %3s  %5s  %5s  %5s  %5s  %5s  %6s  %s\n", "", "", "", "",
              "", "", bonus_size, "bonus", zdb_ot_name (doi.doi_bonus_type));
    }

  if (verbosity >= 4)
    {
      printf (
          "\tdnode flags: %s%s%s%s\n",
          (dn->dn_phys->dn_flags & DNODE_FLAG_USED_BYTES) ? "USED_BYTES " : "",
          (dn->dn_phys->dn_flags & DNODE_FLAG_USERUSED_ACCOUNTED)
              ? "USERUSED_ACCOUNTED "
              : "",
          (dn->dn_phys->dn_flags & DNODE_FLAG_USEROBJUSED_ACCOUNTED)
              ? "USEROBJUSED_ACCOUNTED "
              : "",
          (dn->dn_phys->dn_flags & DNODE_FLAG_SPILL_BLKPTR) ? "SPILL_BLKPTR"
                                                            : "");
      printf ("\tdnode maxblkid: %llu\n",
              (longlong_t)dn->dn_phys->dn_maxblkid);

      if (!dnode_held)
        {
          object_viewer[ZDB_OT_TYPE (doi.doi_bonus_type)](os, object, bonus,
                                                          bsize);
        }
      else
        {
          printf ("\t\t(bonus encrypted)\n");
        }

      if (!os->os_encrypted || !DMU_OT_IS_ENCRYPTED (doi.doi_type))
        {
          object_viewer[ZDB_OT_TYPE (doi.doi_type)](os, object, NULL, 0);
        }
      else
        {
          printf ("\t\t(object encrypted)\n");
        }

      *print_header = 1;
    }

  if (verbosity >= 5)
    dump_indirect (dn);

  if (verbosity >= 5)
    {
      /*
       * Report the list of segments that comprise the object.
       */
      uint64_t start = 0;
      uint64_t end;
      uint64_t blkfill = 1;
      int minlvl = 1;

      if (dn->dn_type == DMU_OT_DNODE)
        {
          minlvl = 0;
          blkfill = DNODES_PER_BLOCK;
        }

      for (;;)
        {
          char segsize[32];
          /* make sure nicenum has enough space */
          CTASSERT (sizeof (segsize) >= NN_NUMBUF_SZ);
          error = dnode_next_offset (dn, 0, &start, minlvl, blkfill, 0);
          if (error)
            break;
          end = start;
          error = dnode_next_offset (dn, DNODE_FIND_HOLE, &end, minlvl,
                                     blkfill, 0);
          zdb_nicenum (end - start, segsize, sizeof (segsize));
          printf ("\t\tsegment [%016llx, %016llx)"
                  " size %5s\n",
                  (u_longlong_t)start, (u_longlong_t)end, segsize);
          if (error)
            break;
          start = end;
        }
    }

  if (db != NULL)
    dmu_buf_rele (db, FTAG);
  if (dnode_held)
    dnode_rele (dn, FTAG);
}

static int
dump_path_impl (objset_t *os, uint64_t obj, char *name)
{
  int err;
  int header;
  uint64_t child_obj;
  char *s;
  dmu_buf_t *db;
  dmu_object_info_t doi;

  header = 1;

  if ((s = strchr (name, '/')) != NULL)
    *s = '\0';
  err = zap_lookup (os, obj, name, 8, 1, &child_obj);

  strlcat (curpath, name, sizeof (curpath));

  if (err != 0)
    {
      fprintf (stderr, "failed to lookup %s: %s\n", curpath, strerror (err));
      return err;
    }

  child_obj = ZFS_DIRENT_OBJ (child_obj);
  err = sa_buf_hold (os, child_obj, FTAG, &db);
  if (err != 0)
    {
      fprintf (stderr, "failed to get SA dbuf for obj %llu: %s\n",
               (u_longlong_t)child_obj, strerror (err));
      return EINVAL;
    }
  dmu_object_info_from_db (db, &doi);
  sa_buf_rele (db, FTAG);

  if (doi.doi_bonus_type != DMU_OT_SA && doi.doi_bonus_type != DMU_OT_ZNODE)
    {
      fprintf (stderr, "invalid bonus type %d for obj %llu\n",
               doi.doi_bonus_type, (u_longlong_t)child_obj);
      return EINVAL;
    }

  if (dump_opt['v'] > 6)
    {
      printf ("obj=%llu %s type=%d bonustype=%d\n", (u_longlong_t)child_obj,
              curpath, doi.doi_type, doi.doi_bonus_type);
    }

  strlcat (curpath, "/", sizeof (curpath));

  switch (doi.doi_type)
    {
    case DMU_OT_DIRECTORY_CONTENTS:
      if (s != NULL && *(s + 1) != '\0')
        return dump_path_impl (os, child_obj, s + 1);
      /*FALLTHROUGH*/
    case DMU_OT_PLAIN_FILE_CONTENTS:
      dump_object (os, child_obj, dump_opt['v'], &header, NULL);
      return 0;
    default:
      fprintf (stderr,
               "object %llu has non-file/directory "
               "type %d\n",
               (u_longlong_t)obj, doi.doi_type);
      break;
    }

  return EINVAL;
}

static int
dump_path (char *ds, char *path)
{
  int err;
  objset_t *os;
  uint64_t root_obj;

  err = open_objset (ds, DMU_OST_ZFS, FTAG, &os);
  if (err != 0)
    {
      return err;
    }

  err = zap_lookup (os, MASTER_NODE_OBJ, ZFS_ROOT_OBJ, 8, 1, &root_obj);
  if (err != 0)
    {
      fprintf (stderr, "can't lookup root znode: %s\n", strerror (err));
      dmu_objset_disown (os, B_FALSE, FTAG);
      return EINVAL;
    }

  snprintf (curpath, sizeof (curpath), "dataset=%s path=/", ds);

  err = dump_path_impl (os, root_obj, path);

  close_objset (os, FTAG);
  return (err);
}

int
main (int argc, char *argv[])
{
  memset (dump_opt, 0, sizeof (dump_opt));
  kernel_init (FREAD);
  dump_opt['v'] =99;
  dump_path ("mypool", "file1");
  kernel_fini ();
  return 0;
}
