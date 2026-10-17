// IDE: National Semiconductor PC87415 controller (PCI slot 9) with one ATA hard drive, read from the
// game's CHD image. The PCI I/O space appears at physical 0x0A000000 (Galileo PCI I/O window), so the
// legacy command block is at 0x0A0001F0-1F7, the control block at 0x0A0003F6, and the bus-master
// registers wherever BAR 4 points. Its interrupt is the CPU's IP4 (line 2), as in MAME.
//
// Sectors the game writes are kept in an overlay file next to the CHD; the image is never modified.
#include "galileo.h"

#include <libchdr/chd.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace rt;

const int IRQ_LINE = 2;

enum { ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DRDY = 0x40, ST_BSY = 0x80 };

struct Disk {
    chd_file *chd = nullptr;
    uint32_t hunk_bytes = 0, sectors = 0;
    uint32_t cyls = 0, heads = 0, secs = 0;
    std::vector<uint8_t> hunk;
    uint32_t cached_hunk = ~0u;
    std::map<uint32_t, std::vector<uint8_t>> overlay;
    std::string overlay_path;
    bool overlay_dirty = false;

    bool open(const std::string &path, const std::string &ovl) {
        if (chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &chd) != CHDERR_NONE) return false;
        const chd_header *h = chd_get_header(chd);
        hunk_bytes = h->hunkbytes;
        sectors = (uint32_t)(h->logicalbytes / 512);
        hunk.resize(hunk_bytes);
        char meta[256] = {};
        if (chd_get_metadata(chd, HARD_DISK_METADATA_TAG, 0, meta, sizeof(meta) - 1, nullptr, nullptr, nullptr) == CHDERR_NONE)
            sscanf(meta, "CYLS:%u,HEADS:%u,SECS:%u", &cyls, &heads, &secs);
        if (!cyls) {
            heads = 16;
            secs = 63;
            cyls = sectors / (heads * secs);
        }
        overlay_path = ovl;
        if (FILE *f = fopen(ovl.c_str(), "rb")) {
            uint32_t lba;
            std::vector<uint8_t> buf(512);
            while (fread(&lba, 4, 1, f) == 1 && fread(buf.data(), 1, 512, f) == 512) overlay[lba] = buf;
            fclose(f);
        }
        log("ide: %s, %u sectors (CHS %u/%u/%u), %zu overlay sectors", path.c_str(), sectors, cyls, heads, secs, overlay.size());
        return true;
    }

    bool read(uint32_t lba, uint8_t *out) {
        if (lba >= sectors) return false;
        auto it = overlay.find(lba);
        if (it != overlay.end()) {
            memcpy(out, it->second.data(), 512);
            return true;
        }
        uint64_t off = (uint64_t)lba * 512;
        uint32_t h = (uint32_t)(off / hunk_bytes);
        if (h != cached_hunk) {
            if (chd_read(chd, h, hunk.data()) != CHDERR_NONE) return false;
            cached_hunk = h;
        }
        memcpy(out, hunk.data() + off % hunk_bytes, 512);
        return true;
    }

    bool write(uint32_t lba, const uint8_t *in) {
        if (lba >= sectors) return false;
        overlay[lba].assign(in, in + 512);
        overlay_dirty = true;
        return true;
    }

    void save() {
        if (!overlay_dirty || overlay_path.empty()) return;
        if (FILE *f = fopen(overlay_path.c_str(), "wb")) {
            for (auto &[lba, data] : overlay) {
                fwrite(&lba, 4, 1, f);
                fwrite(data.data(), 1, 512, f);
            }
            fclose(f);
        }
        overlay_dirty = false;
    }
};

struct Ide : Device, PciDevice {
    Disk disk;
    uint8_t features = 0, count = 0, sector = 0, cyl_lo = 0, cyl_hi = 0, drive = 0xA0, status = ST_DRDY | ST_DSC, error = 0;
    uint8_t control = 0, command = 0;
    bool irq = false;
    // Data transfer state: buffer of whole sectors.
    std::vector<uint8_t> buf;
    size_t buf_pos = 0;
    bool writing = false;
    uint32_t xfer_lba = 0, xfer_left = 0; // sectors left in the command (after the buffered block)
    int multiple = 1, block = 1;
    // Bus master.
    uint8_t bm_cmd = 0, bm_status = 0;
    uint32_t bm_prd = 0;
    bool dma_pending = false;
    uint64_t last_save = 0;
    // Commands complete after a short delay, like a real drive: BSY until then, then the interrupt.
    enum Pending { NONE, DONE, DATA_IN, DATA_OUT_NEXT } pending = NONE;
    uint64_t pending_at = 0;
    static const uint64_t DELAY_NS = 20000;

