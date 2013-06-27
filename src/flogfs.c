#include "flogfs_private.h"

#include <string.h>

typedef struct {
	flog_read_file_t * read_head;   //!< The head of the list of read files
	flog_write_file_t * write_head; //!< The head of the list of write files
	uint32_t max_file_id;
	uint32_t max_block_sequence;
	uint16_t first_age_table_block;
	uint16_t first_file_table_block;
	flog_state_t state;
	
	flog_timestamp_t t;
	
	flog_block_idx_t inode0;
	flog_file_id_t   num_files;
	flog_block_idx_t num_free_blocks;
	
	flog_block_idx_t current_open_block;
	uint16_t         current_open_page;
	uint_fast8_t     page_open;
	flog_result_t    page_open_result;
	
	fs_lock_t lock; //!< A lock to serialize some FS operations
} flogfs_t;

typedef struct {
	flog_block_idx_t block;
	flog_block_idx_t next_block;
	uint32_t inode_idx;
	uint16_t sector;
} flog_inode_iterator_t;

typedef struct {
	uint16_t block;
	uint16_t seq;
} flog_age_entry_t;

typedef struct {
	uint8_t header[6];
	uint8_t major_vsn;
	uint8_t minor_vsn;
	uint32_t sequence;
} flog_header_t;

typedef struct {
	uint32_t block_seq;
	uint32_t file_id;
	uint32_t file_block_seq;
} flog_file_tag_t;

typedef struct {
	flog_file_tag_t tag;
	char filename[FLOG_MAX_FNAME_LEN];
} flog_file_header_t;

static flogfs_t flogfs;

static uint8_t const fs_header_buffer[12] = {
	0x00, 0x00, 0x00, 0x00,
	0xBE, 0xEF, FLOG_VSN_MAJOR, FLOG_VSN_MINOR,
	0x00, 0x00, 0x00, 0x00
};

