// Razor-Crest native statistics for the first two Reading Statistics dashboard pages.
// Read-only access to versioned native binary data and checksummed backups.
#![no_std]
#![allow(dead_code)]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    dsn_exit(99);
    loop {}
}

include!("../../dessen/sdk/dessen.rs");
include!("dashboard.rs"); // Device Statistics + Current Book.
include!("preview.rs"); // Local preview of the future server-wide Overall Statistics.
include!("bookhero.rs"); // Shared Current Book and Continue Reading hero card.
include!("pages.rs"); // Your Week and All Books.
include!("calendar.rs"); // Native-style Reading Calendar.
include!("sync.rs"); // Optional Techy-Notes device-code registration.
include!("qr.rs"); // Scannable QR with prefilled pairing link.
include!("settings.rs");
include!("server.rs"); // Account-scoped server totals; local offline fallback.
// Resume marker follows the Dessen app's actual SD root. All supported
// plugin roots work; the firmware has already unloaded the VM on handoff.
fn native_resume_path(path: &mut [u8; 192]) -> usize {
    const SUFFIX: &[u8] = b"/resume-page";
    let n = dsn_fs_app_dir(path);
    if n <= 0 || n as usize + SUFFIX.len() > path.len() { return 0; }
    let end = n as usize;
    path[end..end + SUFFIX.len()].copy_from_slice(SUFFIX);
    end + SUFFIX.len()
}
fn next_page(page: i32) -> i32 { match page { 5 => 7, 7 => 6, 6 => 0, _ => page + 1 } }
fn prev_page(page: i32) -> i32 { match page { 0 => 6, 6 => 7, 7 => 5, _ => page - 1 } }

fn native_stats_handoff(action: i32, page: u8) {
    let mut path = [0u8; 192];
    let n = native_resume_path(&mut path);
    if n == 0 || dsn_fs_write(&path[..n], &[page]) != 0 { return; }
    let _ = dsn_native_stats(action);
}

// sdk/dessen_abi.h
const BLACK: i32 = 1;
const LIGHT: i32 = 2;
const DARK: i32 = 3;
// Match the native StatsDashboardActivity's fixed-size typography instead of
// theme-scaled FreeInk Small/Body/Title font slots.
const SMALL: i32 = 4; // DSN_FONT_NOTOSANS_12 (labels, captions)
const BODY: i32 = 4;  // DSN_FONT_NOTOSANS_12 (body text)
const TITLE: i32 = 5; // DSN_FONT_NOTOSANS_14 (metric values)
const LARGE: i32 = 6; // DSN_FONT_NOTOSANS_18 (monthly total)
const LEFT: i32 = 0;
const CENTER: i32 = 1;
const RIGHT: i32 = 2;
const BOLD: i32 = 1;
const ERR_UNAVAILABLE: i32 = -4;
const SIDE_KEY: u32 = 1 << 8; // Dessen PREV/NEXT source marker

const GLOBAL: &[u8] = b"/.crosspoint/reading-stats/global_v2.bin";
const GLOBAL_BACKUP: &[u8] = b"/.crosspoint/reading-stats/global_v2.bin.bak";
const BOOK_INDEX: &[u8] = b"/.crosspoint/reading-stats/books_v1.bin";
const PERIODS: &[u8] = b"/.crosspoint/reading-stats/time_global_v1.bin";
const PERIODS_BACKUP: &[u8] = b"/.crosspoint/reading-stats/time_global_v1.bin.bak";
const BOOK_PREFIX: &[u8] = b"/.crosspoint/reading-stats/book_";
const BOOK_SUFFIX: &[u8] = b"_v2.bin";
const BOOK_SUFFIX_BACKUP: &[u8] = b"_v2.bin.bak";
const DAYS: usize = 53 * 7; // retained day history
const G: i32 = 18; // safe-area side gutter for narrow displays

// The book to continue: the first row of the recents shelf.
struct Home {
    rec: [u8; 1100],        // "path\ttitle\tauthor\t..." (DSN_SHELF_RECORD_MAX)
    f: [(usize, usize); 5], // path, title, author, progress, optional series
    percent: i32,           // 0..=100, or -1 if unknown
    cover: [u8; 4096],      // {u16 w, u16 h} + 1-bit rows, from dsn_book_cover
    cover_n: usize,
}