    void finish_later(Pending what) {
        pending = what;
        pending_at = now_ns() + DELAY_NS;
        status = ST_BSY | ST_DSC;
    }

    // The boot ROM leaves the bus-master registers (BAR 4) at I/O 0xF00, where the game's driver expects them.
    Ide() : Device("ide", 0x0A000000, 0x00010000), PciDevice(9, 0x0002100B, 0x01018F01) {
        bar_size[4] = 16;
        config[8] = 0xF01;
    }

    void set_irq(bool on) {
        irq = on;
        if (on) bm_status |= 4;
        rt::set_irq(IRQ_LINE, on && !(control & 2));
    }

    uint32_t lba() const {
        if (drive & 0x40) return ((uint32_t)(drive & 15) << 24) | ((uint32_t)cyl_hi << 16) | ((uint32_t)cyl_lo << 8) | sector;
        uint32_t c = ((uint32_t)cyl_hi << 8) | cyl_lo;
        return (c * disk.heads + (drive & 15)) * disk.secs + sector - 1;
    }
    void set_lba(uint32_t l) {
        if (drive & 0x40) {
            sector = l & 0xFF;
            cyl_lo = (l >> 8) & 0xFF;
            cyl_hi = (l >> 16) & 0xFF;
            drive = (drive & 0xF0) | ((l >> 24) & 15);
        } else {
            uint32_t s = l % disk.secs + 1, t = l / disk.secs;
            sector = (uint8_t)s;
            drive = (drive & 0xF0) | (t % disk.heads);
            uint32_t c = t / disk.heads;
            cyl_lo = c & 0xFF;
            cyl_hi = (c >> 8) & 0xFF;
        }
    }

    void abort_command() {
        error = 0x04;
        status = ST_DRDY | ST_DSC | ST_ERR;
        set_irq(true);
    }

    // Loads the next block of a read into the buffer.
    bool load_block() {
        int n = (int)std::min<uint32_t>(xfer_left, block);
        buf.assign((size_t)n * 512, 0);
        for (int i = 0; i < n; i++)
            if (!disk.read(xfer_lba + i, &buf[(size_t)i * 512])) return false;
        xfer_lba += n;
        xfer_left -= n;
        buf_pos = 0;
        set_lba(xfer_lba - 1);
        return true;
    }

