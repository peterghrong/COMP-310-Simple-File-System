/* standard libraries */
#include "sfs_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "disk_emu.h"

/* Fixed variable declaration */
#define MAX_FNAME_LENGTH 32       // filename length, limit to 32 for the testers
#define BLOCK_SIZE 1024           // block size
#define TOTAL_NUM_OF_BLOCKS 1024  // total number of blocks
#define MAX_INODES 128            // Restricting the number of i-nodes to 128
#define DEFAULT_DISK_NAME "Disk"  // Default disk name
#define SUPER_BLOCK_LOCATION 0  // location of the super block
#define ROOT_DIR_INODE_LOCATION 1  // location of the root directory
#define INODE_TABLE_LOCATION 1  // starting location of inode table
#define INODE_TABLE_SIZE 9      // starting location of data blocks
#define INODE_BITMAP_LOCATION 10  // location of the bitmap for i-nodes
#define INODE_BITMAP_SIZE 4       // number of blocks needed to bitmap
#define DATA_BLOCK_BITMAP_LOCATION 14  // starting location of data block table
#define DATA_BLOCK_BITMAP_SIZE 4       // size of data block bit map
#define DIRECTORY_TABLE_LOCATION 18 // directory table starting location
#define DIRECTORY_TABLE_SIZE 5 // size of the directory table
#define PRE_DEFINED_BLOCKS 23 // total number of predefined blocks

// structure for superblock according to manual
typedef struct super_block {
    int magic;
    int block_size;
    int file_system_size;
    int inode_table_length;
    int root_directory;
} SUPER_BLOCK;

// structure for each inode according to manual
// Each i-node is of size 12*4 + 6*4 = 72 bytes
// for the assumption that we have 124 inodes
// we need 9 blocks for all inodes
typedef struct inode {
    int mode;  // note: 1 -> file, 0 -> directory
    int link_cnt;
    int uid;
    int gid;
    int size;
    int pointers[12];
    int indirect_pointer;
} INODE;

// directory entry structure, 32+4 bytes
typedef struct directory_entry {
    char full_filename[MAX_FNAME_LENGTH];
    int inode_pointer;
} DIRECTORY_ENTRY;

// In memory data structure of the open file descriptor table
typedef struct open_file_descriptor {
    int inode_pointer;
    int read_pointer;
    int write_pointer;
} OPEN_FILE_DESCRIPTOR;

/* dynamic variable declaration */
INODE inode_table[MAX_INODES];
DIRECTORY_ENTRY directory_table[MAX_INODES];                  // directory table keeps copies of directories in memory
OPEN_FILE_DESCRIPTOR open_file_descriptor_table[MAX_INODES];  // open file descriptor table to keep track of inodes
SUPER_BLOCK super_block;
int current_directory = 1;

/* bitmaps */
// bitmap 1-> occupied, 0-> free
int inode_bitmap[MAX_INODES];  // 1024 blocks possible, each entry is 4 bytes, so takes 4 blocks to store inodes
int data_block_bitmap[TOTAL_NUM_OF_BLOCKS];

// min helper function to find the min of 2 integers
int min(int x, int y) {
    if (x > y) {
        return y;
    }
    return x;
}

// max helper function to find the max of 2 integers
int max(int x, int y) {
    if (x > y) {
        return x;
    }
    return y;
}

// set a bit of bitmap to 1
void set_bit_1(char* mode, int loc) {
    if (strcmp(mode, "inode") == 0){
        inode_bitmap[loc] = 1;
    }
    else if (strcmp(mode, "data") == 0){
        data_block_bitmap[loc] = 1;
    }
    else{
        fprintf(stderr, "Wrong input mode\n ");
    }
}

// set a bit of bitmap to 0
void set_bit_0(char* mode, int loc){
    if (strcmp(mode, "inode") == 0){
        inode_bitmap[loc] = 0;
    }
    else if (strcmp(mode, "data") == 0){
        data_block_bitmap[loc] = 0;
    }
    else{
        fprintf(stderr, "Wrong input mode\n ");
    }

}