impl Home {
    fn field(&self, i: usize) -> &[u8] {
        &self.rec[self.f[i].0..self.f[i].1]
    }
    fn has_book(&self) -> bool {
        self.field(0).first() == Some(&b'/')
    }
    #[inline(never)]
    fn load(&mut self) {
        let n = dsn_recent_shelf(0, 1, &mut self.rec);
        let n = if n > 0 { (n as usize).min(self.rec.len()) } else { 0 };
        let (mut start, mut k) = (0, 0);
        for i in 0..n {
            let c = self.rec[i];
            if c == b'\t' || c == b'\n' {
                if k < 5 {
                    self.f[k] = (start, i);
                    k += 1;
                }
                start = i + 1;
                if c == b'\n' {
                    break;
                }
            }
        }
        if k < 2 {
            self.f[0] = (0, 0); // not a whole record
        }
        if self.has_book() {
            let p = dsn_book_progress(self.field(0));
            self.percent = if (0..=100).contains(&p) { p } else { -1 };
        }
    }
}

struct Stats {
    end: u32,         // local day number (days since 1970) of day[DAYS - 1]: today
    day: [u32; DAYS], // seconds read per local day
    hour: [u32; 4],   // real native morning/afternoon/evening/night totals
    book_count: u32, // count of valid native per-book statistics records
    total: u32,
    sessions: u32,
    progress_bp: u32, // native qualified forward page turns
    err: i32,
}

fn le32(data: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([data[off], data[off+1], data[off+2], data[off+3]])
}
fn record_valid(data: &[u8], magic: u32, version: u32) -> bool {
    if data.len() < 12 || le32(data, 0) != magic || le32(data, 4) != version {
        return false;
    }
    let mut hash: u32 = 2166136261;
    for &byte in &data[..data.len()-4] {
        hash = (hash ^ byte as u32).wrapping_mul(16777619);
    }
    hash == le32(data, data.len()-4)
}
fn read_native(path: &[u8], output: &mut [u8], magic: u32, version: u32) -> bool {
    dsn_fs_read(path, output) == output.len() as i32 && record_valid(output, magic, version)
}

impl Stats {
    #[inline(never)]
    fn load(&mut self, have_clock: bool) {
        let mut raw = [0u8; 1500];
        let mut ok = read_native(GLOBAL, &mut raw, 0x32545352, 2);
        if !ok {
            ok = read_native(GLOBAL_BACKUP, &mut raw, 0x32545352, 2);
        }
        if !ok {
            return; // genuinely empty or unavailable store
        }
        let latest = le32(&raw, 28);
        self.sessions = le32(&raw, 12);
        let total = (le32(&raw, 20) as u64) << 32 | le32(&raw, 16) as u64;
        self.total = total.min(u32::MAX as u64) as u32;
        self.progress_bp = le32(&raw, 24); // repurposed as qualified page turns
        if !have_clock && latest != 0 {
            self.end = latest - 1; // native ordinal is Unix day + 1
        }
        if latest != 0 {
            for i in 0..DAYS {
                let day = self.end as i64 - (DAYS - 1 - i) as i64;
                let ordinal = day + 1;
                if ordinal > 0 && ordinal <= latest as i64 &&
                    latest - (ordinal as u32) < 366 {
                    self.day[i] = le32(&raw, 32 + ((ordinal as usize % 366) * 4));
                }
            }
        }
        // The native store has four truthful periods, not 24 hourly values.
        let mut timed = [0u8; 148];
        if read_native(PERIODS, &mut timed, 0x31505452, 1) ||
            read_native(PERIODS_BACKUP, &mut timed, 0x31505452, 1) {
            for i in 0..4 {
                self.hour[i] = le32(&timed, 16 + 4*i);
            }
        }
        // Native book index uses fixed 316-byte, checksummed records.
        // Bound the scan; count only books with valid native snapshots.
        let mut record = [0u8; 316];
        let mut book_snapshot = [0u8; 1500];
        for index in 0..2048u32 {
            let n = dsn_fs_read_at(BOOK_INDEX, index * 316, &mut record);
            if n != 316 {
                break;
            }
            if !record_valid(&record, 0x314b4252, 1) ||
                !record[16..192].contains(&0) ||
                !record[192..264].contains(&0) ||
                !record[264..312].contains(&0) {
                break; // don't fabricate entries from a corrupt catalog
            }
            let hash = (le32(&record, 12) as u64) << 32 | le32(&record, 8) as u64;
            let mut path = [0u8; 80];
            let prefix = BOOK_PREFIX.len();
            path[..prefix].copy_from_slice(BOOK_PREFIX);
            const HEX: &[u8;16] = b"0123456789abcdef";
            for k in 0..16 {
                path[prefix+k] = HEX[((hash >> (60 - 4*k)) & 15) as usize];
            }
            let suffix = prefix + 16;
            path[suffix..suffix+BOOK_SUFFIX.len()].copy_from_slice(BOOK_SUFFIX);
            let size = suffix + BOOK_SUFFIX.len();
            let valid = read_native(&path[..size], &mut book_snapshot, 0x32545352, 2);
            if !valid {
                path[suffix..suffix+BOOK_SUFFIX_BACKUP.len()].copy_from_slice(BOOK_SUFFIX_BACKUP);
                if !read_native(&path[..suffix+BOOK_SUFFIX_BACKUP.len()], &mut book_snapshot, 0x32545352, 2) {
                    continue;
                }
            }
            self.book_count = self.book_count.saturating_add(1);
        }
    }

