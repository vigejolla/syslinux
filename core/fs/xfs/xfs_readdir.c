/*
 * Copyright (c) 2012 Paulo Alcantara <pcacjr@zytor.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <cache.h>
#include <core.h>
#include <fs.h>

#include "xfs_types.h"
#include "xfs_sb.h"
#include "xfs_ag.h"
#include "misc.h"
#include "xfs.h"
#include "xfs_dinode.h"
#include "xfs_dir2.h"

#include "xfs_readdir.h"

static int fill_dirent(struct fs_info *fs, struct dirent *dirent,
		       uint32_t offset, xfs_ino_t ino, char *name,
		       size_t namelen)
{
    xfs_dinode_t *core;

    dirent->d_ino = ino;
    dirent->d_off = offset;
    dirent->d_reclen = offsetof(struct dirent, d_name) + namelen + 1;

    core = xfs_dinode_get_core(fs, ino);
    if (!core) {
        xfs_error("Failed to get dinode from disk (ino 0x%llx)", ino);
        return -1;
    }

    if (be16_to_cpu(core->di_mode) & S_IFDIR)
	dirent->d_type = DT_DIR;
    else if (be16_to_cpu(core->di_mode) & S_IFREG)
	dirent->d_type = DT_REG;

    memcpy(dirent->d_name, name, namelen + 1);

    return 0;
}

int xfs_readdir_dir2_local(struct file *file, struct dirent *dirent,
			   xfs_dinode_t *core)
{
    xfs_dir2_sf_t *sf = (xfs_dir2_sf_t *)&core->di_literal_area[0];
    xfs_dir2_sf_entry_t *sf_entry;
    uint8_t count = sf->hdr.i8count ? sf->hdr.i8count : sf->hdr.count;
    uint32_t offset = file->offset;
    uint8_t *start_name;
    uint8_t *end_name;
    char *name;
    xfs_ino_t ino;
    struct fs_info *fs = file->fs;
    int retval = 0;

    xfs_debug("count %hhu i8count %hhu", sf->hdr.count, sf->hdr.i8count);

    if (file->offset + 1 > count)
	return -1;

    file->offset++;

    sf_entry = (xfs_dir2_sf_entry_t *)((uint8_t *)&sf->list[0] -
				       (!sf->hdr.i8count ? 4 : 0));

    if (file->offset - 1) {
	offset = file->offset;
	while (--offset) {
	    sf_entry = (xfs_dir2_sf_entry_t *)(
				(uint8_t *)sf_entry +
				offsetof(struct xfs_dir2_sf_entry,
					 name[0]) +
				sf_entry->namelen +
				(sf->hdr.i8count ? 8 : 4));
	}
    }

    start_name = &sf_entry->name[0];
    end_name = start_name + sf_entry->namelen;

    name = xfs_dir2_get_entry_name(start_name, end_name);

    ino = xfs_dir2_sf_get_inumber(sf, (xfs_dir2_inou_t *)(
				      (uint8_t *)sf_entry +
				      offsetof(struct xfs_dir2_sf_entry,
					       name[0]) +
				      sf_entry->namelen));

    retval = fill_dirent(fs, dirent, file->offset, ino, (char *)name,
			 end_name - start_name);
    if (retval)
	xfs_error("Failed to fill in dirent structure");

    free(name);

    return retval;
}

int xfs_readdir_dir2_block(struct file *file, struct dirent *dirent,
			   xfs_dinode_t *core)
{
    xfs_bmbt_irec_t r;
    block_t dir_blk;
    struct fs_info *fs = file->fs;
    uint8_t *dirblk_buf;
    uint8_t *p;
    uint32_t offset;
    xfs_dir2_data_hdr_t *hdr;
    xfs_dir2_block_tail_t *btp;
    xfs_dir2_data_unused_t *dup;
    xfs_dir2_data_entry_t *dep;
    uint8_t *start_name;
    uint8_t *end_name;
    char *name;
    xfs_ino_t ino;
    int retval = 0;

    bmbt_irec_get(&r, (xfs_bmbt_rec_t *)&core->di_literal_area[0]);
    dir_blk = fsblock_to_bytes(fs, r.br_startblock) >> BLOCK_SHIFT(fs);

    dirblk_buf = xfs_dir2_get_dirblks(fs, dir_blk, r.br_blockcount);
    hdr = (xfs_dir2_data_hdr_t *)dirblk_buf;
    if (be32_to_cpu(hdr->magic) != XFS_DIR2_BLOCK_MAGIC) {
        xfs_error("Block directory header's magic number does not match!");
        xfs_debug("hdr->magic: 0x%lx", be32_to_cpu(hdr->magic));

	free(dirblk_buf);

	return -1;
    }

    btp = xfs_dir2_block_tail_p(XFS_INFO(fs), hdr);

    if (file->offset + 1 > be32_to_cpu(btp->count))
	return -1;

    file->offset++;

    p = (uint8_t *)(hdr + 1);

    if (file->offset - 1) {
	offset = file->offset;
	while (--offset) {
	    dep = (xfs_dir2_data_entry_t *)p;

	    dup = (xfs_dir2_data_unused_t *)p;
	    if (be16_to_cpu(dup->freetag) == XFS_DIR2_DATA_FREE_TAG) {
		p += be16_to_cpu(dup->length);
		continue;
	    }

	    p += xfs_dir2_data_entsize(dep->namelen);
	}
    }

    dep = (xfs_dir2_data_entry_t *)p;

    start_name = &dep->name[0];
    end_name = start_name + dep->namelen;
    name = xfs_dir2_get_entry_name(start_name, end_name);

    ino = be64_to_cpu(dep->inumber);

    retval = fill_dirent(fs, dirent, file->offset, ino, name,
			 end_name - start_name);
    if (retval)
	xfs_error("Failed to fill in dirent structure");

    free(dirblk_buf);
    free(name);

    return retval;
}

int xfs_readdir_dir2_leaf(struct file *file, struct dirent *dirent,
			  xfs_dinode_t *core)
{
    xfs_bmbt_irec_t irec;
    struct fs_info *fs = file->fs;
    xfs_dir2_leaf_t *leaf;
    block_t leaf_blk, dir_blk;
    xfs_dir2_leaf_entry_t *lep;
    uint32_t newdb;
    uint32_t curdb = -1;
    xfs_dir2_data_entry_t *dep;
    xfs_dir2_data_hdr_t *data_hdr;
    uint8_t *start_name;
    uint8_t *end_name;
    char *name;
    xfs_intino_t ino;
    uint8_t *buf = NULL;
    int retval = 0;

    bmbt_irec_get(&irec, ((xfs_bmbt_rec_t *)&core->di_literal_area[0]) +
					be32_to_cpu(core->di_nextents) - 1);
    leaf_blk = fsblock_to_bytes(fs, irec.br_startblock) >>
					BLOCK_SHIFT(file->fs);

    leaf = (xfs_dir2_leaf_t *)xfs_dir2_get_dirblks(fs, leaf_blk,
						   irec.br_blockcount);
    if (be16_to_cpu(leaf->hdr.info.magic) != XFS_DIR2_LEAF1_MAGIC) {
        xfs_error("Single leaf block header's magic number does not match!");
        goto out;
    }

    if (!leaf->hdr.count)
        goto out;

    if (file->offset + 1 > be16_to_cpu(leaf->hdr.count))
	goto out;

    lep = &leaf->ents[file->offset++];

    /* Skip over stale leaf entries */
    for ( ; be32_to_cpu(lep->address) == XFS_DIR2_NULL_DATAPTR;
	  lep++, file->offset++);

    newdb = xfs_dir2_dataptr_to_db(fs, be32_to_cpu(lep->address));
    if (newdb != curdb) {
	if (buf)
	    free(buf);

	bmbt_irec_get(&irec,
		      ((xfs_bmbt_rec_t *)&core->di_literal_area[0]) + newdb);
	dir_blk = fsblock_to_bytes(fs, irec.br_startblock) >>
						BLOCK_SHIFT(fs);
	buf = xfs_dir2_get_dirblks(fs, dir_blk, irec.br_blockcount);
	data_hdr = (xfs_dir2_data_hdr_t *)buf;
	if (be32_to_cpu(data_hdr->magic) != XFS_DIR2_DATA_MAGIC) {
	    xfs_error("Leaf directory's data magic number does not match!");
	    goto out1;
	}

	curdb = newdb;
    }

    dep = (xfs_dir2_data_entry_t *)(
	(char *)buf + xfs_dir2_dataptr_to_off(fs,
					      be32_to_cpu(lep->address)));

    start_name = &dep->name[0];
    end_name = start_name + dep->namelen;
    name = xfs_dir2_get_entry_name(start_name, end_name);

    ino = be64_to_cpu(dep->inumber);

    retval = fill_dirent(fs, dirent, file->offset, ino, name,
			 end_name - start_name);
    if (retval)
	xfs_error("Failed to fill in dirent structure");

    free(name);
    free(buf);
    free(leaf);

    return retval;

out1:
    free(buf);

out:
    free(leaf);

    return -1;
}

int xfs_readdir_dir2_node(struct file *file, struct dirent *dirent,
			  xfs_dinode_t *core)
{
    (void)file;
    (void)dirent;
    (void)core;

    return -1;
}