/* bitmap -> free bit if there is a freebit, -> -1 if there is none */
int find_free_bit(char *mode) {
    if (strcmp(mode, "inode") == 0){ 
        for (int i = 0; i < MAX_INODES; i++) {
            if (inode_bitmap[i] == 0) {
                return i;
            }
        }
    } else if (strcmp(mode, "data") == 0){
        for (int i = 0; i < TOTAL_NUM_OF_BLOCKS; i++) {
            if (data_block_bitmap[i] == 0) {
                return i;
            }
        }
    }
    fprintf(stderr, "Wrong input mode \n");
    return -1;
}

/* init fresh base blocks */
void init_fresh_base_blocks() {
    // instantiate a single super block
    SUPER_BLOCK super_block;
    super_block.block_size = BLOCK_SIZE;
    super_block.file_system_size = TOTAL_NUM_OF_BLOCKS * BLOCK_SIZE;
    super_block.inode_table_length = MAX_INODES;
    super_block.root_directory = ROOT_DIR_INODE_LOCATION;
    write_blocks(SUPER_BLOCK_LOCATION, 1, &super_block);

    // instantiate bitmap for inodes
    for (int i = 0; i < MAX_INODES; i++) {
        inode_bitmap[i] = 0;
    }
    // instantiate a single root directory block
    INODE root_directory;
    root_directory.mode = 0;
    root_directory.gid = 0;
    root_directory.uid = 0;
    root_directory.link_cnt = 0;
    root_directory.size = 0;
    for (int i = 0; i < 12; i++) {
        root_directory.pointers[i] = 0;
    }
    root_directory.indirect_pointer = 0;
    inode_table[0] = root_directory;
    write_blocks(INODE_TABLE_LOCATION, INODE_TABLE_SIZE, &inode_table);
    set_bit_1("inode", 1);  // flip bit for root directory
    write_blocks(INODE_BITMAP_LOCATION, INODE_BITMAP_SIZE, &inode_bitmap);

    // initialise and flip 23 blocks for data_block_bit_map since all 23 blocks are presumably occupied
    for (int i = 0; i < TOTAL_NUM_OF_BLOCKS; i++) {
        data_block_bitmap[i] = 0;
    }
    for (int i = 0; i < PRE_DEFINED_BLOCKS; i++) {
        set_bit_1("data", i);
    }
    write_blocks(DATA_BLOCK_BITMAP_LOCATION, DATA_BLOCK_BITMAP_SIZE, &data_block_bitmap);

    // instantiate directory table;
    strcpy(directory_table[0].full_filename, "root");
    directory_table[0].inode_pointer = -1;
    write_blocks(DIRECTORY_TABLE_LOCATION, DIRECTORY_TABLE_SIZE, &directory_table);
}

/* init old base blocks */
// we will load them all from disk
void init_old_base_blocks() {
    // inode bitmap
    read_blocks(INODE_BITMAP_LOCATION, INODE_BITMAP_SIZE, &inode_bitmap);
    // directory table
    read_blocks(DIRECTORY_TABLE_LOCATION, DIRECTORY_TABLE_SIZE, &directory_table);
    // inode table
    read_blocks(INODE_TABLE_LOCATION, INODE_TABLE_SIZE, &inode_table);
    // bitmap table
    read_blocks(DATA_BLOCK_BITMAP_LOCATION, DATA_BLOCK_BITMAP_SIZE, &data_block_bitmap);

}

/* mksfs */
// fresh == 1 -> start a fresh disk
// fresh == 0 -> load from disk
void mksfs(int fresh) {
    // before running we dump everything in the memory so there is no garbage
    memset(data_block_bitmap, '\0', sizeof(data_block_bitmap));
    memset(inode_bitmap, '\0', sizeof(inode_bitmap));
    memset(inode_table, '\0', sizeof(inode_table));
    memset(directory_table, '\0', sizeof(directory_table));
    // fresh flag == 1
    if (fresh == 1) {  
        // init disk
        int ret = init_fresh_disk(DEFAULT_DISK_NAME, BLOCK_SIZE, TOTAL_NUM_OF_BLOCKS);
        if (ret == -1) {
            fprintf(stderr, "File System Creation Failure. \n");
            exit(0);
        }
        init_fresh_base_blocks();
    }
    // fresh flag == 0
    else{
        int ret = init_disk(DEFAULT_DISK_NAME, BLOCK_SIZE, TOTAL_NUM_OF_BLOCKS);
        if (ret == -1) {
            fprintf(stderr, "File System Recreation Failure. \n");
        }
        init_old_base_blocks();
    }
}

