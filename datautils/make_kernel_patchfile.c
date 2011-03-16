#include <data/common.h>
#include <data/find.h>
#include <data/binary.h>
#include <data/cc.h>
#include <config/placeholder.h>

int patchfd;

static inline void patch_with_range(const char *name, addr_t addr, prange_t pr) {
    uint32_t len = strlen(name);
    write(patchfd, &len, sizeof(len));
    write(patchfd, name, len);
    write(patchfd, &addr, sizeof(addr));
    uint32_t size = pr.size; // size_t no good
    write(patchfd, &size, sizeof(size));
    write(patchfd, pr.start, pr.size);
}

#define patch(name, addr, typeof_to, to...) \
    ({ typeof_to to_[] = to; \
       patch_with_range(name, addr, (prange_t) {&to_[0], sizeof(to_)}); })

uint32_t find_dvp_struct_offset(struct binary *binary) {
    bool is_armv7 = binary->actual_cpusubtype == 9;
    range_t range = b_macho_segrange(binary, "__PRELINK_TEXT");
    addr_t derive_vnode_path = find_bof(range, find_int32(range, find_string(range, "path", 0, true), true), is_armv7);
    uint8_t byte = b_read8(binary, find_data((range_t){binary, derive_vnode_path, 1024}, !is_armv7 ? "00 00 50 e3 02 30 a0 11 - .. 00 94 e5" : "- .. 69 6a 46", 0, true));
    if(is_armv7) {
        return (4 | (byte >> 6)) << 2;
    } else {
        return byte;
    }
}

addr_t find_scratch(struct binary *binary) {
    range_t range = b_macho_segrange(binary, "__PRELINK_TEXT");
    prange_t pr = rangeconv(range);
    for(size_t o = 0; o < pr.size - 0x1000; o += 4) {
        for(size_t p = 0; p < 0x1000; p++) {
            uint8_t c = ((char *) pr.start)[o + p];
            if(!((p != 0 && c == 0) || c == 0x14)) goto nope;
        }
        return range.start + o;
        nope:;
    }
    die("didn't find scratch");
}

addr_t find_sysctl(struct binary *binary, const char *name) {
    addr_t cs = find_string(b_macho_segrange(binary, "__TEXT"), name, 0, true);
    addr_t csref = find_int32(b_macho_segrange(binary, "__DATA"), cs, true);
    return b_read32(binary, csref - 8);
}

