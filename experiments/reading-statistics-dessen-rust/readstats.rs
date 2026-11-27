// Razor-Crest native statistics for the first two Reading Statistics dashboard pages.
// Read-only JSONL history through the native provider or the stock-runtime scanner.
#![no_std]
#![allow(dead_code)]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    dsn_exit(unsafe { core::ptr::read_volatile(core::ptr::addr_of!(CHECKPOINT)) });
    loop {}
}

// Volatile diagnostic marker survives optimizer and VM continuation.
static mut CHECKPOINT: i32 = 0;
#[inline(never)]
fn checkpoint(code: i32) {
    unsafe { core::ptr::write_volatile(core::ptr::addr_of_mut!(CHECKPOINT), code); }
}

// Build labels in place instead of passing a 28-byte value through every
// formatting call. Keep the original bounded/truncating semantics.
macro_rules! label {
    (s($s:expr)) => { S::literal($s) };
    (n($n:expr)) => { S::number($n, b"") };
    (n($n:expr), s($s:expr)) => { S::number($n, $s) };
    ($($method:ident($arg:expr)),+ $(,)?) => {{
        let mut value = S::new();
        $(value.$method($arg);)+
        value
    }};
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
include!("session_history.rs");
include!("native_history.rs");
include!("history_upload.rs");
include!("server.rs"); // Account-scoped server totals; local offline fallback.
fn next_page(page: i32) -> i32 { match page { 5 => 7, 7 => 6, 6 => 0, _ => page + 1 } }
fn prev_page(page: i32) -> i32 { match page { 0 => 6, 6 => 7, 7 => 5, _ => page - 1 } }

// sdk/dessen_abi.h
const BLACK: i32 = 1;
const LIGHT: i32 = 2;
const DARK: i32 = 3;
// Match the native StatsDashboardActivity's fixed-size typography instead of
// theme-scaled FreeInk Small/Body/Title font slots.
const SMALL: i32 = 3; // DSN_FONT_NOTOSANS_12 (labels, captions)
const BODY: i32 = 3;  // DSN_FONT_NOTOSANS_12 (body text)
const TITLE: i32 = 4; // DSN_FONT_NOTOSANS_14 (metric values)
const LARGE: i32 = 6; // DSN_FONT_NOTOSANS_18 (monthly total)
const LEFT: i32 = 0;
const CENTER: i32 = 1;
const RIGHT: i32 = 2;
const BOLD: i32 = 1;
const ERR_UNAVAILABLE: i32 = -4;
const SIDE_KEY: u32 = 1 << 8; // Dessen PREV/NEXT source marker

const DAYS: usize = 53 * 7; // retained day history
const G: i32 = 18; // safe-area side gutter for narrow displays

// The book to continue: the first row of the recents shelf.
struct Home {
    rec: [u8; 1100],        // "path\ttitle\tauthor\t..." (DSN_SHELF_RECORD_MAX)
    f: [(usize, usize); 5], // path, title, author, progress, optional series
    percent: i32,           // 0..=100, or -1 if unknown
    cover: [u8; 2304],      // {u16 w, u16 h} + 1-bit rows, from dsn_book_cover
    cover_n: usize,
    shelf_index: u32,
}

#[inline(never)]
fn shelf_fields(rec: &[u8]) -> Option<[(usize,usize);5]> {
    let mut fields=[(0,0);5];
    let (mut start,mut k)=(0,0);
    for (i,&c) in rec.iter().enumerate() {
        if c==b'\t' || c==b'\n' {
            if k<5 {fields[k]=(start,i);k+=1;}
            start=i+1;
            if c==b'\n' {
                return (k>=2 && rec.first()==Some(&b'/')).then_some(fields);
            }
        }
    }
    None
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
        self.percent = -1;
        self.shelf_index=u32::MAX;
        for index in 0..10 {
            self.f = [(0,0);5];
            let n = dsn_recent_shelf(index, 1, &mut self.rec);
            if n <= 0 { break; }
            if let Some(fields)=shelf_fields(&self.rec[..(n as usize).min(self.rec.len())]) {
                self.f=fields;
                let p = dsn_book_progress(self.field(0));
                if (0..100).contains(&p) { self.percent=p; self.shelf_index=index; return; }
            }
        }
        self.f = [(0,0);5];
    }
    #[inline(never)]
    fn reload(&mut self) {
        self.f=[(0,0);5];
        if self.shelf_index==u32::MAX{return;}
        let n=dsn_recent_shelf(self.shelf_index,1,&mut self.rec);
        if n>0 && n as usize<=self.rec.len(){
            if let Some(fields)=shelf_fields(&self.rec[..n as usize]){self.f=fields;}
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
    log_source: bool,
    native_stats: bool,
    books_limited: bool,
    week_hour: [u32; 4],
    date_known: bool,
    date_current: bool,
}

#[inline(never)]
fn le32(data: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([data[off], data[off+1], data[off+2], data[off+3]])
}
impl Stats {
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
    #[inline(never)]
    fn literal(t: &[u8]) -> S {
        let mut s=S::new(); s.s(t); s
    }
    #[inline(never)]
    fn number(n:u32, suffix:&[u8]) -> S {
        let mut s=S::new(); s.n(n); s.s(suffix); s
    }
    fn new() -> S {
        S { b: [0; 24], n: 0 }
    }
    #[inline(never)]
    fn s(&mut self, t: &[u8]) {
        let n = t.len().min(self.b.len() - self.n);
        self.b[self.n..self.n+n].copy_from_slice(&t[..n]);
        self.n += n;
    }
    #[inline(never)]
    fn n(&mut self, mut v: u32) {
        let mut d = [0u8; 10];
        let mut i = d.len();
        loop {
            i -= 1;
            d[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 { break; }
        }
        self.s(&d[i..]);
    }
    fn get(&self) -> &[u8] {
        &self.b[..self.n]
    }
}

// "45m", "5h 45m", "123h"
#[inline(never)]
fn dur(secs: u32) -> S {
    let (h, m) = (secs / 3600, secs / 60 % 60);
    match h {
        0 => label!(n(m), s(b"m")),
        1..=99 => label!(n(h), s(b"h "), n(m), s(b"m")),
        _ => label!(n(h), s(b"h")),
    }
}

// No sessions means no meaningful average; retain seconds for short sessions.
#[inline(never)]
fn average_session(seconds: u32, sessions: u32) -> S {
    if sessions == 0 { return label!(s(b"--")); }
    let avg = seconds / sessions;
    if avg < 60 { label!(n(avg), s(b"s")) } else { dur(avg) }
}

#[inline(never)]
fn text(x: i32, y: i32, w: i32, h: i32, s: &[u8], font: i32, align: i32, flags: i32) {
    dsn_text_box(x, y, w, h, s, font, align, 1, flags);
}

// Same 14->12 fallback as the native dashboard when a metric is too wide.
// The ABI returns packed text width (high 16) and line height (low 16).
#[inline(never)]
fn metric_font(value: &[u8], primary: i32, fallback: i32, max_width: i32) -> i32 {
    let width = (dsn_measure_text(value, primary, BOLD) >> 16) as i32;
    if width <= max_width.max(0) { primary } else { fallback }
}

#[inline(never)]
fn hit(x:i32,y:i32,bounds:(i32,i32,i32,i32))->bool {
    let (bx,by,bw,bh)=bounds;
    x>=bx && x<bx+bw && y>=by && y<by+bh
}
impl Ui {
    #[inline(never)]
    fn modal_decision(&self,e:u32,label:&[u8])->u8 {
        match e&255 {
            1=>1,2=>2,
            5=>{
                let x=((e>>8)&0xfff) as i32;
                let y=((e>>20)&0xfff) as i32;
                let (yes,no,by,yw,nw,bh)=self.overall_sync_confirmation_buttons(label);
                if hit(x,y,(yes,by,yw,bh)){1}
                else if hit(x,y,(no,by,nw,bh)){2}
                else{0}
            },
            _=>0
        }
    }
}
struct Ui {
    w: i32,
    h: i32,
    ls: i32, // line heights: small, body, title. Everything is sized from these,
    lb: i32, // so the layout scales with the board's fonts.
    lt: i32,
    touch: bool,
    buttons: bool,
    native_hint_height: i32,
}

#[inline(never)]
fn sleep_capture(server_page: Option<bool>) -> bool {
    let mut path = [0u8; 192];
    let n = StatsSettings::path(&mut path, b"/sleep-frame.capture");
    if n == 0 { return false; }
    if let Some(server) = server_page {
        return dsn_fs_write(&path[..n], &[u8::from(server)]) == 0;
    }
    let mut capability = [0u8; 1];
    dsn_fs_read(&path[..n], &mut capability) == 1 && capability[0] == 1
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    checkpoint(201);
    let mut st = Stats {
        end: 0,
        day: [0; DAYS],
        hour: [0; 4],
        book_count: 0,
        total: 0,
        sessions: 0,
        progress_bp: 0,
        err: 0,
        log_source: true, native_stats: false, books_limited: false, week_hour: [0;4], date_known: false, date_current: false,
    };
    // dsn_clock_t: u16 year, month, day, ... in local time.
    let _ = dsn_timer_set(5000);
    let mut c = [0u8; 8];
    let mut year = if dsn_clock(&mut c) >= 0 { c[0] as u32 | (c[1] as u32) << 8 } else { 0 };
    let mut have_clock = year >= 2000 && (1..=12).contains(&c[2]) && (1..=31).contains(&c[3]);
    let mut session_probe = [0u8; 1];
    let have_sessions = dsn_fs_read_at(SESSION_LOG, 0, &mut session_probe) >= 0;

    // One invocation owns this buffer for its entire lifetime. Avoid moving
    // the 5 KiB Home value through the small guest stack.
    static mut HOME: Home = Home {
        rec: [0; 1100],
        f: [(0, 0); 5],
        percent: 0,
        cover: [0; 2304],
        cover_n: 0,
        shelf_index: 0,
    };
    let home = unsafe { &mut *core::ptr::addr_of_mut!(HOME) };
    home.percent = -1;
    home.load();
    if have_sessions { loading_history(); }
    let anchor = if have_clock { days_from_civil(year,c[2] as u32,c[3] as u32) } else { 0 };
    let use_native = native_available();
    let native_detail = if use_native {
        native_load(&mut st, &home.rec[home.f[0].0..home.f[0].1], anchor, &mut home.cover)
    } else { None };
    let (latest, current_id, book_count, has_baseline) = if have_sessions && !use_native {
        session_inventory(&home.rec[home.f[0].0..home.f[0].1], &mut home.cover)
    } else { ((0,0),[0;32],0,false) };
    if has_baseline { st.err=ERR_UNAVAILABLE; }
    if !use_native && !has_baseline { st.book_count=book_count; }
    st.date_current = have_clock;
    if !have_clock && !st.native_stats {
        let now = dsn_time() as i64;
        if valid_epoch(now) {
            let (y,m,d) = civil_from_days(((now + latest.1*60)/86400) as u32);
            year=y; c[2]=m as u8; c[3]=d as u8;
            have_clock=true; st.date_current=true;
        } else if valid_epoch(latest.0) {
            let (y,m,d)=civil_from_days(((latest.0+latest.1*60)/86400) as u32);
            year=y; c[2]=m as u8; c[3]=d as u8; have_clock=true;
        }
    }
    if have_clock { st.end=days_from_civil(year,c[2] as u32,c[3] as u32); }
    if !have_clock && st.end >= 10957 {
        let (y,m,d)=civil_from_days(st.end);
        year=y; c[2]=m as u8; c[3]=d as u8; have_clock=true;
    }
    st.date_known=have_clock;

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
        native_hint_height: (u16::from_le_bytes([device[22], device[23]]) as i32).min((scr & 0xffff) as i32),
    };
    // The cover buffer is shared by the two mutually exclusive hero pages.
    let mut current_cover_loaded = false;

    let detail = if let Some(detail) = native_detail { detail }
        else if !use_native && !has_baseline { load_sessions(&mut st, &home.rec[home.f[0].0..home.f[0].1], &current_id, &mut home.cover) }
        else { Current {found:false,secs:0,sessions:0,turns:0,active:0,periods:[0;4]} };
    let month_day = if have_clock { c[3] as u32 } else { 0 };
    // One guest invocation owns this storage; avoid a large constructor copy on the stack.
    static mut CAROUSEL: BookCarousel = BookCarousel::new();
    let carousel = unsafe { &mut *core::ptr::addr_of_mut!(CAROUSEL) };
    carousel.load(&mut home.rec);
    let mut carousel_record=true;
    let mut pairing = Pairing::new();
    checkpoint(202);
    pairing.load_web_login();
    checkpoint(203);
    let mut server = ServerOverall::new();
    if pairing.phase == PairPhase::Paired { server.load_cached(); }
    let mut page = if pairing.phase == PairPhase::Paired { 0 } else { 1 };
    let mut disconnect_failed = false;
    checkpoint(204);
    let mut settings = StatsSettings::load();
    let upload_supported = history_supported();
    let mut confirm_history_upload = false;
    let mut uploading_history = false;
    let mut history_message: &[u8] = b"";
    let mut history_refresh = None;
    checkpoint(205);

    let sleep_capture_supported = sleep_capture(None);
    let mut capture_sleep = sleep_capture_supported && settings.stats_on_sleep;

    let mut confirm_month_sync = false;
    let mut confirm_web_login = false;
    let mut generating_web_login = false;
    let mut confirm_pairing = false;
    let mut generating_pairing = false;
    let mut confirm_auto_sync = false;
    let mut was_paired = pairing.phase == PairPhase::Paired;
    let mut confirm_disconnect = false;
    let mut disconnecting = false;
    let mut syncing_month = settings.auto_fetch &&
        pairing.phase == PairPhase::Paired && server.stale(dsn_time());
    let mut show_server_after_refresh = false;
    let mut sync_focus = 0u8;
    let mut settings_focus = 0u8;
    let mut previous_page = -1;
    let mut book_offset = 0u32;
    let mut calendar_offset = 0u32; // months before the current month
    loop {
        let paired = pairing.phase == PairPhase::Paired;
        if paired && !was_paired {
            server = ServerOverall::new();
            // Initial account snapshot is part of successful pairing, independent of Auto Sync.
            show_server_after_refresh = true;
            syncing_month = true;
        }
        was_paired = paired;
        let first_page = if paired { 0 } else { 1 };
        if page == 0 && first_page == 1 { page = 1; }
        if page == 7 && previous_page != 7 {
            settings_focus = settings_default(paired, upload_supported);
        }
        previous_page = page;
        if capture_sleep && !syncing_month && !uploading_history && !confirm_history_upload {
            let use_server = paired && server.available;
            if use_server {
                current_cover_loaded = false;
                if !carousel_record {carousel.select(carousel.selected,&mut home.rec);carousel_record=true;}
                ui.overall_preview(carousel, &home.rec, &server, true, false, &mut home.cover);
            } else {
                ui.dashboard_render(&st, &home, &detail, 0, month_day, have_clock);
            }
            sleep_capture(Some(use_server));
            capture_sleep = false;
        }
        if disconnecting || (page == 6 && confirm_disconnect) {
            ui.sync_unpair_confirmation(disconnecting);
        } else if confirm_history_upload || uploading_history {
            ui.network_dialog(7, if uploading_history {b"Uploading reading history..."} else {b"Upload this reader's history?"},
                if uploading_history {b"Uploading and refreshing server statistics."} else {b"Upload only this reader's own history, never copied history. Server statistics will also refresh."}, uploading_history);
        } else if confirm_pairing || generating_pairing {
            ui.network_dialog(6, if generating_pairing { b"Generating pairing code..." } else { b"Generate a pairing code?" },
                if generating_pairing { b"Connecting to Wi-Fi. Please wait." } else { b"The reader will connect to Wi-Fi." }, generating_pairing);
        } else if confirm_auto_sync {
            ui.network_dialog(7, b"Enable Auto Sync?", b"The reader will use Wi-Fi when server statistics need updating.", false);
        } else if confirm_web_login || generating_web_login {
            ui.network_dialog(6, if generating_web_login { b"Generating web login..." } else { b"Generate a web login?" },
                if generating_web_login { b"Connecting to Wi-Fi. Please wait." } else { b"The reader will connect to Wi-Fi." }, generating_web_login);
        } else if confirm_month_sync {
            ui.overall_sync_confirmation();
        } else if syncing_month && page != 0 {
            ui.network_dialog(page, b"Syncing...", b"Connecting to Wi-Fi. Please wait.", true);
        } else if page == 0 {
            current_cover_loaded = false;
            checkpoint(206);
            if !carousel_record {carousel.select(carousel.selected,&mut home.rec);carousel_record=true;}
            ui.overall_preview(carousel, &home.rec, &server,
                               pairing.phase == PairPhase::Paired, syncing_month, &mut home.cover);
        } else if page == 7 {
            ui.settings_page(&settings, &server, paired, settings_focus, sleep_capture_supported, upload_supported, history_message, history_refresh);
        } else if page == 6 {
            ui.sync_page(&pairing, sync_focus, disconnect_failed);
        } else if page <= 2 {
            if page == 2 && !current_cover_loaded {
                if carousel_record {home.reload();carousel_record=false;}
                home.cover_n=0;
                if home.has_book() {
                let cover_limit = ((ui.content_bottom() - (13 + ui.ls + ui.lt) - 33) * 35 / 100 - 20).clamp(40, 150);
                let (start, end) = home.f[0];
                let n = dsn_book_cover(&home.rec[start..end], cover_limit, &mut home.cover);
                home.cover_n = if n > 4 { (n as usize).min(home.cover.len()) } else { 0 };
                }
                current_cover_loaded = true;
            }
            ui.dashboard_render(&st, &home, &detail, page - 1, month_day, have_clock);
        } else {
            if page == 5 { current_cover_loaded = false; }
            let books_ready = if page == 5 && st.native_stats {
                current_cover_loaded = false;
                native_ensure_books(st.end, &mut home.cover)
            } else { true };
            dsn_clear();
            ui.header(page);
            let month_number = if have_clock { year * 12 + c[2] as u32 - 1 - calendar_offset } else { 0 };
            let shown_year = if have_clock { month_number / 12 } else { 0 };
            let shown_month = if have_clock { month_number % 12 + 1 } else { 0 };
            let shown_day = if calendar_offset == 0 && st.date_current { month_day } else { 0 };
            ui.extra_page(&st, page - 1, shown_year, shown_month, shown_day, book_offset, books_ready, &mut home.cover);
        }
        checkpoint(207);
        if !disconnecting && !generating_web_login && !generating_pairing && !syncing_month && !uploading_history {
            ui.draw_footer(page, carousel.count, page == 6 && pairing.phase == PairPhase::Paired,
                           confirm_disconnect || confirm_web_login || confirm_month_sync || confirm_pairing || confirm_auto_sync || confirm_history_upload);
        }
        checkpoint(208);
        dsn_refresh();
        checkpoint(209);
        if uploading_history {
            uploading_history = false;
            current_cover_loaded = false;
            // The cover holds the upload request and then the receipt; the
            // inactive shelf record holds parser scratch. Reload the selected
            // shelf row before drawing or opening a book.
            let selection = carousel.selected;
            history_message = upload_history(&pairing, &mut server, &mut home.cover, &mut home.rec, &mut history_refresh);
            let _ = dsn_reading_stats_read(4,0,&mut []);
            carousel.select(selection,&mut home.rec);
            carousel_record=true;
            if history_refresh == Some(true) &&
                (history_message == HISTORY_OK || history_message == HISTORY_ALREADY_UPLOADED) {
                page = 0;
            }
            capture_sleep = sleep_capture_supported && settings.stats_on_sleep;
            continue;
        }
        if disconnecting {
            disconnecting = false;
            history_message = b"";
            history_refresh = None;
            disconnect_failed = !pairing.disconnect();
            if !disconnect_failed {
                server = ServerOverall::new();
                sync_focus = 0;
                capture_sleep = sleep_capture_supported && settings.stats_on_sleep;
            }
            continue;
        }
        if generating_pairing {
            generating_pairing = false;
            pairing.begin();
            continue;
        }
        if generating_web_login {
            generating_web_login = false;
            pairing.request_web_login();
            continue;
        }
        // The confirmation is painted first. Only after a second, explicit
        // confirmation do we display Syncing... before the firmware reboot.
        if syncing_month {
            syncing_month = false;
            current_cover_loaded=false;
            let refreshed = server.fetch(&pairing,&mut home.cover);
            if show_server_after_refresh {
                show_server_after_refresh = false;
                if refreshed { page = 0; }
                else if page == 6 { pairing.message = b"Paired. Server refresh failed."; }
                else if page == 7 {
                    history_message = b"Server refresh failed. Retry.";
                    history_refresh = None;
                }
            }
            capture_sleep = sleep_capture_supported && settings.stats_on_sleep;
            continue;
        }

        loop {
            // First repaint the "Requesting/Checking" state. Run the blocking
            // network syscall only after Dessen has queued that display update.
            if pairing.pending {
                pairing.activate();
                break;
            }
            let e = dsn_wait_event();
            if confirm_disconnect || confirm_history_upload || confirm_pairing ||
                confirm_auto_sync || confirm_web_login || confirm_month_sync {
                let label=if confirm_disconnect {b"Disconnect".as_slice()}
                    else if confirm_month_sync {b"Sync now"}else{b"Continue"};
                let decision=ui.modal_decision(e,label);
                if decision==0 {continue;}
                if decision==1 {
                    if confirm_disconnect {disconnecting=true;}
                    else if confirm_history_upload {uploading_history=true;}
                    else if confirm_pairing {generating_pairing=true;}
                    else if confirm_web_login {generating_web_login=true;}
                    else if confirm_month_sync {show_server_after_refresh=true;syncing_month=true;}
                    else {
                        settings.toggle_flag(false);
                        syncing_month=settings.auto_fetch && paired && server.stale(dsn_time());
                    }
                }
                confirm_disconnect=false;
                confirm_history_upload=false;
                confirm_pairing=false;
                confirm_auto_sync=false;
                confirm_web_login=false;
                confirm_month_sync=false;
                break;
            }
            match e & 0xff {
                1 if page == 7 => {
                    if settings_focus == 0 {
                        if settings.auto_fetch { settings.toggle_flag(false); } else { confirm_auto_sync = true; }
                    }
                    else if settings_focus == 1 && sleep_capture_supported {
                        settings.toggle_flag(true);
                        capture_sleep = settings.stats_on_sleep;
                    }
                    else if settings_focus == 3 && upload_supported && paired { confirm_history_upload = true; }
                    else if paired { confirm_month_sync = true; }
                    else { page = 6; }
                    break;
                }
                1 if page == 0 => {
                    if let Some(book) = carousel.current(&home.rec) { dsn_open_book(book.field(&home.rec,0)); }
                }
                1 if page == 2 && home.has_book() => { dsn_open_book(home.field(0)); }
                1 if page == 6 => {
                    if pairing.phase == PairPhase::Paired {
                        match sync_focus {
                            0 => confirm_web_login = true,
                            1 => { confirm_month_sync = true; }
                            _ => confirm_disconnect = true,
                        }
                    } else if pairing.phase == PairPhase::Code { pairing.schedule(); }
                    else { confirm_pairing = true; }
                    break;
                }
                11 if page == 6 && pairing.phase == PairPhase::Paired && pairing.web_valid => {
                    if !pairing.web_login_available() {
                        pairing.web_valid = false;
                        pairing.message = b"Web PIN expired. Generate a new one";
                        break;
                    }
                }
                11 if page == 6 && pairing.phase == PairPhase::Code => {
                    // Firmware's five-second wake event. Check quietly without
                    // two full e-ink refreshes per poll; repaint only when the
                    // visible status/phase actually changes.
                    let previous_message = pairing.message;
                    pairing.poll();
                    if pairing.phase != PairPhase::Code || pairing.message != previous_message {
                        break;
                    }
                }
                2 => return 0,
                3 | 4 | 7 | 8 | 9 | 10 => {
                    let kind = e & 0xff;
                    let forward = matches!(kind,4|7|9);
                    let front = matches!(kind,3|4) && e & SIDE_KEY == 0;
                    if front && page == 7 {
                        settings_focus = settings_step(settings_focus, forward, sleep_capture_supported, upload_supported && paired);
                    } else if front && page == 6 && paired {
                        sync_focus = (sync_focus + if forward {1} else {2}) % 3;
                    } else if page == 0 && carousel.count > 1 &&
                        (front || (matches!(kind,9|10) && ui.overall_carousel_contains(
                            ((e >> 8) & 0xfff) as i32, ((e >> 20) & 0xfff) as i32))) {
                        carousel.step(if forward {1} else {-1},&mut home.rec);
                    } else {
                        let cap = ui.books_capacity();
                        if page == 5 && forward && book_offset + cap < st.book_count {book_offset += cap;}
                        else if page == 5 && !forward && book_offset >= cap {book_offset -= cap;}
                        else if forward {page=next_page(page); if page==0 {page=first_page;}}
                        else {page=prev_page(page); if page==0 && first_page==1 {page=6;}}
                    }
                    break;
                }

                5 => {
                    // Tap the current-book hero to resume reading; elsewhere change page.
                    let x = ((e >> 8) & 0xfff) as i32;
                    let y = ((e >> 20) & 0xfff) as i32;
                    if x < G + 55 && y < 13 + ui.ls + ui.lt {
                        return 0;
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
                            if let Some(book) = carousel.current(&home.rec) { dsn_open_book(book.field(&home.rec,0)); }
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
                        let (rx, ry, rw, rh) = ui.settings_bounds(2);
                        let (sx, sy, sw, sh) = ui.settings_bounds(1);
                        let (hx,hy,hw,hh) = ui.settings_bounds(3);
                        if upload_supported && paired && x >= hx && x < hx+hw && y >= hy && y < hy+hh {
                            confirm_history_upload = true;
                        }
                        else if sleep_capture_supported && x >= sx && x < sx+sw && y >= sy && y < sy+sh {
                            settings.toggle_flag(true);
                            capture_sleep = settings.stats_on_sleep;
                        }
                        else if x >= bx && x < bx+bw && y >= by && y < by+bh {
                            if settings.auto_fetch { settings.toggle_flag(false); } else { confirm_auto_sync = true; }
                        }
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
                            if pairing.phase == PairPhase::Paired {
                                for action in [0, 2] {
                                    let (bx, by, bw, bh) = ui.sync_action_bounds(action, Ui::sync_action_label(&pairing, action));
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
                                if pairing.phase == PairPhase::Code { pairing.schedule(); }
                                else { confirm_pairing = true; }
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