/* returns the name of the next file in directory into fname*/
// works as a circular array, if there are no more next files,
// return to the first file in directory
int sfs_getnextfilename(char* fname) {
    while (current_directory != MAX_INODES){
        if (directory_table[current_directory].inode_pointer != 0 && strncmp(directory_table[current_directory].full_filename, "root", MAX_FNAME_LENGTH) != 0 ) {
            strcpy(fname, directory_table[current_directory].full_filename);
            current_directory++;
            return 1;
        }
        current_directory++;
    }
    if (current_directory == MAX_INODES) {
            current_directory = 1;
            return 0;
    }
    return 0;
}

/* sfs_getfilesize */
// get the file size referred to by the path name
int sfs_getfilesize(const char* path) {
    for (int i = 0; i < MAX_INODES; i++) {
        if (strncmp(path, directory_table[i].full_filename, 32) == 0) {
            int ptr = directory_table[i].inode_pointer;
            int size = inode_table[ptr].size;
            return size;
        }
    }
    return 0;
}

// helper function to fine free entry in the following tables
// 1. Open file descriptor table
// 2. Directory table
// 3. I-node table
int find_free_entry(char* mode) {
    if (strcmp("open_file_descriptor_table", mode) == 0) {
        for (int i = 0; i < MAX_INODES; i++) {
            OPEN_FILE_DESCRIPTOR descriptor = open_file_descriptor_table[i];
            if (descriptor.inode_pointer == 0) {
                return i;
            }
        }
        fprintf(stderr, "open_file_descriptor_table full. \n");
        return -1;
    } else if (strcmp("directory_table", mode) == 0) {
        for (int i = 0; i < MAX_INODES; i++) {
            DIRECTORY_ENTRY entry = directory_table[i];
            if (strcmp(entry.full_filename, "") == 0) {
                return i;
            }
        }
        fprintf(stderr, "directory_table full. \n");
        return -1;
    } else if (strcmp("inode_table", mode) == 0) {
        for (int i = 1; i < MAX_INODES; i++) {
            INODE inode = inode_table[i];
            if (inode.mode == 0) {
                return i;
            }
        }
        fprintf(stderr, "inode_table is full. \n");
        return -1;
    }
    fprintf(stderr, "Wrong input mode. \n");
    return -1;
}

