#ifndef MRP_FORMAT_H
#define MRP_FORMAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  Format .mrp (Equinox OS Runnable Program) — v1
// ----------------------------------------------------------------------------
//  File .mrp = [ header (18 byte, fixed) ][ raw machine code (flat binary) ]
//
//  Why not text "#is_valid_mrp\n0110101..." like the original idea?
//  A random-length signature is EXPENSIVE to validate (you must compare N
//  bytes, with N unclear) and easily thrown off by a different
//  newline/whitespace (e.g. created on Windows vs Linux, \r\n vs \n).
//  A 4-byte magic number stays easy to check (one uint32 comparison) and
//  unambiguous — just like ELF uses "\x7fELF" or PE uses "MZ".
//
//  The checksum field exists so the loader can DETECT corrupt/truncated
//  files before jumping into incomplete code (which in kernel mode = a
//  total crash, not just a segfault like in modern OSes).
// ============================================================================

#define MRP_MAGIC0 'M'
#define MRP_MAGIC1 'R'
#define MRP_MAGIC2 'P'
#define MRP_MAGIC3 '1'

#define MRP_VERSION 1

// Flags (bit field in header.flags)
#define MRP_FLAG_NONE       0x00
#define MRP_FLAG_NEEDS_GUI  0x01   // reserved for the future (LVGL access)

struct __attribute__((packed)) mrp_header {
    uint8_t  magic[4];      // must be 'M','R','P','1'
    uint8_t  version;       // format version (currently must == MRP_VERSION)
    uint32_t entry_offset;  // offset of _start relative to the START of the code (bytes after the header)
    uint32_t code_size;     // size of the code payload after the header, in bytes
    uint8_t  flags;         // MRP_FLAG_*
    uint32_t checksum;      // simple additive checksum over code_size bytes of code
};

#define MRP_HEADER_SIZE (sizeof(struct mrp_header))  // = 18 bytes

// Compute the simple checksum used by the loader & packer (the algorithm
// must be exactly identical on both sides). Not cryptographic — just to
// catch truncated/corrupt files, not for security.
static inline uint32_t mrp_checksum(const uint8_t* data, uint32_t len) {
    uint32_t sum = 0x811C9DC5u; // non-zero seed so an all-zero array does not checksum to 0
    for (uint32_t i = 0; i < len; i++) {
        sum = ((sum << 5) | (sum >> 27)) ^ data[i]; // rotate-xor, cheap & enough for a corruption check
    }
    return sum;
}

// Full validation of an .mrp buffer ALREADY loaded into memory.
// total_len = size of the whole file (header + code).
// Returns 1 if valid, 0 if not (see *out_reason if you need the reason).
enum mrp_validate_reason {
    MRP_OK = 0,
    MRP_ERR_TOO_SMALL,       // file smaller than the header alone
    MRP_ERR_BAD_MAGIC,       // first 4 bytes are not "MRP1"
    MRP_ERR_BAD_VERSION,     // header version unknown to this loader
    MRP_ERR_SIZE_MISMATCH,   // header code_size != remaining file bytes
    MRP_ERR_BAD_ENTRY,       // entry_offset out of range of code_size
    MRP_ERR_BAD_CHECKSUM,    // checksum mismatch -> corrupt/truncated file
    MRP_ERR_EMPTY_CODE       // code_size == 0, nothing to run
};

static inline int is_valid_mrp(const uint8_t* file_data, uint32_t total_len,
                                enum mrp_validate_reason* out_reason) {
    enum mrp_validate_reason reason = MRP_OK;
    int ok = 0;

    if (total_len < MRP_HEADER_SIZE) {
        reason = MRP_ERR_TOO_SMALL;
    } else {
        const struct mrp_header* hdr = (const struct mrp_header*)file_data;

        if (hdr->magic[0] != MRP_MAGIC0 || hdr->magic[1] != MRP_MAGIC1 ||
            hdr->magic[2] != MRP_MAGIC2 || hdr->magic[3] != MRP_MAGIC3) {
            reason = MRP_ERR_BAD_MAGIC;
        } else if (hdr->version != MRP_VERSION) {
            reason = MRP_ERR_BAD_VERSION;
        } else if (hdr->code_size == 0) {
            reason = MRP_ERR_EMPTY_CODE;
        } else if (hdr->code_size != (total_len - MRP_HEADER_SIZE)) {
            reason = MRP_ERR_SIZE_MISMATCH;
        } else if (hdr->entry_offset >= hdr->code_size) {
            reason = MRP_ERR_BAD_ENTRY;
        } else {
            const uint8_t* code = file_data + MRP_HEADER_SIZE;
            uint32_t computed = mrp_checksum(code, hdr->code_size);
            if (computed != hdr->checksum) {
                reason = MRP_ERR_BAD_CHECKSUM;
            } else {
                ok = 1;
            }
        }
    }

    if (out_reason) *out_reason = reason;
    return ok;
}

static inline const char* mrp_reason_str(enum mrp_validate_reason r) {
    switch (r) {
        case MRP_OK:                  return "OK";
        case MRP_ERR_TOO_SMALL:       return "file too small for the .mrp header";
        case MRP_ERR_BAD_MAGIC:       return "magic bytes do not match (not an .mrp file)";
        case MRP_ERR_BAD_VERSION:     return ".mrp version not supported by this loader";
        case MRP_ERR_SIZE_MISMATCH:   return "code_size in header does not match the file size (file truncated/corrupt)";
        case MRP_ERR_BAD_ENTRY:       return "entry_offset out of range of the code";
        case MRP_ERR_BAD_CHECKSUM:    return "checksum mismatch (corrupt file)";
        case MRP_ERR_EMPTY_CODE:      return "code_size == 0, nothing to run";
        default:                      return "unknown error";
    }
}

#ifdef __cplusplus
}
#endif

#endif
