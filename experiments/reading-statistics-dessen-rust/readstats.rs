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

include!("build/dessen_compact.rs");
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
fn next_page(page: i32) -> i32 { match page { 5 => 7, 7 => 0, 6 => 7, _ => page + 1 } }
fn prev_page(page: i32) -> i32 { match page { 0 => 7, 6 => 7, 7 => 5, _ => page - 1 } }

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

fn shelf_progress(field: &[u8], path: &[u8]) -> i32 {
    if !field.is_empty() && field.len() <= 3 && field.iter().all(u8::is_ascii_digit) {
        let mut value=0;
        for &b in field {value=value*10+(b-b'0') as i32;}
        if value<=100{return value;}
    }
    dsn_book_progress(path)
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
                let p = shelf_progress(self.field(3), self.field(0));
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
    day: [u32; 31], // current month and last week only; older days are read on demand
    latest_secs:u32,
    latest_ago:u32,
    streak:u32,
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
    owner: [u8;16],
}

#[inline(never)]
fn le32(data: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([data[off], data[off+1], data[off+2], data[off+3]])
}
const DAY_BYTES:usize=DAYS*4;
impl Stats {
    #[inline(never)]
    fn finish_days(&mut self,days:&[u8]) {
        for i in 0..31 {self.day[i]=le32(days,(DAYS-31+i)*4);}
        for ago in 0..DAYS {
            let v=le32(days,(DAYS-1-ago)*4);
            if v>0 {self.latest_secs=v;self.latest_ago=ago as u32;break;}
        }
        let skip=usize::from(self.day[30]==0);
        self.streak=0;
        for ago in skip..DAYS {
            if le32(days,(DAYS-1-ago)*4)==0{break;}
            self.streak+=1;
        }
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
    rich_books: bool,
    sleep_render: bool,
}

include!("sleep.rs");
include!("book_filters.rs");

#[no_mangle]
pub extern "C" fn main() -> i32 {
    checkpoint(201);
    let mut st = Stats {
        end: 0,
        day: [0; 31], latest_secs:0, latest_ago:0, streak:0,
        hour: [0; 4],
        book_count: 0,
        total: 0,
        sessions: 0,
        progress_bp: 0,
        err: 0,
        log_source: true, native_stats: false, books_limited: false, week_hour: [0;4], date_known: false, date_current: false, owner:[0;16],
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
            year=y;
            c[2]=m as u8; c[3]=d as u8; have_clock=true;
        }
    }
    if have_clock { st.end=days_from_civil(year,c[2] as u32,c[3] as u32); }
    if !have_clock && st.end >= 10957 {
        let (_,m,d)=civil_from_days(st.end);
        c[2]=m as u8; c[3]=d as u8; have_clock=true;
    }
    st.date_known=have_clock;