/* sfs_fopen */
// scenarios:
// 1. file does not exist on disk -> we create file -> add to table
// 2. file exists but not open -> open file, add to table
// 3. file exists and opened -> return index
// note: we maintain a 1 to 1 correspondence between the
// indices of I-node table and directory_entry table
int sfs_fopen(char* name) {
    if (strlen(name) > MAX_FNAME_LENGTH){
        fprintf(stderr, "File name is too long\n");
        return -1;
    }
    // Iterate directory table
    for (int i = 1; i < MAX_INODES; i++) {
        DIRECTORY_ENTRY entry = directory_table[i];
        // found the file in the directory table
        if (strncmp(entry.full_filename, name, MAX_FNAME_LENGTH) == 0) {
            int inode_index = entry.inode_pointer;
            OPEN_FILE_DESCRIPTOR file_descriptor = open_file_descriptor_table[i];
            // case 3
            if (file_descriptor.inode_pointer != 0) {
                return i;
            } else {
                // case 2 file is not open yet
                OPEN_FILE_DESCRIPTOR insert_descriptor;
                insert_descriptor.inode_pointer = inode_index;
                insert_descriptor.read_pointer = 0;
                // last byte is where we should continue writing
                insert_descriptor.write_pointer = inode_table[inode_index].size;
                open_file_descriptor_table[i] = insert_descriptor;
                return i;
            }
        }
    }
    // case 1
    // find free inode
    int free_dir_loc = find_free_entry("directory_table");
    int free_inode_loc = find_free_entry("inode_table");
    if (free_dir_loc == -1 || free_inode_loc == -1) {
        return -1;
    }
    INODE inode;
    inode.gid = 0;
    inode.indirect_pointer = 0;
    inode.link_cnt = 0;
    inode.mode = 1;
    // by default set to an array of 0s
    for (int i = 0; i < 12; i++) {
        inode.pointers[i] = 0;
    }
    inode.indirect_pointer = 0;
    inode.size = 0;
    inode.uid = 0;
    inode_table[free_inode_loc] = inode;
    // create entry in the directory table
    strcpy(directory_table[free_dir_loc].full_filename, name);
    directory_table[free_dir_loc].inode_pointer = free_inode_loc;

    open_file_descriptor_table[free_dir_loc].inode_pointer = free_inode_loc;
    open_file_descriptor_table[free_dir_loc].read_pointer = 0;
    open_file_descriptor_table[free_dir_loc].write_pointer = inode_table[free_inode_loc].size;
    // occupy a bit on the bitmap
    set_bit_1("inode", free_inode_loc);
    // write inode into disk
    write_blocks(INODE_TABLE_LOCATION, INODE_TABLE_SIZE, &inode_table);
    // write directory table into disk
    write_blocks(DIRECTORY_TABLE_LOCATION, DIRECTORY_TABLE_SIZE, &directory_table);

    return free_dir_loc;
}

/* sfs_fclose */
// closes a file -> remove entry from FDT
// success -> return 0, fail -> return -1
int sfs_fclose(int fileID) {
    if (fileID >= 0 && fileID < MAX_INODES) {
        OPEN_FILE_DESCRIPTOR descriptor = open_file_descriptor_table[fileID];
        if (descriptor.inode_pointer == 0) {
            fprintf(stderr,"File is not open. \n");
            return -1;
        }
        OPEN_FILE_DESCRIPTOR fresh_descriptor;
        fresh_descriptor.inode_pointer = 0;
        fresh_descriptor.read_pointer = 0;
        fresh_descriptor.write_pointer = 0;
        open_file_descriptor_table[fileID] = fresh_descriptor;
        return 0;
    }
    fprintf(stderr, "fileID index out of bound. \n");
    return -1;
}

/* helper function to write buffer to a block */
// block_pointer: Index of the block to write to
// buffer: buffer to write from
// length: remaining bytes need to be written
// offset: offset of bytes from the beginning of the block to write to
// returns the number of bytes wrote
int write_to_block(int block_pointer, char* buffer, int length, int offset) {
    // adjust read location for offsets
    char *string_buf = malloc(BLOCK_SIZE);
    memset(string_buf, '\0', BLOCK_SIZE);
    int bytes_wrote;

    // if we are writing somewhere in the middle of the block
    if (offset != 0) {
        int max_write = min(BLOCK_SIZE - offset, length);
        read_blocks(block_pointer, 1, string_buf);
        memcpy(string_buf + offset, buffer, max_write);
        bytes_wrote = max_write;
    } 
    // else we are writing in the middle of the block 
    else {
        int max_write = min(BLOCK_SIZE, length);
        memcpy(string_buf, buffer, max_write);
        bytes_wrote = max_write;
    }
    if(write_blocks(block_pointer, 1, string_buf) < 0){
        fprintf(stderr,"Writing failed\n");
    };
    return bytes_wrote;
}