    // (current, best) runs of consecutive days with reading. Today not having
    // been read yet doesn't break the current streak.
    fn streaks(&self) -> (u32, u32) {
        let (mut best, mut run) = (0, 0);
        for &d in self.day.iter() {
            run = if d > 0 { run + 1 } else { 0 };
            best = best.max(run);
        }
        let mut cur = 0;
        let skip = (self.day[DAYS - 1] == 0) as usize;
        for &d in self.day.iter().rev().skip(skip) {
            if d == 0 {
                break;
            }
            cur += 1;
        }
        (cur, best) // best streak is limited to the retained 53-week history
    }
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm), y >= 1970.
fn days_from_civil(y: u32, m: u32, d: u32) -> u32 {
    let y = if m <= 2 { y - 1 } else { y };
    let (era, yoe) = (y / 400, y % 400);
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d - 1;
    era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468
}

// A tiny string builder for labels.
struct S {
    b: [u8; 24],
    n: usize,
}

impl S {
    fn new() -> S {
        S { b: [0; 24], n: 0 }
    }
    fn s(mut self, t: &[u8]) -> S {
        for &c in t {
            if self.n < self.b.len() {
                self.b[self.n] = c;
                self.n += 1;
            }
        }
        self
    }
    fn n(self, v: u32) -> S {
        let mut d = [0u8; 10];
        let (mut i, mut v) = (d.len(), v);
        loop {
            i -= 1;
            d[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        self.s(&d[i..])
    }
    fn get(&self) -> &[u8] {
        &self.b[..self.n]
    }
}

// "45m", "5h 45m", "123h"
fn dur(secs: u32) -> S {
    let (h, m) = (secs / 3600, secs / 60 % 60);
    match h {
        0 => S::new().n(m).s(b"m"),
        1..=99 => S::new().n(h).s(b"h ").n(m).s(b"m"),
        _ => S::new().n(h).s(b"h"),
    }
}

fn text(x: i32, y: i32, w: i32, h: i32, s: &[u8], font: i32, align: i32, flags: i32) {
    dsn_text_box(x, y, w, h, s, font, align, 1, flags);
}

// Same 14->12 fallback as the native dashboard when a metric is too wide.
// The ABI returns packed text width (high 16) and line height (low 16).
fn metric_font(value: &[u8], primary: i32, fallback: i32, max_width: i32) -> i32 {
    let width = (dsn_measure_text(value, primary, BOLD) >> 16) as i32;
    if width <= max_width.max(0) { primary } else { fallback }
}

struct Ui {
    w: i32,
    h: i32,
    ls: i32, // line heights: small, body, title. Everything is sized from these,
    lb: i32, // so the layout scales with the board's fonts.
    lt: i32,
    touch: bool,
    buttons: bool,
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    let mut st = Stats {
        end: 0,
        day: [0; DAYS],
        hour: [0; 4],
        book_count: 0,
        total: 0,
        sessions: 0,
        progress_bp: 0,
        err: 0,
    };
    // dsn_clock_t: u16 year, month, day, ... in local time.
    let mut c = [0u8; 8];
    let year = if dsn_clock(&mut c) >= 0 { c[0] as u32 | (c[1] as u32) << 8 } else { 0 };
    let have_clock = year >= 2000 && (1..=12).contains(&c[2]) && (1..=31).contains(&c[3]);
    if have_clock {
        st.end = days_from_civil(year, c[2] as u32, c[3] as u32);
    }
    st.load(have_clock);

    let scr = dsn_screen();
    let mut device = [0u8; 24];
    let device_n = dsn_device(&mut device);
    let touch = device_n >= 20 && device[18] != 0;
    let buttons = device_n >= 20 && device[19] != 0;
    let ui = Ui {
        w: (scr >> 16) as i32,
        h: (scr & 0xffff) as i32,
        ls: dsn_line_height(SMALL),
        lb: dsn_line_height(BODY),
        lt: dsn_line_height(TITLE),
        touch,
        buttons,
    };
    let mut home = Home {
        rec: [0; 1100],
        f: [(0, 0); 5],
        percent: -1,
        cover: [0; 4096],
        cover_n: 0,
    };
    home.load();
    if home.has_book() {
        // Limit the cover to the Current Book hero; keep the original source unmodified.
        let cover_limit = ((ui.content_bottom() - (13 + ui.ls + ui.lt) - 33) * 35 / 100 - 20).clamp(40, 150);
        let (start, end) = home.f[0];
        let n = dsn_book_cover(&home.rec[start..end], cover_limit, &mut home.cover);
        home.cover_n = if n > 4 { (n as usize).min(home.cover.len()) } else { 0 };
    }

    let detail = load_current(&home);
    let month_day = if have_clock { c[3] as u32 } else { 0 };
    // One guest invocation owns this storage; avoid a large constructor copy on the stack.
    static mut CAROUSEL: BookCarousel = BookCarousel::new();
    let carousel = unsafe { &mut *core::ptr::addr_of_mut!(CAROUSEL) };
    carousel.load();
    let native_stats = dsn_native_stats_mode() > 0;
    let mut pairing = Pairing::new();
    pairing.load_web_login();
    let mut server = ServerOverall::new();
    if native_stats && pairing.phase == PairPhase::Paired { server.load_cached(); }
    let mut page = if pairing.phase == PairPhase::Paired { 0 } else { 1 };
    let mut resumed = false;
    let mut disconnect_failed = false;
    let mut settings = StatsSettings::load();
    if native_stats {
        let mut previous = [0u8; 1];
        let mut path = [0u8; 192];
        let n = native_resume_path(&mut path);
        if n > 0 {
            if dsn_fs_read(&path[..n], &mut previous) == 1 && previous[0] <= 8 {
                disconnect_failed = previous[0] == 8;
                page = if disconnect_failed { 6 } else { previous[0] as i32 };
                resumed = true;
            }
            let _ = dsn_fs_delete(&path[..n]);
        }
    }
    let mut previous_page = 0;
    let mut confirm_month_sync = false;
    let mut confirm_web_login = false;
    let mut generating_web_login = false;
    let mut confirm_disconnect = false;
    let mut syncing_month = native_stats && !resumed && settings.auto_fetch &&
        pairing.phase == PairPhase::Paired && server.last_synced != 0 && server.stale(dsn_time());
    let mut sync_focus = 0u8;
    let mut settings_focus = 0u8;
    let mut book_offset = 0u32;
    let mut calendar_offset = 0u32; // months before the current month
    loop {
        let first_page = if pairing.phase == PairPhase::Paired { 0 } else { 1 };
        if page == 0 && first_page == 1 { page = 1; }
        // A first attempt can fail quietly while Wi-Fi is initially off.
        // Returning from Server Sync (where Wi-Fi may have been connected
        // for Web PIN generation) must retry the account-wide Overview.
        if page == 0 && previous_page != 0 && pairing.phase == PairPhase::Paired {
            server.attempted = false;
            server.attempts = 0;
        }
        previous_page = page;
        if page == 6 && confirm_disconnect {
            ui.sync_unpair_confirmation();
        } else if confirm_web_login || generating_web_login {
            ui.web_login_dialog(generating_web_login);
        } else if confirm_month_sync {
            ui.overall_sync_confirmation();
        } else if page == 0 {
            ui.overall_preview(carousel, &server,
                               pairing.phase == PairPhase::Paired, syncing_month);
        } else if page == 7 {
            ui.settings_page(&settings, &server, pairing.phase == PairPhase::Paired, settings_focus);
        } else if page == 6 {
            ui.sync_page(&pairing, native_stats, sync_focus, disconnect_failed);
        } else if page <= 2 {
            ui.dashboard_render(&st, &home, &detail, page - 1, month_day, have_clock);
        } else {
            dsn_clear();
            ui.header(page);
            let month_number = if have_clock { year * 12 + c[2] as u32 - 1 - calendar_offset } else { 0 };
            let shown_year = if have_clock { month_number / 12 } else { 0 };
            let shown_month = if have_clock { month_number % 12 + 1 } else { 0 };
            let shown_day = if calendar_offset == 0 { month_day } else { 0 };
            ui.extra_page(&st, page - 1, shown_year, shown_month, shown_day, book_offset);
        }
        ui.draw_footer(page, carousel.count, native_stats && page == 6 && pairing.phase == PairPhase::Paired && !confirm_disconnect);
        dsn_refresh();
        if generating_web_login {
            generating_web_login = false;
            native_stats_handoff(1, 6);
            continue;
        }
        // The confirmation is painted first. Only after a second, explicit
        // confirmation do we display Syncing... before the firmware reboot.
        if syncing_month {
            syncing_month = false;
            if native_stats {
                native_stats_handoff(1, 0);
            } else {
                server.attempted = false;
                server.attempts = 0;
                server.fetch(&pairing);
            }
            continue;
        }
        // Show local statistics promptly. Fetch account-wide statistics once
        // after the initial frame, or again when explicitly refreshed.
        if page == 0 && pairing.phase == PairPhase::Paired && !server.attempted {
            if native_stats { server.load_cached(); } else { server.fetch(&pairing); }
            continue;
        }
        loop {
            // First repaint the "Requesting/Checking" state. Run the blocking
            // network syscall only after Dessen has queued that display update.
            if pairing.pending && !native_stats {
                pairing.activate();
                break;
            }
            let e = dsn_wait_event();
            if confirm_disconnect {
                match e & 0xff {
                    1 => { confirm_disconnect = false; native_stats_handoff(2, 0); break; }
                    2 => { confirm_disconnect = false; break; }
                    5 => {
                        let x = ((e >> 8) & 0xfff) as i32;
                        let y = ((e >> 20) & 0xfff) as i32;
                        let (yes_x, no_x, by, bw, bh) = ui.overall_sync_confirmation_buttons();
                        if y >= by && y < by + bh && x >= yes_x && x < yes_x + bw {
                            confirm_disconnect = false;
                            native_stats_handoff(2, 0);
                        } else if x >= no_x && x < no_x + bw && y >= by && y < by + bh {
                            confirm_disconnect = false;
                        }
                        break;
                    }
                    _ => {}
                }
                continue;
            }
            if confirm_web_login {
                match e & 0xff {
                    1 => { confirm_web_login = false; generating_web_login = true; break; }
                    2 => { confirm_web_login = false; break; }
                    5 => {
                        let x = ((e >> 8) & 0xfff) as i32;
                        let y = ((e >> 20) & 0xfff) as i32;
                        let (yes_x, no_x, by, bw, bh) = ui.overall_sync_confirmation_buttons();
                        if y >= by && y < by + bh {
                            if x >= yes_x && x < yes_x + bw { generating_web_login = true; }
                            else if x < no_x || x >= no_x + bw { continue; }
                            confirm_web_login = false;
                            break;
                        }
                    }
                    _ => {}
                }
                continue;
            }
            if confirm_month_sync {
                match e & 0xff {
                    1 => { confirm_month_sync = false; page = 0; syncing_month = true; break; }
                    2 => { confirm_month_sync = false; break; }
                    5 => {
                        let x = ((e >> 8) & 0xfff) as i32;
                        let y = ((e >> 20) & 0xfff) as i32;
                        let (yes_x, no_x, by, bw, bh) = ui.overall_sync_confirmation_buttons();
                        if y >= by && y < by + bh && x >= yes_x && x < yes_x + bw {
                            confirm_month_sync = false;
                            page = 0;
                            syncing_month = true;
                        } else if y < by || y >= by + bh || x >= no_x && x < no_x + bw {
                            confirm_month_sync = false;
                        }
                        break;
                    }
                    _ => {}
                }
                continue;
            }
            match e & 0xff {
                11 if !native_stats && page == 0 && pairing.phase == PairPhase::Paired &&
                    server.attempted && !server.available && server.attempts < 3 => {
                    // Retry twice after transient Wi-Fi/TLS startup failures.
                    // Don't redraw e-paper unless remote data actually arrives.
                    server.fetch(&pairing);
                    if server.available { break; }
                }
                1 if page == 7 => {
                    if settings_focus == 0 { settings.toggle(); }
                    else if pairing.phase == PairPhase::Paired { confirm_month_sync = true; }
                    else { page = 6; }
                    break;
                }
                1 if page == 0 => {
                    if let Some(book) = carousel.current() { dsn_open_book(book.path()); }
                }
                1 if page == 2 && home.has_book() => { dsn_open_book(home.field(0)); }
                1 if page == 6 => {
                    if native_stats {
                        if pairing.phase != PairPhase::Paired { native_stats_handoff(0, 0); }
                        else if !ui.touch {
                            match sync_focus {
                                0 => confirm_web_login = true,
                                1 => { page = 0; confirm_month_sync = true; }
                                _ => confirm_disconnect = true,
                            }
                        }
                    } else { pairing.schedule(); }
                    break;
                }
                11 if page == 6 && pairing.phase == PairPhase::Paired && pairing.web_valid => {
                    if !pairing.web_login_available() {
                        pairing.web_valid = false;
                        pairing.message = b"Web PIN expired. Generate a new one";
                        break;
                    }
                }
                11 if !native_stats && page == 6 && pairing.phase == PairPhase::Code => {
                    // Firmware's five-second wake event. Check quietly without
                    // two full e-ink refreshes per poll; repaint only when the
                    // visible status/phase actually changes.
                    let previous_message = pairing.message;
                    pairing.poll();
                    if pairing.phase != PairPhase::Code || pairing.message != previous_message {
                        break;
                    }
                }
                2 if page == first_page => return 0,
                2 => { page = first_page; break; }
                3 | 8 | 10 => {
                    if page == 7 && (e & 0xff) == 3 && (e & SIDE_KEY) == 0 {
                        settings_focus = 0; break;
                    }
                    if native_stats && page == 6 && pairing.phase == PairPhase::Paired && (e & 0xff) == 3 && (e & SIDE_KEY) == 0 {
                        sync_focus = (sync_focus + 2) % 3;
                        break;
                    }
                    // Front Previous browses books when there are multiple.
                    // The configured side key always pages, regardless of book count.
                    if page == 0 && carousel.count > 1 &&
                        (((e & 0xff) == 3 && (e & SIDE_KEY) == 0) ||
                         ((e & 0xff) == 10 && ui.overall_carousel_contains(
                             ((e >> 8) & 0xfff) as i32, ((e >> 20) & 0xfff) as i32))) {
                        carousel.step(-1);
                        break;
                    }
                    if page == 5 && book_offset >= 5 { book_offset -= 5; }
                    else { page = prev_page(page); if page == 0 && first_page == 1 { page = 6; } }
                    break;
                }
                4 | 7 | 9 => {
                    if native_stats && page == 6 && pairing.phase == PairPhase::Paired && (e & 0xff) == 4 && (e & SIDE_KEY) == 0 {
                        sync_focus = (sync_focus + 1) % 3;
                        break;
                    }
                    if page == 7 && (e & 0xff) == 4 && (e & SIDE_KEY) == 0 {
                        settings_focus = 1; break;
                    }
                    if page == 0 && carousel.count > 1 &&
                        (((e & 0xff) == 4 && (e & SIDE_KEY) == 0) ||
                         ((e & 0xff) == 9 && ui.overall_carousel_contains(
                             ((e >> 8) & 0xfff) as i32, ((e >> 20) & 0xfff) as i32))) {
                        carousel.step(1);
                        break;
                    }
                    if page == 5 && book_offset + 5 < st.book_count { book_offset += 5; }
                    else { page = next_page(page); if page == 0 { page = first_page; } }
                    break;
                }
                5 => {
                    // Tap the current-book hero to resume reading; elsewhere change page.
                    let x = ((e >> 8) & 0xfff) as i32;
                    let y = ((e >> 20) & 0xfff) as i32;
                    if x < G + 55 && y < 13 + ui.ls + ui.lt {
                        if page == first_page { return 0; }
                        page = first_page;
                        break;
                    } else if page == 0 {
                        let (mx, my, mw, mh) = ui.overall_month_bounds();
                        if ui.touch && x >= mx && x < mx + mw &&
                           y >= my && y < my + mh {
                            // The entire month widget opens a confirmation,
                            // never starts networking or advances the page.
                            if pairing.phase == PairPhase::Paired {
                                server.icon_revealed = true;
                                confirm_month_sync = true;
                            }
                            break;
                        }
                        if ui.overall_carousel_contains(x, y) && carousel.count > 0 {
                            if let Some(book) = carousel.current() { dsn_open_book(book.path()); }
                        } else {
                            page = 1;
                            break;
                        }
                    } else if page == 4 && have_clock && y >= 13 + ui.ls + ui.lt &&
                              y < 13 + ui.ls + ui.lt * 2 + 20 && x < ui.w / 4 {
                        calendar_offset = (calendar_offset + 1).min(11);
                        break;
                    } else if page == 4 && have_clock && y >= 13 + ui.ls + ui.lt &&
                              y < 13 + ui.ls + ui.lt * 2 + 20 && x > ui.w * 3 / 4 {
                        calendar_offset = calendar_offset.saturating_sub(1);
                        break;
                    } else if page == 7 {
                        let (bx, by, bw, bh) = ui.settings_bounds(0);
                        let (rx, ry, rw, rh) = ui.settings_bounds(1);
                        if x >= bx && x < bx+bw && y >= by && y < by+bh { settings.toggle(); }
                        else if x >= rx && x < rx+rw && y >= ry && y < ry+rh {
                            if pairing.phase == PairPhase::Paired { confirm_month_sync = true; } else { page = 6; }
                        }
                        break;
                    } else if page == 6 {
                        // This is a single-action page. Accept taps anywhere
                        // in the content card: real devices may report a small
                        // offset between FreeInk drawing and touch coordinates.
                        if ui.touch && y >= 13 + ui.ls + ui.lt &&
                            x >= G && x < ui.w - G &&
                            y < ui.content_bottom() {
                            if native_stats && pairing.phase == PairPhase::Paired {
                                for action in [0, 2] {
                                    let (bx, by, bw, bh) = ui.native_sync_action_bounds(action);
                                    if x >= bx && x < bx + bw && y >= by && y < by + bh {
                                        if action == 0 { confirm_web_login = true; }
                                        else { confirm_disconnect = true; }
                                        break;
                                    }
                                }
                                break;
                            }
                            let (bx, by, bw, bh) = ui.sync_button_bounds();
                            // A scanned PIN must remain valid while viewing
                            // the QR: only its action button regenerates it.
                            if pairing.phase != PairPhase::Paired ||
                                (x >= bx && x < bx + bw && y >= by && y < by + bh) {
                                if native_stats { native_stats_handoff(if pairing.phase == PairPhase::Paired { 1 } else { 0 }, 0); }
                                else { pairing.schedule(); }
                                break;
                            }
                        }
                    } else if page == 2 && home.has_book() && y < (13 + ui.ls + ui.lt) + (ui.content_bottom() - (13 + ui.ls + ui.lt) - 33) * 35 / 100 {
                        dsn_open_book(home.field(0));
                    } else {
                        page = next_page(page); if page == 0 { page = first_page; }
                        break;
                    }
                }
                _ => {}
            }
        }
    }
}