void do_kernel(prange_t output, prange_t sandbox, struct binary *binary) {
    bool is_armv7 = binary->actual_cpusubtype == 9;

    bool four_dot_three = true;
    addr_t _kernel_pmap, _PE_i_can_has_debugger, _vn_getpath, _memcmp;
    if(0) {
        _kernel_pmap = 0x8027e2dc;
        _PE_i_can_has_debugger = 0x80203f74;
        _vn_getpath = 0x8008d7bd;
        _memcmp = 0x8006558d;
    } else {
        _kernel_pmap = b_sym(binary, "_kernel_pmap", false);
        _PE_i_can_has_debugger = b_sym(binary, "_PE_i_can_has_debugger", false);
        _vn_getpath = b_sym(binary, "_vn_getpath", true);
        _memcmp = b_sym(binary, "_memcmp", true);
    }


    // '+' = in place only, '-' = in advance only
    // patches
    patch("+kernel_pmap.nx_enabled",
          b_read32(binary, _kernel_pmap) + 0x420,
          uint32_t, {0});
    // the second ref to mem_size
    // 4.3: 23 68 - c3 f8 20 84
    // 4.2: 03 68 - c3 f8 20 24
    if(four_dot_three) {
        patch("-kernel_pmap.nx_enabled initializer",
              find_data(b_macho_segrange(binary, "__TEXT"), "23 68 - c3 f8 24 84", 0, true),
              uint32_t, {0x6424f8c3});
    } else {
        patch("-kernel_pmap.nx_enabled initializer",
              find_data(b_macho_segrange(binary, "__TEXT"), is_armv7 ? "03 68 - c3 f8 20 24" : "84 23 db 00 - d5 50 22 68", 0, true),
              uint32_t, {is_armv7 ? 0xc420f8c3 : 0x682250d0});
    }

    patch("-lunchd",
          find_string(b_macho_segrange(binary, "__DATA"), "/sbin/launchd", 0, true),
          char, "/sbin/lunchd");

    // vm_map_enter (patch1) - allow RWX pages
    patch("vm_map_enter",
          find_data(b_macho_segrange(binary, "__TEXT"), is_armv7 ? "- 02 0f .. .. 63 08 03 f0 01 05 e3 0a 13 f0 01 03" : "- .. .. .. .. .. 08 1e 1c .. 0a 01 22 .. 1c 16 40 .. 40", 0, true),
          uint32_t, {is_armv7 ? 0x46c00f02 : 0x46c046c0});

    // vm_map_protect - allow vm_protect etc. to create RWX pages
    patch("vm_map_protect",
          find_data(b_macho_segrange(binary, "__TEXT"), is_armv7 ? "- 25 f0 04 05 .. e7 92 45 98 bf 02 99 .. d8" : "?", 0, true),
          uint32_t, {0x0500f025});

    // AMFI (patch3) - disable the predefined list of executable stuff
    // safe because it follows longs
    addr_t mystery = find_data(b_macho_segrange(binary, "__PRELINK_TEXT"), four_dot_three ? "- f0 b5 03 af 4d f8 04 8d .. .. 03 78 80 46" : is_armv7 ? "- 90 b5 01 af 14 29 .. .. .. .. 90 f8 00 c0" : "???", 0, true);
    addr_t scratch = resolve_ldr(binary, is_armv7 ? (mystery + 9) : 42);
    patch("AMFI", mystery, uint32_t, {is_armv7 ? 0x47702001 : 0xe3a00001});

    // PE_i_can_has_debugger (patch4) - so AMFI allows non-ldid'd binaries (and some other stuff is allowed)
    patch("PE_i_can_has_debugger",
          _PE_i_can_has_debugger,
          uint32_t, {0x47702001});


    // task_for_pid 0
    // this is necessary so a reboot isn't required after using the screwed up version
    patch("task_for_pid 0",
          find_data(b_macho_segrange(binary, "__TEXT"), is_armv7 ? "85 68 00 23 .. 93 .. 93 - 5c b9 02 a8 29 46 04 22" : "85 68 .. 93 .. 93 - 00 2c 0b d1", 0, true),
          uint32_t, {is_armv7 ? 0xa802e00b : 0xe00b2c00});
        
    // cs_enforcement_disable
    patch("cs_enforcement_disable",
          resolve_ldr(binary, find_data(b_macho_segrange(binary, "__TEXT"), four_dot_three ? "1d ee 90 3f d3 f8 80 33 93 f8 94 30 1b 09 03 f0 01 02 + .. .. .. .." : is_armv7 ? "1d ee 90 3f d3 f8 4c 33 d3 f8 9c 20 + .. .. .. .. 19 68 00 29" : "9c 22 03 59 99 58 + .. .. 1a 68 00 2a", 0, true)),
          uint32_t, {1});

    // sandbox
    range_t range = b_macho_segrange(binary, "__PRELINK_TEXT");
    addr_t sb_evaluate = find_bof(range, find_int32(range, find_string(range, "bad opcode", false, true), true), is_armv7);
    
    preplace32(sandbox, CONFIG_IS_ARMV7, (uint32_t) is_armv7);
    preplace32(sandbox, CONFIG_VN_GETPATH, _vn_getpath);
    preplace32(sandbox, CONFIG_MEMCMP, _memcmp);
    preplace32(sandbox, CONFIG_SB_EVALUATE_ORIG1, b_read32(binary, sb_evaluate));
    preplace32(sandbox, CONFIG_SB_EVALUATE_ORIG2, b_read32(binary, sb_evaluate + 4));
    preplace32(sandbox, CONFIG_SB_EVALUATE_JUMPTO, sb_evaluate + (is_armv7 ? 9 : 8));
    preplace32(sandbox, CONFIG_DVP_STRUCT_OFFSET, find_dvp_struct_offset(binary));
    
    check_no_placeholders(sandbox);
    patch_with_range("sb_evaluate hook",
                     scratch,
                     sandbox);
    
    patch("sb_evaluate",
          sb_evaluate,
          uint32_t, {(is_armv7 ? 0xf000f8df : 0xe51ff004), scratch | 1});


    patch("proc_enforce",
          find_sysctl(binary, "proc_enforce"),
          uint32_t, {0});

    // some "notes"

    addr_t sysent = find_data(b_macho_segrange(binary, "__DATA"), "21 00 00 00 00 10 86 00 -", 0, true);
    addr_t sysent_patch_orig = b_read32(binary, sysent + 4);
    patch("sysent patch", 0, uint32_t, {sysent + 4});
    patch("sysent patch orig", 0, uint32_t, {sysent_patch_orig});
    patch("scratch", 0, uint32_t, {(scratch + sandbox.size + 0xfff) & ~0xfff});
    //patch("IOLog", 0, uint32_t, {b_sym(binary, "_IOLog", true)});
}


int main(int argc, char **argv) {
    struct binary binary;
    b_init(&binary);
    prange_t kernel = load_file(argv[1], false, NULL);
    b_prange_load_macho(&binary, kernel, argv[1]);
    prange_t sandbox = load_file(argv[2], true, NULL);

    patchfd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(patchfd == -1) {
        edie("could not open patchfd");
    }

    do_kernel(kernel, sandbox, &binary);

    close(patchfd);
    return 0;
}