/* sfs_fwrite */
// fileID: index of the file to write to in the open file descriptor table
// buf: buffer to write to the file
// length: total length of data we need to write
int sfs_fwrite(int fileID, const char* buf, int length) {
    // some error handling
    OPEN_FILE_DESCRIPTOR descriptor = open_file_descriptor_table[fileID];
    if (descriptor.inode_pointer == 0) {
        fprintf(stderr, "File is not open. \n");
        return -1;
    }

    // set up
    int remaining = length;
    int bytes_wrote = 0;
    int write_ptr_loc = descriptor.write_pointer;
    char* buffer = (char*)buf;
    int indirect_pointer = inode_table[descriptor.inode_pointer].indirect_pointer;
    int indirect_buffer[BLOCK_SIZE / sizeof(int)];

    // if the indirect pointer is already being used, we load the datablock into buffer
    if (indirect_pointer != 0) {
        read_blocks(indirect_pointer, 1, &indirect_buffer);
    }

    // while there are things to write
    while (remaining > 0) {
        // meaning we are writing to direct blocks still;
        if (write_ptr_loc < (12 * BLOCK_SIZE)) {
            // find location of the block to write in the pointers
            int block_pointer_index = write_ptr_loc / BLOCK_SIZE;
            // find the offset we write from in the block pointed by block pointer index
            int offset = write_ptr_loc % BLOCK_SIZE;
            int inode = descriptor.inode_pointer;
            int block_pointer = inode_table[inode].pointers[block_pointer_index];

            // if we are writing to a new block
            if (block_pointer == 0) {
                block_pointer = find_free_bit("data");
                if (block_pointer == -1) {
                    fprintf(stderr, "Disk is full, cannot write anymore. \n");
                    return -1;
                }
                set_bit_1("data", block_pointer);
                // add to the inodes
                inode_table[inode].pointers[block_pointer_index] = block_pointer;
            }
            // write to block
            int bytes = write_to_block(block_pointer, buffer, remaining, offset);
            // add bytes
            bytes_wrote += bytes;
            // reduce from remaining.
            remaining -= bytes;
            // increase write ptr
            write_ptr_loc += bytes;
            // move buffer
            buffer += bytes;
            // increase inode size if we are writing at the end of the file
            inode_table[inode].size = max(write_ptr_loc, inode_table[inode].size);
        }
        // else we are going to operate with the indirect pointers
        else {
            // set up
            int block_pointer;
            int offset;
            // there is no indirect pointer present, so we have to set up indirect pointer
            if (indirect_pointer == 0) {
                int new_indirect = find_free_bit("data");
                if (new_indirect == -1) {
                    fprintf(stderr,"Disk is full. \n");
                    return -1;
                }
                set_bit_1("data", new_indirect);
                inode_table[descriptor.inode_pointer].indirect_pointer = new_indirect;
                indirect_pointer = new_indirect;  // didnt write the pointer to the indirect block

                // instantiate a new datablock
                block_pointer = find_free_bit("data");
                if (block_pointer == -1) {
                    fprintf(stderr, "Disk if full. \n");
                    return -1;
                }
                set_bit_1("data", block_pointer);

                // initialise the indirect buffer
                for (int i = 0; i < (BLOCK_SIZE / sizeof(int)); i++) {
                    indirect_buffer[i] = 0;
                }
                indirect_buffer[0] = block_pointer;
            } 
            // there exist an indirect pointer already we will just keep writing to it
            else {            

                // find which block we want to operate on
                int block_index = ((write_ptr_loc / BLOCK_SIZE) - 12);
                if (block_index > (BLOCK_SIZE / sizeof(int))) {
                    fprintf(stderr, "Error: Maximum file size reached.\n");
                    return -1;
                }
                block_pointer = indirect_buffer[block_index];

                // if we arrive at a new block which is not occupied
                if (block_pointer == 0) {
                    block_pointer = find_free_bit("data");
                    if (block_pointer == -1) {
                        fprintf(stderr, "Disk if full. cannot write anymore\n");
                        return -1;
                    }
                    set_bit_1("data", block_pointer);

                    // insert new block pointer in the indirect buffer
                    for (int i = 0; i < (BLOCK_SIZE / sizeof(int)); i++) {
                        if (indirect_buffer[i] == 0) {
                            indirect_buffer[i] = block_pointer;
                            break;
                        }
                    }
                }
            }

            // now we actually write to the block
            offset = write_ptr_loc % BLOCK_SIZE;
            int bytes = write_to_block(block_pointer, buffer, remaining, offset);

            bytes_wrote += bytes;
            // reduce from remaining.
            remaining -= bytes;
            // incr write ptr
            write_ptr_loc += bytes;
            // move buffer
            buffer += bytes;
            // increase inode size
            inode_table[descriptor.inode_pointer].size = max(write_ptr_loc, inode_table[descriptor.inode_pointer].size);
        }
        // update open file descriptor table
        open_file_descriptor_table[fileID].write_pointer = write_ptr_loc;
        // and we are done, now flush all to memory
        write_blocks(DATA_BLOCK_BITMAP_LOCATION, DATA_BLOCK_BITMAP_SIZE, &data_block_bitmap);
        // if we used indirect_pointer, write to disk
        if (indirect_pointer != 0) {
            write_blocks(indirect_pointer, 1, &indirect_buffer);
        }
        // we updated inodes
        write_blocks(INODE_TABLE_LOCATION, INODE_TABLE_SIZE, &inode_table);
    }
    return bytes_wrote;
}