#if !FLOG_BUILD_CPP
#ifdef __cplusplus
extern "C" {
#endif
#endif


//! @addtogroup Private
//! @{

static uint16_t flog_allocate_block();

static inline flog_result_t flog_open_page(uint16_t block, uint16_t page);

/*!
 @brief Open the page corresponding to a sector
 @param block The block
 @param sector The sector you wish to access
 */
static inline flog_result_t flog_open_sector(uint16_t block, uint16_t sector);


static inline void flog_close_sector();
//! @}

static inline flog_result_t flog_open_page(uint16_t block, uint16_t page){
	if(flogfs.page_open &&
	   (flogfs.current_open_block == block) &&
	   (flogfs.current_open_page == page)){
		return flogfs.page_open_result;
	}
	flogfs.page_open_result = flash_open_page(block, page);
	flogfs.page_open = 1;
	flogfs.current_open_block = block;
	flogfs.current_open_page = page;
	return flogfs.page_open_result;
}

static inline flog_result_t flog_open_sector(uint16_t block, uint16_t sector){
	return flog_open_page(block, sector / FS_SECTORS_PER_PAGE);
}

static inline void flog_close_sector(){
	flogfs.page_open = 0;
}


static void flog_inode_iterator_init(flog_inode_iterator_t * iter,
                                     flog_block_idx_t inode0){
	iter->block = inode0;
	flog_open_page(inode0, 0);
	flash_read_sector((uint8_t *)&iter->next_block, FLOG_INODE_TAIL_SECTOR, 0,
	                  sizeof(flog_block_idx_t));
	iter->inode_idx = 0;
	iter->sector = FS_SECTORS_PER_PAGE;
}

static void flog_inode_iterator_next(flog_inode_iterator_t * iter){
	iter->sector += 2;
	iter->inode_idx += 1;
	if(iter->sector >= FS_PAGES_PER_BLOCK * FS_SECTORS_PER_PAGE){
		iter->block = iter->next_block;
		flog_open_page(iter->block, 0);
		flash_read_sector((uint8_t *)&iter->next_block, FLOG_INODE_TAIL_SECTOR, 0,
		                  sizeof(flog_block_idx_t));
		iter->sector = FS_SECTORS_PER_PAGE;
	}
}


flog_result_t flogfs_init(){
	flogfs.state = FLOG_STATE_RESET;
	flogfs.max_file_id = 0;
	flogfs.max_block_sequence = 0;
	flogfs.page_open = 0;
		
	flogfs.max_file_id = 0;
	return flash_init();
}

flog_result_t flogfs_format(){
	uint32_t i;
	
	union {
		flog_inode_sector0_t main_buffer;
		flog_inode_sector0_spare_t spare_buffer;
	};
	
	fs_lock(&flogfs.lock);
	
	for(i = 0; i < FS_NUM_BLOCKS; i++){
		flash_open_page(i, 0);
		if(!flash_block_is_bad()){
			if(FLOG_FAILURE == flash_erase_block(i)){
				fs_unlock(&flogfs.lock);
				flash_unlock();
				return FLOG_FAILURE;
			}
		}
	}
	
	// Write the first file table
	flash_open_page(0, 0);
	main_buffer.age = 0;
	main_buffer.timestamp = 0;
	flash_write_sector((const uint8_t *)&main_buffer,
	                   0, 0, sizeof(main_buffer));
	spare_buffer.inode_index = 0;
	spare_buffer.type_id = FLOG_BLOCK_TYPE_INODE;
	flash_write_spare((const uint8_t *)&spare_buffer, 0);
	flash_commit();
	flash_close_page();
	
	fs_unlock(&flogfs.lock);
	
	flash_unlock();
	return FLOG_SUCCESS;
}

flog_result_t flogfs_mount(){
	uint8_t buffer[32];
	uint32_t i, j, done_scanning;
	uint16_t block, page, sector;
	
	////////////////////////////////////////////////////////////
	// Data structures
	////////////////////////////////////////////////////////////
	
	// Use in search for highest allocation timestamp
	struct {
		flog_block_idx_t block;
		flog_block_age_t age;
		flog_file_id_t file_id;
		flog_timestamp_t timestamp;
	} last_allocation;
	
	struct {
		flog_block_idx_t first_block, last_block;
		flog_file_id_t   file_id;
		flog_timestamp_t timestamp;
	} last_deletion;
	
	// Count how many free blocks are encountered
	flog_block_idx_t num_free_blocks;
	
	// Find the freshest block to allocate. Why not?
	struct {
		flog_block_idx_t block;
		flog_block_age_t age;
	} min_age_block;
	
	flog_block_idx_t inode0_idx;
	
	// Find the maximum block age
	flog_block_age_t max_block_age;
	
	flog_inode_iterator_t inode_iter;

	////////////////////////////////////////////////////////////
	// Flexible buffers for flash reads
	////////////////////////////////////////////////////////////
	
	flog_timestamp_t timestamp_buffer;
	
	union {
		uint8_t sector0_buffer;
		flog_file_sector0_header_t file_sector0_header;
		flog_inode_sector0_t inode_sector0;
		flog_inode_file_invalidation_t inode_file_invalidation_sector;
	};
	
	union {
		uint8_t sector_buffer;
		flog_file_tail_sector_header_t file_tail_sector_header;
		flog_inode_tail_sector_t inode_tail_sector;
		flog_inode_file_allocation_header_t inode_file_allocation_sector;
		flog_file_invalidation_sector_t file_invalidation_sector;
	};
	
	union {
		uint8_t spare_buffer;
		flog_inode_sector0_spare_t inode_spare0;
		flog_file_sector_spare_t file_spare0;
	};
	
	
	////////////////////////////////////////////////////////////
	// Initialize data structures
	////////////////////////////////////////////////////////////
	
	last_allocation.block = FLOG_BLOCK_IDX_INVALID;
	last_allocation.timestamp = 0;
	
	last_deletion.timestamp = 0;
	last_deletion.file_id = FLOG_FILE_ID_INVALID;
	
	num_free_blocks = 0;
	
	min_age_block.age = 0xFFFFFFFF;
	min_age_block.block = FLOG_BLOCK_IDX_INVALID;
	
	inode0_idx = FLOG_BLOCK_IDX_INVALID;
	
	max_block_age = 0;
	
	////////////////////////////////////////////////////////////
	// Claim the disk and get this show started
	////////////////////////////////////////////////////////////
	
	fs_lock(&flogfs.lock);
	flash_lock();
	
	////////////////////////////////////////////////////////////
	// First, iterate through all blocks to find:
	// - Most recent allocation time in a file block
	// - Number of free blocks
	// - Some free blocks that are fair to use
	// - Oldest block age
	// - Inode table 0
	////////////////////////////////////////////////////////////
	for(i = 0; i < FS_NUM_BLOCKS; i++){
		// Everything can be determined from page 0
		if(!flash_open_page(i, 0)){
			continue;
		}
		if(flash_block_is_bad()){
			continue;
		}
		// Read the sector 0 spare to identify valid blocks
		flash_read_spare((uint8_t *)&spare_buffer, 0);

		switch(inode_spare0.type_id) {
		case FLOG_BLOCK_TYPE_INODE:
			// Check for an invalidation timestamp
			flash_read_sector((uint8_t *)&timestamp_buffer,
			                  FLOG_INODE_INVALIDATION_SECTOR, 0,
			                  sizeof(flog_timestamp_t));
			flash_read_sector(&sector0_buffer, 0, 0,
			                  sizeof(flog_inode_sector0_t));
			if(timestamp_buffer == 0xFFFFFFFF){
				// This thing is still valid
				if(inode_spare0.inode_index == 0){
					// Found the original gangster!
					inode0_idx = i;
				} else {
					// Not the first, but valid!
				}
			} else {
				// YOU FOUND AN INVALIDATED INODE
				// Deal with it...
				// Count it as free?
			}
			// Check if this is a really old block
			if(inode_sector0.age > max_block_age){
				max_block_age = inode_sector0.age;
			}
			break;
		case FLOG_BLOCK_TYPE_FILE:
			flash_read_sector(&sector_buffer,
			                  FLOG_FILE_TAIL_SECTOR, 0,
			                  sizeof(flog_file_tail_sector_header_t));
			flash_read_sector(&sector0_buffer, 0, 0, sizeof(flog_file_sector0_header_t));
			if(file_tail_sector_header.timestamp == 0xFFFFFFFF){
				// This is the last allocated block for whatever that file is
				// That is pointless
			} else if(file_tail_sector_header.timestamp >
			          last_allocation.timestamp){
				// This is now the most recent allocation timestamp!
				last_allocation.timestamp = file_tail_sector_header.timestamp;
				last_allocation.block = file_tail_sector_header.next_block;
				last_allocation.age = file_tail_sector_header.next_age;
				last_allocation.file_id = file_sector0_header.file_id;
			}
			// Check if this block is really old
			if(file_sector0_header.age > max_block_age){
				max_block_age = file_sector0_header.age;
			}
			break;
		case FLOG_BLOCK_TYPE_UNALLOCATED:
			num_free_blocks += 1;
			break;
		}
	}
	
	if(inode0_idx == FLOG_BLOCK_IDX_INVALID){
		flash_debug_error("Inode 0 not found!");
		goto failure;
	}
	
	////////////////////////////////////////////////////////////
	// Now iterate through the inode chain, finding:
	// - Most recent file deletion
	// - Most recent file allocation
	// - Max file ID
	////////////////////////////////////////////////////////////
	
	done_scanning = 0;
	block = inode0_idx; // Inode block
	for(flog_inode_iterator_init(&inode_iter, inode0_idx);;
		flog_inode_iterator_next(&inode_iter)){
		flog_open_sector(inode_iter.block, inode_iter.sector);
		flash_read_sector(&sector_buffer, inode_iter.sector, 0,
		                  sizeof(flog_inode_file_allocation_header_t));
		if(inode_file_allocation_sector.file_id == FLOG_FILE_ID_INVALID){
			// Passed the last file
			break;
		}
		flog_open_sector(inode_iter.block, inode_iter.sector + 1);
		flash_read_sector(&sector0_buffer, inode_iter.sector + 1, 0,
		                  sizeof(flog_inode_file_invalidation_t));
		
		// Keep track of the maximum file ID
		// Since these are allocated sequentially, this has to be the latest
		// ... so far ...
		flogfs.max_file_id = inode_file_allocation_sector.file_id;
		
		// Was it deleted?
		if(inode_file_invalidation_sector.timestamp ==
			FLOG_TIMESTAMP_INVALID){
			// This is still valid
			
			// Check if this is now the most recent allocation
			if(inode_file_allocation_sector.timestamp >
			   last_allocation.timestamp){
				// This isn't really always true becase we also consider
				// allocations in the file chain itself, which are not
				// reflected
				last_allocation.block =
				  inode_file_allocation_sector.first_block;
				last_allocation.file_id =
				  inode_file_allocation_sector.file_id;
				last_allocation.age =
				  inode_file_allocation_sector.first_block_age;
				last_allocation.timestamp =
				  inode_file_allocation_sector.timestamp;
			}
		} else {
			// Check if this was the most recent deletion
			if(inode_file_invalidation_sector.timestamp >
			   last_deletion.timestamp){
				last_deletion.first_block =
				  inode_file_allocation_sector.first_block;
				last_deletion.last_block =
				  inode_file_invalidation_sector.last_block;
				last_deletion.file_id =
				  inode_file_allocation_sector.file_id;
				last_deletion.timestamp =
				  inode_file_invalidation_sector.timestamp;
			}
		}
	}
	
	// Go check and (maybe) clean the last allocation
	if(last_allocation.timestamp > 0){
		flog_open_sector(last_allocation.block, 0);
		flash_read_sector(&sector0_buffer, 0, 0,
		                  sizeof(flog_file_sector0_header_t));
		if(file_sector0_header.file_id != last_allocation.file_id){
			// This block never got allocated
			// Erase and initialize it!
			flash_erase_block(last_allocation.block);
			flog_open_page(last_allocation.block, 0);
			file_sector0_header.age = last_allocation.age;
			file_sector0_header.file_id = last_allocation.file_id;
			flash_write_sector(&sector0_buffer, 0, 0,
			                   sizeof(flog_file_sector0_header_t));
			file_spare0.nbytes = 0;
			file_spare0.nothing = 0;
			file_spare0.type_id = FLOG_BLOCK_TYPE_FILE;
			flash_write_spare(&spare_buffer, 0);
			flash_commit();
			
			flogfs.t = last_allocation.timestamp + 1;
		}
	}
	
	// Verify the completion of the most recent deletion operation
	if(last_deletion.timestamp > 0){
		flog_open_sector(last_deletion.last_block, 0);
		flash_read_sector(&sector0_buffer, 0, 0, sizeof(flog_file_header_t));
		if(file_sector0_header.file_id == last_deletion.file_id){
			// This is the same file still, see if it's been invalidated
			flog_open_sector(last_deletion.last_block,
			                 FLOG_FILE_INVALIDATION_SECTOR);
			flash_read_sector(&sector_buffer, 0, 0,
			                  sizeof(flog_file_invalidation_sector_t));
			if(file_invalidation_sector.timestamp != FLOG_TIMESTAMP_INVALID){
				// Crap, this never got invalidated correctly
				// TODO: Actually go and invalidate the chain now
				flash_debug_error("Found a file that wasn't completely deleted");
				goto failure;
			}
		}
	}
	
	flash_unlock();
	fs_unlock(&flogfs.lock);
	return FLOG_SUCCESS;

failure:
	flash_unlock();
	fs_unlock(&flogfs.lock);
	return FLOG_FAILURE;
}

flog_result_t flogfs_open_read(flog_read_file_t * file, char const * filename){
	flog_inode_iterator_t inode_iter;
	
	union {
		uint8_t sector_buffer;;
		flog_inode_file_allocation_t inode_file_allocation_sector;
		flog_inode_file_invalidation_t inode_file_invalidation_sector;
	};
	union {
		uint8_t spare_buffer;
		flog_file_sector_spare_t file_sector_spare;
	};
	
	if(strlen(filename) >= FLOG_MAX_FNAME_LEN){
		return FLOG_FAILURE;
	}
	
	fs_lock(&flogfs.lock);
	for(flog_inode_iterator_init(&inode_iter, flogfs.inode0);;
		flog_inode_iterator_next(&inode_iter)){
		
		// Check if the entry is valid
		flog_open_sector(inode_iter.block, inode_iter.sector);
		flash_read_sector(&sector_buffer, inode_iter.sector, 0,
		                  sizeof(flog_inode_file_allocation_t));
		
		if(inode_file_allocation_sector.header.file_id ==
		   FLOG_FILE_ID_INVALID){
			// This file is the end.
			// Do a quick check to make sure there are no foolish errors
			if(inode_iter.next_block != FLOG_BLOCK_IDX_INVALID){
				flash_debug_warn("Found fake\ninode end");
			}
			goto failure;
		}
		
		// Check if the name matches
		if(strncmp(filename, inode_file_allocation_sector.filename,
		   FLOG_MAX_FNAME_LEN) != 0){
			continue;
		}
		
		// Now check if it's been deleted
		flog_open_sector(inode_iter.block, inode_iter.sector+1);
		flash_read_sector(&sector_buffer, inode_iter.sector+1, 0,
		                  sizeof(flog_timestamp_t));
		
		if(inode_file_invalidation_sector.timestamp != FLOG_TIMESTAMP_INVALID){
			// This one is invalid
			continue;
		}
		
		
		// Now go find the start of file data (either first or second sector)
		// and adjust some settings
		file->id = inode_file_allocation_sector.header.file_id;
		file->block = inode_file_allocation_sector.header.first_block;
		flog_open_sector(file->block, 0);
		flash_read_spare(&spare_buffer, 0);
		if(file_sector_spare.nbytes != 0){
			// The first sector has some stuff in it!
			file->sector = 0;
			file->offset = sizeof(flog_file_sector0_header_t);
		} else {
			flog_open_sector(file->block, 1);
			flash_read_spare(&spare_buffer, 1);
			file->sector = 1;
			file->offset = 0;
		}
		
		file->nbytes_in_sector = file_sector_spare.nbytes;
		
		// If we got this far...
		break;
	}
	
	// TODO: Add item to 'open file' list
	
failure:
	flash_unlock();
	fs_unlock(&flogfs.lock);
	return FLOG_FAILURE;
}



flog_result_t flogfs_open_write(flog_write_file_t * file, char const * filename){

}

static uint32_t flog_get_block_age(uint16_t block){

}

/*!
 @brief Search for a free (completely unallocated) block
 
 @return The ID of the free block, FS_NUM_BLOCKS if none
 */
static uint16_t flog_find_free_block(){

}

/*!
 @brief Allocate an available block
 
 1. Check list of completely unused blocks
	These are blocks of minimum age. Use them first
 2. Iterate through block age table. The block of minimum age should be used
 */
static uint16_t flog_allocate_block() {

}

static uint16_t flog_pick_start_point() {
	return 0;
}



#if !FLOG_BUILD_CPP
#ifdef __cplusplus
};
#endif
#endif