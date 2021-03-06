#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "ips.h"
#include "log.h"

static const size_t BUF_SIZE = 32768;

// Copy the input file to the output file, it is assumed
// that both files will be at position 0 before this function
// is called.
static int copy_file(FILE* input_file, FILE* output_file) {
    uint8_t buf[BUF_SIZE];
    int rc;

    int infd = fileno(input_file);
    if (infd == -1) {
        rombp_log_err("Bad input file, is the stream closed?\n");
        return infd;
    }
    struct stat input_file_stat;
    rc = fstat(infd, &input_file_stat);
    if (rc == -1) {
        rombp_log_err("Failed to stat input file, errno: %d\n", errno);
        return rc;
    }

    off_t input_file_size = input_file_stat.st_size;
    size_t total_read = 0;
    while (1) {
        size_t nread = fread(&buf, 1, BUF_SIZE, input_file);
        total_read += nread;
        if (nread < BUF_SIZE) {
            if (total_read < input_file_size) {
                rombp_log_err("Failed to read the entire input file, read: %ld bytes, input file size: %ld\n", (long int)total_read, (long int)input_file_size);
                return -1;
            } else {
                return 0;
            }
        }
        size_t nwritten = fwrite(&buf, 1, nread, output_file);
        if (nwritten < nread) {
            rombp_log_err("Tried to copy %ld bytes to the output file, but only copied: %ld\n", (long int)nread, (long int)nwritten);
            return -1;
        }
    }

    return 0;
}

rombp_patch_err ips_start(FILE* input_file, FILE* output_file) {
    // Once the header is verified, copy the input to output
    int rc = copy_file(input_file, output_file);
    if (rc != 0) {
        rombp_log_err("Failed to seek to copy input file to output file: %d\n", rc);
        return PATCH_ERR_IO;
    }

    return PATCH_OK;
}

static const uint8_t IPS_EXPECTED_MARKER[] = {
    0x50, 0x41, 0x54, 0x43, 0x48 // PATCH
};
static const size_t IPS_MARKER_SIZE = sizeof(IPS_EXPECTED_MARKER) / sizeof(uint8_t);

rombp_patch_err ips_verify_marker(FILE* ips_file) {
    return patch_verify_marker(ips_file, IPS_EXPECTED_MARKER, IPS_MARKER_SIZE);
}

static const size_t HUNK_PREAMBLE_BYTE_SIZE = 5;

static const inline uint32_t be_24bit_int(uint8_t *buf) {
    return ((buf[0] << 16) & 0x00FF0000) |
        ((buf[1] << 8) & 0x0000FF00) |
        (buf[2] & 0x000000FF);
}

static const inline uint16_t be_16bit_int(uint8_t *buf) {
    return ((buf[0] << 8) & 0xFF00) |
        (buf[1] & 0x00FF);
}

static const size_t RLE_PAYLOAD_BYTE_SIZE = 3;
// Extract the RLE length, as well as the byte value that needs to repeated (rle_length times)
static int ips_get_rle_payload(FILE* ips_file, uint32_t* rle_length, uint8_t* rle_value) {
    uint8_t buf[RLE_PAYLOAD_BYTE_SIZE];

    size_t nread = fread(&buf, 1, RLE_PAYLOAD_BYTE_SIZE, ips_file);
    if (nread < RLE_PAYLOAD_BYTE_SIZE) {
        int err = ferror(ips_file);
        if (err != 0) {
            rombp_log_err("Error reading from IPS file for RLE payload, error: %d\n", err);
            return -1;
        }
        if (feof(ips_file) != 0) {
            rombp_log_err("Unexpectedly reached EOF while trying to read the RLE payload\n");
            return -1;
        }
    }

    *rle_length = be_16bit_int(buf);
    *rle_value = buf[2];

    return 0;
}

static int ips_next_hunk_header(FILE* ips_file, ips_hunk_header* header) {
    uint8_t buf[HUNK_PREAMBLE_BYTE_SIZE];

    assert(header != NULL);

    // Read the hunk preamble
    // 3 byte offset
    // 2 byte payload length.
    size_t nread = fread(&buf, 1, HUNK_PREAMBLE_BYTE_SIZE, ips_file);
    if (nread < HUNK_PREAMBLE_BYTE_SIZE) {
        int err = ferror(ips_file);
        if (err != 0) {
            rombp_log_err("Error reading from IPS file, error: %d\n", err);
            return HUNK_ERR_IO;
        }
        if (feof(ips_file) != 0) {
            return HUNK_DONE;
        }
    }

    // We have a 5 byte buffer of the hunk preamble, decode values:
    header->offset = be_24bit_int(buf);
    header->length = be_16bit_int(buf+3);

    return HUNK_NEXT;
}