// helper function to actually read from the block
// block_pointer: index of the block to read from
// buffer: read data into the buffer
// length: number of bytes remaining to read
// offset: offset in the datablock to start reading from
int read_from_block(int block_pointer, char* buffer, int length, int offset) {
    // adjust read location for offsets
    char* string_buf= malloc(BLOCK_SIZE);
    memset(string_buf, '\0', BLOCK_SIZE);
    int bytes_read;

    if (offset != 0) {
        int max_read = min(BLOCK_SIZE - offset, length);
        read_blocks(block_pointer, 1, string_buf);
        memcpy(buffer, string_buf + offset, max_read);
        bytes_read = max_read;
    } else {
        int max_read = min(BLOCK_SIZE, length);
        read_blocks(block_pointer, 1, string_buf);
        memcpy(buffer, string_buf, max_read);
        bytes_read = max_read;
    }
    return bytes_read;
}

/* sfs_fread */
// fileID: index of the file to read from the open file descriptor table
// buf: buffer to load data into
// length: total bytes to read
int sfs_fread(int fileID, char* buf, int length) {
    // some error handling
    OPEN_FILE_DESCRIPTOR descriptor = open_file_descriptor_table[fileID];
    if (descriptor.inode_pointer == 0) {
        fprintf(stderr, "File is not open. \n");
        return -1;
    }

    // setup
    int remaining = length;
    int read_ptr_loc = descriptor.read_pointer;
    
    // if we are reading past the total size of the file
    // read till the end of the file only
    if (length+read_ptr_loc > inode_table[descriptor.inode_pointer].size) {
        remaining = inode_table[descriptor.inode_pointer].size - descriptor.read_pointer;
    }
    int bytes_read = 0;

    // setup indirect pointer if we need to read the indirect pointers
    int indirect_pointer = inode_table[descriptor.inode_pointer].indirect_pointer;
    int indirect_buffer[BLOCK_SIZE / sizeof(int)];
    if (indirect_pointer!=0){
        read_blocks(indirect_pointer, 1, &indirect_buffer);
    }

    // keep reading if the remaining bytes are bigger than 0
    while (remaining > 0) {
        // meaning we are writing to direct blocks still;
        if (read_ptr_loc < (12 * BLOCK_SIZE)) {
            // find location of the block to write in the pointers
            int offset = read_ptr_loc % BLOCK_SIZE;
            int block_pointer_index = read_ptr_loc / BLOCK_SIZE;
            int block_pointer = inode_table[descriptor.inode_pointer].pointers[block_pointer_index];
            int bytes = read_from_block(block_pointer, buf, remaining, offset);
            // add bytes
            bytes_read += bytes;
            // reduce from remaining.
            remaining -= bytes;
            // incr write ptr
            read_ptr_loc += bytes;
            // move buffer
            buf += bytes;
        }
        // we are going to read from the indirect pointers
        else {
            // set up indirect pointer things
            int block_pointer;
            int offset;
            // there is no indirect pointer, corrupted file system
            if (indirect_pointer == 0) {
                fprintf(stderr, "NOOOOO, corrupted file system, nothing to read in indirect pointers\n");
                return -1;
            } else {
                // find which block we want to operate on
                int block_index = ((read_ptr_loc / BLOCK_SIZE) - 12);
                block_pointer = indirect_buffer[block_index];
                // if we arrive at a new block which is not occupied
            }
            offset = read_ptr_loc % BLOCK_SIZE;
            int bytes = read_from_block(block_pointer, buf, remaining, offset);

            bytes_read += bytes;
            // reduce from remaining.
            remaining -= bytes;
            // incr write ptr
            read_ptr_loc += bytes;
            // move buffer
            buf += bytes;
        }
    }
    // and we are done, update read pointer
    open_file_descriptor_table[fileID].read_pointer = read_ptr_loc;
    return bytes_read;
}