    let scr = dsn_screen();
    let mut device = [0u8; 24];
    let device_n = dsn_device(&mut device);
    let touch = device_n >= 20 && device[18] != 0;
    let buttons = device_n >= 20 && device[19] != 0;
    let mut ui = Ui {
        w: (scr >> 16) as i32,
        h: (scr & 0xffff) as i32,
        ls: dsn_line_height(SMALL),
        lb: dsn_line_height(BODY),
        lt: dsn_line_height(TITLE),
        touch,
        buttons,
        native_hint_height: (u16::from_le_bytes([device[22], device[23]]) as i32).min((scr & 0xffff) as i32),
        rich_books: dsn_reading_stats_read(9, 0, &mut []) == 1,
        sleep_render:false,
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
    pairing.load_web_login(&mut home.cover);
    let mut server = ServerOverall::new();
    if pairing.phase == PairPhase::Paired { server.load_cached(&mut home.cover); }
    let mut page = if pairing.phase == PairPhase::Paired { 0 } else { 1 };
    let mut settings = StatsSettings::load();
    let upload_supported = history_supported();
    let mut confirm_history_upload = false;
    let mut uploading_history = false;
    let mut history_message: &[u8] = b"";
    let mut history_refresh = None;

    let sleep_capture_supported = dsn_screenshot(&[]) == 1;
    let mut choose_sleep=false;
    if settings.sleep_page>5 && sleep_capture_supported {
        if sleep_publish(false,0,&mut home.cover){settings.save_sleep(0);}
        else{settings.save_failed=true;}
    }
    let mut sleep_focus=settings.sleep_page.min(5);
    let mut capture_sleep = sleep_capture_supported && (1..=5).contains(&settings.sleep_page);
    if sleep_capture_supported && sleep_account_changed(if pairing.phase==PairPhase::Paired{hist_hash(&pairing.access_token)}else{0}){
        // Root wallpaper has no app-aware firmware routing. Reconcile on launch
        // before using a potentially stale account snapshot.
        let restored=sleep_publish(false,0,&mut home.cover);
        server=ServerOverall::new();
        pairing.web_valid=false;
        clear_account_cache();
        if !restored{settings.save_failed=true;capture_sleep=false;}
    }

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
    let mut settings_focus = 0u8;
    let mut previous_page = -1;
    let mut book_offset = 0u32;
    let mut book_selected=0u32;
    let mut book_confirm=false;
    let mut book_action=0u8;
    let mut book_filter=BookFilter::new();
    let manage_books=ui.rich_books && st.native_stats;
    let mut book_error=false;
    let mut calendar_all = true;
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
        if page!=5 {if previous_page==5 && manage_books {carousel.select(carousel.selected,&mut home.rec);carousel_record=true;} book_filter.ready=false;}
        previous_page = page;
        let capture = !book_error && !book_confirm && !confirm_disconnect && !confirm_pairing && !confirm_web_login && !confirm_auto_sync && !confirm_month_sync && capture_sleep && !syncing_month && !uploading_history && !confirm_history_upload &&
            !choose_sleep;
        let restore_page=page;
        let restore_month=calendar_offset;
        if capture {
            ui.sleep_render=true;
            page=i32::from(settings.sleep_page.saturating_sub(1));
            if page==0 && (!paired || !server.available){page=1;}
            calendar_offset=0;
            current_cover_loaded=false;
        }
        if book_error {
            ui.network_dialog(5,b"Book unavailable",b"Cannot open book or save book list.",false,b"Continue");
        } else if book_confirm && book_action!=4 {
            ui.book_actions(&home.cover,book_action,book_filter.hidden);
        } else if book_confirm {
            let n=home.cover[10].min(80) as usize;
            ui.network_dialog(5,b"Open Book?",&home.cover[12..12+n],false,b"Continue");
        } else if choose_sleep {
            ui.sleep_picker(sleep_focus,paired);
        } else if disconnecting || confirm_disconnect {
            ui.sync_unpair_confirmation(disconnecting);
        } else if confirm_history_upload || uploading_history {
            ui.network_dialog(7, if uploading_history {b"Uploading reading history..."} else {b"Upload this reader's history?"},
                if uploading_history {b"Uploading and refreshing server statistics."} else {b"Upload only this reader's own history, never copied history. Server statistics will also refresh."}, uploading_history,b"Continue");
        } else if confirm_pairing || generating_pairing {
            ui.network_dialog(6, if generating_pairing { b"Generating pairing code..." } else { b"Generate a pairing code?" },
                if generating_pairing { b"Connecting to Wi-Fi. Please wait." } else { b"The reader will connect to Wi-Fi." }, generating_pairing,b"Continue");
        } else if confirm_auto_sync {
            ui.network_dialog(7, b"Enable Auto Sync?", b"The reader will use Wi-Fi when server statistics need updating.", false,b"Continue");
        } else if confirm_web_login || generating_web_login {
            ui.network_dialog(6, if generating_web_login { b"Generating web login..." } else { b"Generate a web login?" },
                if generating_web_login { b"Connecting to Wi-Fi. Please wait." } else { b"The reader will connect to Wi-Fi." }, generating_web_login,b"Continue");
        } else if confirm_month_sync {
            ui.overall_sync_confirmation();
        } else if syncing_month && page != 0 {
            ui.network_dialog(page, b"Syncing...", b"Connecting to Wi-Fi. Please wait.", true,b"Continue");
        } else if page == 0 {
            current_cover_loaded = false;
            checkpoint(206);
            if !carousel_record {carousel.select(carousel.selected,&mut home.rec);carousel_record=true;}
            ui.overall_preview(carousel, &home.rec, &server,
                               pairing.phase == PairPhase::Paired, syncing_month, &mut home.cover);
        } else if page == 7 {
            ui.settings_page(&settings, &server, paired, settings_focus, sleep_capture_supported, upload_supported, history_message, history_refresh,manage_books);
        } else if page == 6 {
            ui.sync_page(&pairing);
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
            let books_ready = if matches!(page,4|5) && st.native_stats {
                current_cover_loaded = false;
                native_ensure_books(st.end, &mut home.cover)
            } else { true };
            if page==5 && manage_books && books_ready && !book_filter.ready{
                if !book_filter.load(st.book_count,&mut home.cover,&mut home.rec[..128]){book_filter.excluded=0;book_error=true;continue;}
                let count=book_filter.count(st.book_count);
                book_selected=book_selected.min(count.saturating_sub(1));
                book_offset=(book_selected/ui.books_capacity())*ui.books_capacity();
            }
            dsn_clear();
            ui.header(if page==5 && book_filter.hidden{8}else{page});
            // With no reliable RTC, the validated offline server snapshot can
            // supply the month for All Devices even if this reader has no dates.
            let mut calendar_base=if have_clock {st.end}else{0};
            if page==4 && paired && calendar_all && !st.date_current &&
                cached_calendar(&pairing,&mut home.cover)!=0 {
                calendar_base=calendar_base.max(le32(&home.cover,4));
            }
            let (cy,cm,_)=if calendar_base>0 {civil_from_days(calendar_base)}else{(0,0,0)};
            let month_number=if cy>0 {cy*12+cm-1-calendar_offset}else{0};
            let shown_year = if cy>0 { month_number / 12 } else { 0 };
            let shown_month = if cy>0 { month_number % 12 + 1 } else { 0 };
            let shown_day = if calendar_offset == 0 && st.date_current { month_day } else { 0 };
            if page==4 {
                let mut local_days=[0u8;124];
                if shown_month!=0 {
                    let first=days_from_civil(shown_year,shown_month,1);
                    let count=days_in_month(shown_year,shown_month);
                    if st.native_stats {
                        if books_ready && first<=st.end {
                            let low=first.max(st.end.saturating_sub(DAYS as u32-1));
                            let n=(first+count).min(st.end+1).saturating_sub(low) as usize;
                            if n>0 && dsn_reading_stats_read(2,low,&mut home.cover[..n*16])==(n*16) as i32 {
                                for i in 0..n {
                                    let j=(low-first) as usize+i;
                                    local_days[j*4..j*4+4].copy_from_slice(&native_u32(&home.cover,i*16).to_le_bytes());
                                }
                            }
                        }
                    }else if st.err!=ERR_UNAVAILABLE {
                        stock_calendar(first,count,&mut local_days,&mut home.cover);
                    }
                }
                let size=if paired && calendar_all {cached_calendar(&pairing,&mut home.cover)}else{0};
                current_cover_loaded=false;
                ui.calendar_page(&st,shown_year,shown_month,shown_day,paired && calendar_all,&home.cover[..size],&local_days);
            }else{
                ui.extra_page(&st, page - 1, book_offset,book_selected, books_ready,&book_filter,&home.rec[..128], &mut home.cover);
            }
        }
        if capture {
            let ok=sleep_publish(true,if paired{hist_hash(&pairing.access_token)}else{0},&mut home.cover);
            settings.save_failed=!ok;
            ui.sleep_render=false;
            page=restore_page;calendar_offset=restore_month;
            current_cover_loaded=false;
            capture_sleep=false;
            continue;
        }
        checkpoint(207);
        if !disconnecting && !generating_web_login && !generating_pairing && !syncing_month && !uploading_history {
            ui.draw_footer(if page==5 && book_filter.hidden{8}else{page}, carousel.count, paired, choose_sleep || (book_confirm && book_action!=4),
                           book_error || (book_confirm && book_action==4) || confirm_disconnect || confirm_web_login || confirm_month_sync || confirm_pairing || confirm_auto_sync || confirm_history_upload);
        }
        dsn_refresh();
        if uploading_history {
            uploading_history = false;
            current_cover_loaded = false;
            // The cover holds the upload request and then the receipt; the
            // inactive shelf record holds parser scratch. Reload the selected
            // shelf row before drawing or opening a book.
            let selection = carousel.selected;
            history_message = upload_history(&pairing, &mut server, &mut home.cover, &mut home.rec, &mut history_refresh,&mut st.owner);
            if history_refresh==Some(true) {server.fetch_calendar(&pairing,&mut home.cover);}
            let _ = dsn_reading_stats_read(4,0,&mut []);
            carousel.select(selection,&mut home.rec);
            carousel_record=true;
            if history_refresh == Some(true) &&
                (history_message == HISTORY_OK || history_message == HISTORY_ALREADY_UPLOADED) {
                page = 0;
            }
            capture_sleep = sleep_capture_supported && (1..=5).contains(&settings.sleep_page);
            continue;
        }
        if disconnecting {
            disconnecting = false;
            history_message = b"";
            history_refresh = None;
            if sleep_capture_supported && !sleep_publish(false,0,&mut home.cover){settings.save_failed=true;}
            let disconnect_failed = !pairing.disconnect();
            if !disconnect_failed {
                server = ServerOverall::new();
                page=7;
                capture_sleep = sleep_capture_supported && (1..=5).contains(&settings.sleep_page);
            }
            if disconnect_failed{history_message=b"Could not disconnect. Retry.";}
            continue;
        }
        if generating_pairing {
            generating_pairing = false;
            current_cover_loaded=false; pairing.begin(&mut home.cover);
            continue;
        }
        if generating_web_login {
            generating_web_login = false;
            current_cover_loaded=false; pairing.request_web_login(&mut home.cover);
            continue;
        }
        // The confirmation is painted first. Only after a second, explicit
        // confirmation do we display Syncing... before the firmware reboot.
        if syncing_month {
            syncing_month = false;
            current_cover_loaded=false;
            let refreshed = server.fetch(&pairing,&mut home.cover);
            if refreshed {server.fetch_calendar(&pairing,&mut home.cover);}
            if show_server_after_refresh {
                show_server_after_refresh = false;
                if refreshed { page = 0; }
                else if page == 6 { pairing.message = b"Paired. Server refresh failed."; }
                else if page == 7 {
                    history_message = b"Server refresh failed. Retry.";
                    history_refresh = None;
                }
            }
            capture_sleep = sleep_capture_supported && (1..=5).contains(&settings.sleep_page);
            continue;
        }

        loop {
            // First repaint the "Requesting/Checking" state. Run the blocking
            // network syscall only after Dessen has queued that display update.
            if pairing.pending {
                current_cover_loaded=false; pairing.activate(&mut home.cover);
                break;
            }
            let e = dsn_wait_event();
            if book_error {
                if ui.modal_decision(e,b"Continue")==0{continue;}
                book_error=false;break;
            }
            if book_confirm && book_action!=4{
                let k=e&255;let count=if book_filter.hidden{2}else{4};
                if k==2{book_confirm=false;break;}
                if matches!(k,3|4|7|8){
                    book_action=(book_action+if matches!(k,4|7){1}else{count-1})%count;
                    break;
                }
                let mut apply=k==1;
                if k==5{
                    for i in 0..count{
                        if hit(((e>>8)&0xfff) as i32,((e>>20)&0xfff) as i32,ui.book_option(i)){
                            book_action=i;apply=true;break;
                        }
                    }
                }
                if !apply{continue;}
                if book_action==0 && !book_filter.hidden{book_action=4;break;}
                if (book_filter.hidden && book_action==0) || (!book_filter.hidden && matches!(book_action,1|2)){
                    let mode=if book_filter.hidden{0}else{book_action};
                    let index=book_filter.index(book_selected,st.book_count,&home.rec[..128]);
                    if !book_filter.change(index,mode,&mut home.cover){book_error=true;}
                }
                book_confirm=false;break;
            }
            if book_confirm {
                let decision=ui.modal_decision(e,b"Continue");
                if decision==0{continue;}
                if decision==1{
                    let path=&home.cover[92..268];
                    let n=path.iter().position(|&v|v==0).unwrap_or(path.len());
                    if n==0 || dsn_open_book(&path[..n])<0{book_error=true;}
                }
                book_confirm=false;break;
            }
            if choose_sleep {
                let k=e&255;
                if k==2{choose_sleep=false;break;}
                if matches!(k,3|4|7|8){
                    loop{
                        sleep_focus=(sleep_focus+if matches!(k,4|7){1}else{5})%6;
                        if paired || sleep_focus!=1{break;}
                    }
                    break;
                }
                let mut apply=k==1;
                if k==5{
                    for i in 0..6{
                        if (i!=1 || paired) && hit(((e>>8)&0xfff) as i32,((e>>20)&0xfff) as i32,ui.sleep_option(i)){
                            sleep_focus=i;apply=true;break;
                        }
                    }
                }
                if apply {
                    if sleep_focus!=0 || sleep_publish(false,0,&mut home.cover){
                        settings.save_sleep(sleep_focus);
                        capture_sleep=settings.sleep_page!=0;
                    }else{settings.save_failed=true;}
                    choose_sleep=false;break;
                }
                continue;
            }
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
            if page==7 && matches!(e&255,1|5){
                let mut row=settings_focus;
                if e&255==5{
                    let x=((e>>8)&0xfff) as i32;let y=((e>>20)&0xfff) as i32;
                    if x<G+55 && y<13+ui.ls+ui.lt{return 0;}
                    row=255;
                    for r in SETTINGS_ORDER{
                        if settings_visible(r,sleep_capture_supported,upload_supported,paired,manage_books)&&
                            hit(x,y,ui.settings_bounds(r,sleep_capture_supported,upload_supported,paired,manage_books)){
                            row=r;break;
                        }
                    }
                    if row==255{continue;}
                }
                match row{
                    0=>if settings.auto_fetch{settings.toggle_flag(false);}else{confirm_auto_sync=true;},
                    1=>{sleep_focus=if settings.sleep_page==1&&!paired{2}else{settings.sleep_page.min(5)};choose_sleep=true;},
                    3=>confirm_history_upload=true,
                    2=>if paired{confirm_month_sync=true;}else{page=6;confirm_pairing=true;},
                    4=>{page=6;if !pairing.web_login_available(){confirm_web_login=true;}},
                    5=>confirm_disconnect=true,
                    6=>{page=5;book_filter.hidden=true;book_filter.ready=false;book_offset=0;book_selected=0;},
                    _=>{}
                }
                break;
            }
            if page==5 && ui.rich_books && st.native_stats && matches!(e&255,1|5){
                let mut selected=book_selected;
                if e&255==5{
                    let x=((e>>8)&0xfff) as i32;let y=((e>>20)&0xfff) as i32;
                    if x<G+55&&y<13+ui.ls+ui.lt{if book_filter.hidden{page=7;book_filter.hidden=false;break;}return 0;}
                    let top=13+ui.ls+ui.lt+ui.lb+20;
                    if x<G || x>=ui.w-G || y<top || y>=top+ui.books_capacity() as i32*ui.book_row_height(){continue;}
                    selected=book_offset+((y-top)/ui.book_row_height()) as u32;
                }
                let index=book_filter.index(selected,st.book_count,&home.rec[..128]);
                if index<st.book_count && native_ensure_books(st.end,&mut home.cover) &&
                    dsn_reading_stats_read(9,index,&mut home.cover[..364])==364 {
                    book_selected=selected;book_action=0;book_confirm=true;
                }
                break;
            }
            match e & 0xff {
                1 if page == 4 && paired => {calendar_all=!calendar_all;calendar_offset=0;break;}
                1 if page == 0 => {
                    if let Some(book) = carousel.current(&home.rec) { dsn_open_book(book.field(&home.rec,0)); }
                }
                1 if page == 2 && home.has_book() => { dsn_open_book(home.field(0)); }
                1 if page == 6 => {
                    if paired {confirm_web_login=true;}
                    else if pairing.phase==PairPhase::Code{pairing.schedule();}
                    else{confirm_pairing=true;}
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
                    current_cover_loaded=false; pairing.poll(&mut home.cover);
                    if pairing.phase != PairPhase::Code || pairing.message != previous_message {
                        break;
                    }
                }
                2 if page==5 && book_filter.hidden => {page=7;book_filter.hidden=false;book_offset=0;book_selected=0;break;}
                2 if page==6 => {page=7;break;}
                2 => return 0,
                3 | 4 | 7 | 8 | 9 | 10 => {
                    let kind = e & 0xff;
                    let forward = matches!(kind,4|7|9);
                    let front = matches!(kind,3|4) && e & SIDE_KEY == 0;
                    if front && page == 7 {
                        settings_focus = settings_step(settings_focus,forward,sleep_capture_supported,upload_supported,paired,manage_books);
                    } else if page == 4 && (have_clock || (paired && calendar_all)) &&
                        (front || (matches!(kind,9|10) && hit(((e>>8)&0xfff) as i32,((e>>20)&0xfff) as i32,ui.calendar_bounds()))) {
                        let newer=forward; // Left swipe and physical Next advance toward the current month.
                        if newer && calendar_offset==0 && !front{page=5;}
                        else{calendar_offset=if newer{calendar_offset.saturating_sub(1)}else{(calendar_offset+1).min(11)};}
                    } else if page==5 && front && ui.rich_books && st.native_stats && book_filter.count(st.book_count)>0 {
                        book_selected=if forward{(book_selected+1).min(book_filter.count(st.book_count)-1)}else{book_selected.saturating_sub(1)};
                        let cap=ui.books_capacity();
                        book_offset=(book_selected/cap)*cap;
                    } else if page==6 {page=7;
                    } else if page == 0 && carousel.count > 1 &&
                        (front || (matches!(kind,9|10) && ui.overall_carousel_contains(
                            ((e >> 8) & 0xfff) as i32, ((e >> 20) & 0xfff) as i32))) {
                        carousel.step(if forward {1} else {-1},&mut home.rec);
                    } else {
                        let cap = ui.books_capacity();
                        if page == 5 && forward && book_offset + cap < book_filter.count(st.book_count) {book_offset += cap;book_selected=book_offset;}
                        else if page == 5 && !forward && book_offset >= cap {book_offset -= cap;book_selected=book_offset;}
                        else if page==5 && book_filter.hidden {page=7;book_filter.hidden=false;book_selected=0;book_offset=0;}
                        else if forward {page=next_page(page); if page==0 {page=first_page;}}
                        else {page=prev_page(page); if page==0 && first_page==1 {page=7;}}
                    }
                    break;
                }

                5 => {
                    // Tap the current-book hero to resume reading; elsewhere change page.
                    let x = ((e >> 8) & 0xfff) as i32;
                    let y = ((e >> 20) & 0xfff) as i32;
                    if x < G + 55 && y < 13 + ui.ls + ui.lt {
                        if page==6{page=7;break;}
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
                    } else if page == 4 && hit(x,y,ui.calendar_bounds()) {
                        let top=ui.calendar_bounds().1+ui.lb+5;
                        if paired && y>=top && y<top+ui.ls {
                            calendar_all=!calendar_all;calendar_offset=0;
                            break;
                        }
                        continue;
                    } else if page==6 {
                        if hit(x,y,ui.sync_button_bounds()){
                            if paired{confirm_web_login=true;}
                            else if pairing.phase==PairPhase::Code{pairing.schedule();}
                            else{confirm_pairing=true;}
                            break;
                        }
                        continue;
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