    void identify() {
        uint16_t id[256] = {};
        auto str = [&](int word, int len, const char *s) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%-*s", len * 2, s);
            for (int i = 0; i < len; i++) id[word + i] = (uint16_t)((tmp[2 * i] << 8) | tmp[2 * i + 1]);
        };
        id[0] = 0x0040;
        id[1] = (uint16_t)disk.cyls;
        id[3] = (uint16_t)disk.heads;
        id[6] = (uint16_t)disk.secs;
        str(10, 10, "00000000");
        str(23, 4, "1.0");
        str(27, 20, "RECOMP DISK");
        id[47] = 0x8010;           // up to 16 sectors per READ/WRITE MULTIPLE
        id[49] = 0x0300;           // LBA and DMA
        id[51] = 0x0200;
        id[53] = 0x0001;
        id[54] = (uint16_t)disk.cyls;
        id[55] = (uint16_t)disk.heads;
        id[56] = (uint16_t)disk.secs;
        uint32_t chs = disk.cyls * disk.heads * disk.secs;
        id[57] = chs & 0xFFFF;
        id[58] = chs >> 16;
        id[59] = multiple > 1 ? (uint16_t)(0x100 | multiple) : 0;
        id[60] = disk.sectors & 0xFFFF;
        id[61] = disk.sectors >> 16;
        id[63] = 0x0007;
        buf.assign(512, 0);
        memcpy(buf.data(), id, 512);
        buf_pos = 0;
        xfer_left = 0;
        writing = false;
        finish_later(DATA_IN);
    }

    void execute(uint8_t cmd) {
        command = cmd;
        error = 0;
        uint32_t n = count ? count : 256;
        switch (cmd) {
        case 0xEC: identify(); return;
        case 0x20: case 0x21: case 0xC4: // READ SECTORS / READ MULTIPLE
            block = cmd == 0xC4 ? multiple : 1;
            xfer_lba = lba();
            xfer_left = n;
            writing = false;
            if (!load_block()) return abort_command();
            finish_later(DATA_IN);
            return;
        case 0x30: case 0x31: case 0xC5: // WRITE SECTORS / WRITE MULTIPLE
            block = cmd == 0xC5 ? multiple : 1;
            xfer_lba = lba();
            xfer_left = n;
            writing = true;
            buf.assign((size_t)std::min<uint32_t>(xfer_left, block) * 512, 0);
            buf_pos = 0;
            status = ST_DRDY | ST_DSC | ST_DRQ; // no interrupt before the first block
            return;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: // READ / WRITE DMA
            xfer_lba = lba();
            xfer_left = n;
            writing = cmd >= 0xCA;
            dma_pending = true;
            status = ST_DRDY | ST_DSC | ST_BSY;
            if (bm_cmd & 1) run_dma();
            return;
        case 0xC6: // SET MULTIPLE MODE
            if (count == 0 || count > 16 || (count & (count - 1))) return abort_command();
            multiple = count;
            break;
        case 0x40: case 0x41: // READ VERIFY
            set_lba(lba() + n - 1);
            break;
        case 0x10: case 0x70: case 0x91: case 0xEF: case 0xE0: case 0xE1: case 0xE2: case 0xE3:
        case 0xE5: case 0xE6: case 0xE7: case 0x08:
            break;
        default:
            log("ide: unsupported command %02x", cmd);
            return abort_command();
        }
        finish_later(DONE);
    }

    void run_dma() {
        if (!dma_pending) return;
        dma_pending = false;
        bm_status |= 1;
        uint32_t prd = bm_prd;
        std::vector<uint8_t> sec(512);
        size_t sec_pos = 512;
        bool ok = true;
        for (int guard = 0; guard < 4096 && xfer_left; guard++) {
            uint32_t addr = phys_read32(prd), bc = phys_read32(prd + 4);
            uint32_t bytes = bc & 0xFFFF;
            if (!bytes) bytes = 0x10000;
            for (uint32_t i = 0; i < bytes && xfer_left; i += 4) {
                if (!writing) {
                    if (sec_pos == 512) {
                        ok = disk.read(xfer_lba, sec.data());
                        sec_pos = 0;
                    }
                    uint32_t v;
                    memcpy(&v, &sec[sec_pos], 4);
                    phys_write32(addr + i, v);
                    sec_pos += 4;
                    if (sec_pos == 512) { xfer_lba++; xfer_left--; }
                } else {
                    uint32_t v = phys_read32(addr + i);
                    memcpy(&sec[sec_pos == 512 ? 0 : sec_pos], &v, 4);
                    if (sec_pos == 512) sec_pos = 0;
                    sec_pos += 4;
                    if (sec_pos == 512) { ok = disk.write(xfer_lba, sec.data()); xfer_lba++; xfer_left--; }
                }
            }
            if (bc & 0x80000000u) break;
            prd += 8;
        }
        set_lba(xfer_lba - 1);
        bm_status &= ~1;
        if (!ok) {
            bm_status |= 2;
            return abort_command();
        }
        finish_later(DONE);
    }

    uint16_t data_read16() {
        if (!(status & ST_DRQ) || writing) return 0xFFFF;
        uint16_t v = (uint16_t)(buf[buf_pos] | (buf[buf_pos + 1] << 8));
        buf_pos += 2;
        if (buf_pos >= buf.size()) {
            if (xfer_left) {
                if (!load_block()) abort_command();
                else finish_later(DATA_IN);
            } else {
                status = ST_DRDY | ST_DSC;
            }
        }
        return v;
    }

    void data_write16(uint16_t v) {
        if (!(status & ST_DRQ) || !writing) return;
        buf[buf_pos] = v & 0xFF;
        buf[buf_pos + 1] = v >> 8;
        buf_pos += 2;
        if (buf_pos >= buf.size()) {
            size_t n = buf.size() / 512;
            for (size_t i = 0; i < n; i++) disk.write(xfer_lba + (uint32_t)i, &buf[i * 512]);
            xfer_lba += (uint32_t)n;
            xfer_left -= (uint32_t)n;
            set_lba(xfer_lba - 1);
            if (xfer_left) {
                buf.assign((size_t)std::min<uint32_t>(xfer_left, block) * 512, 0);
                buf_pos = 0;
                finish_later(DATA_OUT_NEXT);
            } else {
                finish_later(DONE);
            }
        }
    }

    uint32_t bm_base() const { return config[4 + 4] & ~15u; }

    uint8_t bm_byte(uint32_t r) {
        switch (r) {
        case 0: return bm_cmd;
        case 2: return bm_status;
        case 4: case 5: case 6: case 7: return (uint8_t)(bm_prd >> (8 * (r - 4)));
        }
        return 0; // secondary channel: no drive
    }
    void bm_byte_write(uint32_t r, uint8_t v) {
        switch (r) {
        case 0:
            bm_cmd = v;
            if (v & 1) run_dma();
            break;
        case 2: bm_status = (uint8_t)((bm_status & ~(v & 6)) | (v & 0x60)); break; // bits 1, 2 write-1-to-clear
        case 4: case 5: case 6: case 7: {
            int sh = 8 * (r - 4);
            bm_prd = ((bm_prd & ~(0xFFu << sh)) | ((uint32_t)v << sh)) & ~3u;
            break;
        }
        }
    }

    uint32_t read(uint32_t off, int size) override {
        if (off == 0x1F0) {
            uint32_t v = data_read16();
            if (size == 4) v |= (uint32_t)data_read16() << 16;
            return v;
        }
        uint32_t bm = bm_base();
        if (bm && off >= bm && off < bm + 16) {
            // Bus-master registers, read as bytes: 0 command, 2 status, 4-7 PRD table address.
            uint32_t v = 0;
            for (int i = 0; i < size; i++) v |= (uint32_t)bm_byte(off - bm + i) << (8 * i);
            return v;
        }
        switch (off) {
        case 0x1F1: return error;
        case 0x1F2: return count;
        case 0x1F3: return sector;
        case 0x1F4: return cyl_lo;
        case 0x1F5: return cyl_hi;
        case 0x1F6: return drive;
        case 0x1F7:
            set_irq(false);
            return (drive & 0x10) ? 0 : status; // no slave drive
        case 0x3F6: return (drive & 0x10) ? 0 : status;
        }
        return 0xFF;
    }

    void write(uint32_t off, uint32_t v, int size) override {
        if (off == 0x1F0) {
            data_write16((uint16_t)v);
            if (size == 4) data_write16((uint16_t)(v >> 16));
            return;
        }
        uint32_t bm = bm_base();
        if (bm && off >= bm && off < bm + 16) {
            for (int i = 0; i < size; i++) bm_byte_write(off - bm + i, (uint8_t)(v >> (8 * i)));
            return;
        }
        switch (off) {
        case 0x1F1: features = (uint8_t)v; break;
        case 0x1F2: count = (uint8_t)v; break;
        case 0x1F3: sector = (uint8_t)v; break;
        case 0x1F4: cyl_lo = (uint8_t)v; break;
        case 0x1F5: cyl_hi = (uint8_t)v; break;
        case 0x1F6: drive = (uint8_t)v; break;
        case 0x1F7:
            if (!(drive & 0x10)) execute((uint8_t)v);
            break;
        case 0x3F6:
            if ((v & 4) && !(control & 4)) { // software reset
                status = ST_DRDY | ST_DSC;
                error = 1;
                count = sector = 1;
                cyl_lo = cyl_hi = 0;
                multiple = 1;
            }
            control = (uint8_t)v;
            set_irq(irq);
            break;
        }
    }

    void update(uint64_t now) override {
        if (pending != NONE && now >= pending_at) {
            Pending p = pending;
            pending = NONE;
            status = ST_DRDY | ST_DSC | (p == DATA_IN || p == DATA_OUT_NEXT ? ST_DRQ : 0);
            set_irq(true);
        }
        if (now - last_save > 2000000000ull) {
            last_save = now;
            disk.save();
        }
    }
};

} // namespace

namespace rt {

static Ide *ide_device;

bool ide_shutdown() {
    if (ide_device) ide_device->disk.save();
    return true;
}

bool ide_init(const std::string &chd, const std::string &overlay) {
    Ide *ide = ide_device = new Ide();
    if (!ide->disk.open(chd, overlay)) {
        log("ide: cannot open %s", chd.c_str());
        return false;
    }
    add_device(ide);
    pci_add(ide);
    return true;
}

} // namespace rt