// Write the rle_value to the output_file rle_hunk_length times. By the time this function is
// called, we should assume that output_file has already been seeked to the correct position
// in the file.
static int ips_write_rle_hunk(FILE* output_file, uint32_t rle_hunk_length, uint8_t rle_value) {
    // It'd be sweet if we could do this in one fwrite call. Oh well.
    size_t nwritten;

    for (int i = 0; i < rle_hunk_length; i++) {
        nwritten = fwrite(&rle_value, sizeof(uint8_t), 1, output_file);
        if (nwritten == 0) {
            rombp_log_err("Failed to write RLE byte value, length: %d, value: %d, i: %d\n",
                    rle_hunk_length, rle_value, i);
            return -1;
        }
    }

    return 0;
}
 
// For normal hunks (non-RLE encoded), copy payload values from the IPS file to the output.
// By the time this function is called, the ips_file should be positioned at the start of the payload
// and the output file should already be seeked to the destination file position.
static int ips_write_hunk(FILE* ips_file, FILE* output_file, uint32_t hunk_length) {
    uint8_t buf[BUF_SIZE];

    size_t length_remaining = hunk_length;
    size_t nread;
    size_t nwritten;
    while (length_remaining > 0) {
        size_t amount_to_copy = MIN(BUF_SIZE, length_remaining);

        nread = fread(&buf, 1, amount_to_copy, ips_file);
        if (nread < amount_to_copy) {
            int err = ferror(ips_file);
            if (err != 0) {
                rombp_log_err("Error reading payload IPS file, error: %d\n", err);
                return -1;
            }
            if (feof(ips_file) != 0) {
                rombp_log_err("Unexpected EOF while trying to read payload from IPS file, remaining: %ld, ips file pos: %ld, nread: %ld\n", (long int)length_remaining, ftell(ips_file), (long int)nread);
                return -1;
            }
        }
        nwritten = fwrite(&buf, 1, nread, output_file);
        if (nwritten < nread) {
            rombp_log_err("Failed to write all data to output file, expected to write: %ld bytes, wrote: %ld\n", (long int)nread, (long int)nwritten);
            return -1;
        }
        length_remaining -= nwritten;
    }

    return 0;
}

static int ips_patch_hunk(ips_hunk_header* hunk_header, FILE* input_file, FILE* output_file, FILE* ips_file) {
    // Seek the output file to the specified hunk offset
    int rc = fseek(output_file, hunk_header->offset, SEEK_SET);
    if (rc == -1) {
        rombp_log_err("Error seeking output file to offset: %d, error: %d\n",
                      hunk_header->offset, errno);
        return rc;
    }

    rombp_log_info("Hunk RLE: %d, offset: %d, length: %d, ips_offset: %ld\n",
                   hunk_header->length == 0,
                   hunk_header->offset,
                   hunk_header->length,
                   ftell(ips_file));

    // 0 length header means the hunk is run length encoded (RLE).
    // We have to look into the payload to determine how big the hunk
    // is.
    if (hunk_header->length == 0) {
        uint32_t rle_hunk_length;
        uint8_t rle_value;
        rc = ips_get_rle_payload(ips_file, &rle_hunk_length, &rle_value);
        if (rc < 0) {
            rombp_log_err("Failed to find RLE payload length, err: %d\n", rc);
            return rc;
        }
        rc = ips_write_rle_hunk(output_file, rle_hunk_length, rle_value);
        if (rc < 0) {
            rombp_log_err("Failed to write RLE hunk value to output, rle length: %d, rle value: %d\n",
                          rle_hunk_length, rle_value);
            return rc;
        }
    } else {
        rc = ips_write_hunk(ips_file, output_file, hunk_header->length);
        if (rc < 0) {
            rombp_log_err("Failed writing non-RLE hunk value to output, length: %d\n",
                          hunk_header->length);
            return rc;
        }
    }

    return 0;
}

rombp_hunk_iter_status ips_next(FILE* input_file, FILE* output_file, FILE* ips_file) {
    ips_hunk_header hunk_header;

    int rc = ips_next_hunk_header(ips_file, &hunk_header);
    if (rc < 0) {
        rombp_log_err("Error getting next hunk, at hunk count: %d\n", rc);
        return HUNK_ERR_IO;
    } else if (rc == HUNK_DONE) {
        return HUNK_DONE;
    } else {
        assert(rc == HUNK_NEXT);
        rc = ips_patch_hunk(&hunk_header, input_file, output_file, ips_file);
        if (rc < 0) {
            rombp_log_err("Failed to patch next hunk: %d\n", rc);
            return HUNK_ERR_IO;
        }

        return HUNK_NEXT;
    }
}
 