/* sfs_fseek */
// move the read and write pointer to a certain location
int sfs_fseek(int fileID, int loc) {
    if (open_file_descriptor_table[fileID].inode_pointer == 0) {
        fprintf(stderr, "File have not been opened yet. \n");
        return 0;
    }
    open_file_descriptor_table[fileID]
        .read_pointer = loc;
    open_file_descriptor_table[fileID].write_pointer = loc;
    return 1;
}

/* removes a file */
// file: file name to remove
int sfs_remove(char* file) {
    // remove from directories
    int index = 0;
    for (int i = 0; i < MAX_INODES; i++) {
        if (strncmp(directory_table[i].full_filename, file, MAX_FNAME_LENGTH) == 0) {
            index = i;
            break;
        }
    }
    // if doesnt exist -> error
    if (index == 0) {
        fprintf(stderr, "File does not exist in directories table. \n");
        return -1;
    }
    // else remove from directory table
    DIRECTORY_ENTRY dir = directory_table[index];
    strcpy(directory_table[index].full_filename, "");
    int inode_ptr = dir.inode_pointer;
    directory_table[index].inode_pointer = 0;

    // remove from open file table
    if (open_file_descriptor_table[index].inode_pointer == inode_ptr) {
        open_file_descriptor_table[index].inode_pointer = 0;
        open_file_descriptor_table[index].read_pointer = 0;
        open_file_descriptor_table[index].write_pointer = 0;
    }
    // remove inode block
    inode_table[inode_ptr].gid = 0;

    // fill a block with 0s so we can erase data
    int eraser[BLOCK_SIZE];
    memset(&eraser, '\0', sizeof(eraser));

    // resolve indirect pointer data blocks
    int indirect_pointer = inode_table[inode_ptr].indirect_pointer;
    if (indirect_pointer != 0) {
        int indirect_buffer[BLOCK_SIZE / sizeof(int)];
        read_blocks(indirect_pointer, 1, &indirect_buffer);

        for (int i = 0; i < (BLOCK_SIZE / sizeof(int)); i++) {
            if (indirect_buffer[i] != 0) {
                write_blocks(indirect_buffer[i], 1, &eraser);
                set_bit_0("data", indirect_buffer[i]);
            }
        }
        set_bit_0("data", indirect_pointer);
    }
    inode_table[inode_ptr].indirect_pointer = 0;
    inode_table[inode_ptr].link_cnt = 0;
    inode_table[inode_ptr].mode = 0;

    // resolve direct pointers
    for (int i = 0; i < 12; i++) {
        if (inode_table[inode_ptr].pointers[i] != 0) {
            write_blocks(inode_table[inode_ptr].pointers[i], 1, &eraser);
            set_bit_0("data", inode_table[inode_ptr].pointers[i]);
            inode_table[inode_ptr].pointers[i] = 0;
        }
    }
    inode_table[inode_ptr].uid = 0;
    set_bit_0("inode", inode_ptr);

    // overwrite all blocks
    write_blocks(INODE_BITMAP_LOCATION, INODE_BITMAP_SIZE, &inode_bitmap);
    write_blocks(DATA_BLOCK_BITMAP_LOCATION, DATA_BLOCK_BITMAP_SIZE, &data_block_bitmap);
    write_blocks(DIRECTORY_TABLE_LOCATION, DIRECTORY_TABLE_SIZE, &directory_table);
    return 0;
}
